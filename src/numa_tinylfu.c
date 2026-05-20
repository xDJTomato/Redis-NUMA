/* numa_tinylfu.c - TinyLFU 热点数据快速发现与迁移策略（插槽 2）
 *
 * 算法来源: Caffeine (Ben Manes) 的 Window-TinyLFU
 *
 * 数据结构:
 *   1. Count-Min Sketch (4 行 × 16384 列, 4-bit 计数器)  = 32 KB
 *   2. Doorkeeper Bloom Filter (65536 位)                 =  8 KB
 *   3. 迁移候选环形缓冲区 (512 条目)                      ≈ 16 KB
 *   总计: ~56 KB 固定内存, 与 key 数量无关
 *
 * 热路径开销: 1 次 SipHash + 4 次位操作 (Doorkeeper) + 4 次数组读取 (CMS)
 */

#ifdef HAVE_NUMA

#define _GNU_SOURCE
#include "numa_tinylfu.h"
#include "numa_strategy_slots.h"
#include "numa_key_migrate.h"
#include "zmalloc.h"
#include "dict.h"
#include "sds.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <numa.h>
#include <sched.h>

extern void _serverLog(int level, const char *fmt, ...);
extern uint64_t dictGenHashFunction(const void *key, int len);

#define LL_DEBUG   0
#define LL_VERBOSE 1
#define LL_NOTICE  2
#define LL_WARNING 3

#define TLFU_LOG(level, ...) _serverLog(level, __VA_ARGS__)

/* 主线程 NUMA 节点（与 composite_lru 共享） */
static int g_main_thread_node = 0;

/* ========== CMS 4-bit packed 操作 ========== */

static inline uint8_t cms_get(const uint8_t *row, uint32_t col) {
    uint8_t byte = row[col >> 1];
    return (col & 1) ? (byte >> 4) : (byte & 0x0F);
}

static inline void cms_set(uint8_t *row, uint32_t col, uint8_t val) {
    uint32_t idx = col >> 1;
    if (col & 1)
        row[idx] = (row[idx] & 0x0F) | (val << 4);
    else
        row[idx] = (row[idx] & 0xF0) | (val & 0x0F);
}

static inline int cms_increment(uint8_t *row, uint32_t col) {
    uint8_t v = cms_get(row, col);
    if (v < TINYLFU_COUNTER_MAX) {
        cms_set(row, col, v + 1);
        return 1;
    }
    return 0;
}

/* 从 64-bit hash 中导出各行列索引（Caffeine 风格：分段取 16 位） */
static inline uint32_t cms_index(uint64_t hash, int row, uint32_t mask) {
    uint32_t h = (uint32_t)(hash >> (row * 16));
    return h & mask;
}

/* ========== CMS 高层操作 ========== */

static int cms_alloc(tinylfu_cms_t *cms, uint32_t width) {
    cms->width = width;
    cms->width_mask = width - 1;
    cms->bytes_per_row = width >> 1;
    for (int i = 0; i < TINYLFU_CMS_DEPTH; i++) {
        cms->rows[i] = zcalloc(cms->bytes_per_row);
        if (!cms->rows[i]) return -1;
    }
    return 0;
}

static void cms_free(tinylfu_cms_t *cms) {
    for (int i = 0; i < TINYLFU_CMS_DEPTH; i++) {
        if (cms->rows[i]) { zfree(cms->rows[i]); cms->rows[i] = NULL; }
    }
}

static void cms_record(tinylfu_cms_t *cms, uint64_t hash) {
    for (int i = 0; i < TINYLFU_CMS_DEPTH; i++)
        cms_increment(cms->rows[i], cms_index(hash, i, cms->width_mask));
}

static uint8_t cms_estimate(const tinylfu_cms_t *cms, uint64_t hash) {
    uint8_t min_val = TINYLFU_COUNTER_MAX;
    for (int i = 0; i < TINYLFU_CMS_DEPTH; i++) {
        uint8_t v = cms_get(cms->rows[i], cms_index(hash, i, cms->width_mask));
        if (v < min_val) min_val = v;
    }
    return min_val;
}

/* 全局减半: (byte >> 1) & 0x77 同时对两个 4-bit nibble 右移 */
static void cms_halve(tinylfu_cms_t *cms) {
    for (int i = 0; i < TINYLFU_CMS_DEPTH; i++) {
        uint8_t *row = cms->rows[i];
        uint32_t n = cms->bytes_per_row;
        for (uint32_t j = 0; j < n; j++)
            row[j] = (row[j] >> 1) & 0x77;
    }
}

/* ========== Doorkeeper Bloom Filter ========== */

static int dk_alloc(tinylfu_doorkeeper_t *dk, uint32_t cms_width) {
    dk->num_bits = TINYLFU_CMS_DEPTH * cms_width;
    dk->num_bytes = (dk->num_bits + 7) >> 3;
    dk->bits = zcalloc(dk->num_bytes);
    return dk->bits ? 0 : -1;
}

static void dk_free(tinylfu_doorkeeper_t *dk) {
    if (dk->bits) { zfree(dk->bits); dk->bits = NULL; }
}

static void dk_clear(tinylfu_doorkeeper_t *dk) {
    memset(dk->bits, 0, dk->num_bytes);
}

static inline int dk_test(const tinylfu_doorkeeper_t *dk, uint64_t hash) {
    uint32_t mask = dk->num_bits - 1;
    uint32_t h1 = (uint32_t)(hash) & mask;
    uint32_t h2 = (uint32_t)(hash >> 32) & mask;
    return (dk->bits[h1 >> 3] & (1u << (h1 & 7))) &&
           (dk->bits[h2 >> 3] & (1u << (h2 & 7)));
}

static inline void dk_add(tinylfu_doorkeeper_t *dk, uint64_t hash) {
    uint32_t mask = dk->num_bits - 1;
    uint32_t h1 = (uint32_t)(hash) & mask;
    uint32_t h2 = (uint32_t)(hash >> 32) & mask;
    dk->bits[h1 >> 3] |= (1u << (h1 & 7));
    dk->bits[h2 >> 3] |= (1u << (h2 & 7));
}

/* ========== 环形缓冲区 ========== */

static int ring_alloc(tinylfu_data_t *d) {
    d->ring = zcalloc(sizeof(tinylfu_candidate_t) * d->config.ring_size);
    d->ring_head = 0;
    d->ring_count = 0;
    return d->ring ? 0 : -1;
}

static void ring_free(tinylfu_data_t *d) {
    if (!d->ring) return;
    for (uint32_t i = 0; i < d->config.ring_size; i++) {
        if (d->ring[i].key) { sdsfree(d->ring[i].key); d->ring[i].key = NULL; }
    }
    zfree(d->ring);
    d->ring = NULL;
}

static void ring_push(tinylfu_data_t *d, sds key, void *val, void *data_ptr,
                       int target_node, uint8_t freq) {
    uint32_t slot = d->ring_head % d->config.ring_size;
    if (d->ring[slot].key) sdsfree(d->ring[slot].key);
    d->ring[slot].key = sdsdup(key);
    d->ring[slot].val = val;
    d->ring[slot].data_ptr = data_ptr;
    d->ring[slot].target_node = target_node;
    d->ring[slot].freq_snapshot = freq;
    d->ring_head++;
    if (d->ring_count < d->config.ring_size) d->ring_count++;
}

/* ========== 访问记录（热路径，在 lookupKey 中调用） ========== */

void tinylfu_record_access(numa_strategy_t *strategy, void *key_sds,
                           void *val, void *data_ptr) {
    tinylfu_data_t *d = strategy->private_data;
    if (!d) return;

    d->stat_accesses++;

    /* 所有访问都记录 locality 统计 */
    if (data_ptr) {
        int data_node = numa_get_node_id(data_ptr);
        if (data_node >= 0) {
            if (data_node == 0) d->stat_accesses_node0++;
            else if (data_node == 1) d->stat_accesses_node1++;
            else if (data_node == 2) d->stat_accesses_node2++;
            else if (data_node == 3) d->stat_accesses_node3++;

            if (data_node == g_main_thread_node)
                d->stat_accesses_local++;
            else
                d->stat_accesses_remote++;
        } else {
            d->stat_accesses_unknown++;
        }
    } else {
        d->stat_accesses_unknown++;
    }

    sds keyname = (sds)key_sds;
    uint64_t hash = dictGenHashFunction(keyname, sdslen(keyname));

    /* Doorkeeper: 过滤一次性访问 */
    if (!dk_test(&d->doorkeeper, hash)) {
        dk_add(&d->doorkeeper, hash);
        d->stat_doorkeeper_filtered++;
        goto decay_check;
    }

    /* 已通过 doorkeeper, 递增 CMS */
    cms_record(&d->cms, hash);

    /* 估计频率 (+1 因为 doorkeeper 吸收了第一次) */
    uint8_t freq = cms_estimate(&d->cms, hash);
    if (freq < TINYLFU_COUNTER_MAX) freq++;

    /* 检查迁移条件 */
    if (freq >= d->config.migrate_threshold && data_ptr) {
        int data_node = numa_get_node_id(data_ptr);
        if (data_node >= 0 && data_node != g_main_thread_node) {
            ring_push(d, keyname, val, data_ptr,
                      g_main_thread_node, freq);
            d->stat_candidates_enqueued++;
        }
    }

decay_check:
    d->total_ops++;
    if (d->total_ops >= d->config.reset_interval) {
        cms_halve(&d->cms);
        dk_clear(&d->doorkeeper);
        d->total_ops = 0;
        d->stat_resets++;
    }
}

/* ========== 策略执行（serverCron 每秒调用） ========== */

int tinylfu_execute(numa_strategy_t *strategy) {
    tinylfu_data_t *d = strategy->private_data;
    if (!d || !d->config.auto_migrate_enabled || !d->db) return 0;
    if (d->ring_count == 0) return 0;

    uint32_t budget = d->config.migration_budget;
    uint32_t migrated = 0;
    uint32_t processed = 0;

    /* 从最旧的候选开始处理 */
    uint32_t size = d->config.ring_size;
    uint32_t start;
    if (d->ring_count >= size)
        start = d->ring_head % size;
    else
        start = (d->ring_head - d->ring_count + size) % size;

    uint32_t count = d->ring_count;

    for (uint32_t i = 0; i < count && migrated < budget; i++) {
        uint32_t idx = (start + i) % size;
        tinylfu_candidate_t *c = &d->ring[idx];
        if (!c->key) continue;

        processed++;

        /* 重新估计频率以确认仍然是热点 */
        uint64_t hash = dictGenHashFunction(c->key, sdslen(c->key));
        uint8_t current_freq = cms_estimate(&d->cms, hash);
        if (dk_test(&d->doorkeeper, hash) && current_freq < TINYLFU_COUNTER_MAX)
            current_freq++;

        if (current_freq >= d->config.migrate_threshold) {
            int ret = numa_migrate_key_by_name(d->db, c->key, c->target_node);
            if (ret == 0) {
                migrated++;
                d->stat_migrations_done++;
            } else {
                d->stat_migrations_failed++;
            }
        }

        sdsfree(c->key);
        c->key = NULL;
        c->val = NULL;
        c->data_ptr = NULL;
    }

    d->ring_count = 0;
    d->ring_head = 0;

    if (migrated > 0 && d->config.debug_logging_enabled) {
        TLFU_LOG(LL_NOTICE,
            "[TinyLFU] cycle: processed=%u migrated=%u failed=%lu",
            processed, migrated, (unsigned long)d->stat_migrations_failed);
    }

    return (int)migrated;
}

/* ========== vtable 实现 ========== */

int tinylfu_init(numa_strategy_t *strategy) {
    tinylfu_data_t *d = zcalloc(sizeof(tinylfu_data_t));
    if (!d) return NUMA_STRATEGY_ERR;

    /* 主线程 NUMA 节点由 server main() 显式锁定，避免初始化时 CPU 漂移。 */

    /* 默认配置 */
    d->config.cms_width = TINYLFU_CMS_WIDTH_DEFAULT;
    d->config.migrate_threshold = TINYLFU_DEFAULT_MIGRATE_THRESHOLD;
    d->config.reset_interval = TINYLFU_DEFAULT_RESET_INTERVAL;
    d->config.ring_size = TINYLFU_DEFAULT_RING_SIZE;
    d->config.migration_budget = TINYLFU_DEFAULT_MIGRATION_BUDGET;
    d->config.auto_migrate_enabled = 1;
    d->config.debug_logging_enabled = 0;

    if (cms_alloc(&d->cms, d->config.cms_width) < 0) goto err;
    if (dk_alloc(&d->doorkeeper, d->config.cms_width) < 0) goto err;
    if (ring_alloc(d) < 0) goto err;

    strategy->private_data = d;

    TLFU_LOG(LL_NOTICE,
        "[TinyLFU] initialized: CMS %ux%u (%.1fKB), doorkeeper %.1fKB, "
        "ring %u, threshold %u, reset every %u ops",
        TINYLFU_CMS_DEPTH, d->config.cms_width,
        (double)(TINYLFU_CMS_DEPTH * d->cms.bytes_per_row) / 1024.0,
        (double)d->doorkeeper.num_bytes / 1024.0,
        d->config.ring_size, d->config.migrate_threshold,
        d->config.reset_interval);

    return NUMA_STRATEGY_OK;

err:
    cms_free(&d->cms);
    dk_free(&d->doorkeeper);
    ring_free(d);
    zfree(d);
    return NUMA_STRATEGY_ERR;
}

void tinylfu_cleanup(numa_strategy_t *strategy) {
    tinylfu_data_t *d = strategy->private_data;
    if (!d) return;
    cms_free(&d->cms);
    dk_free(&d->doorkeeper);
    ring_free(d);
    zfree(d);
    strategy->private_data = NULL;
}

static const char* tinylfu_get_name(numa_strategy_t *strategy) {
    (void)strategy;
    return "tinylfu";
}

static const char* tinylfu_get_description(numa_strategy_t *strategy) {
    (void)strategy;
    return "TinyLFU frequency-based hot data migration (CMS + Doorkeeper)";
}

static int tinylfu_set_config(numa_strategy_t *strategy,
                              const char *key, const char *value) {
    tinylfu_data_t *d = strategy->private_data;
    if (!d) return NUMA_STRATEGY_ERR;

    if (!strcmp(key, "migrate_threshold")) {
        int v = atoi(value);
        if (v >= 1 && v <= 15) { d->config.migrate_threshold = v; return 0; }
    } else if (!strcmp(key, "reset_interval")) {
        int v = atoi(value);
        if (v >= 1000) { d->config.reset_interval = v; return 0; }
    } else if (!strcmp(key, "migration_budget")) {
        int v = atoi(value);
        if (v >= 1) { d->config.migration_budget = v; return 0; }
    } else if (!strcmp(key, "auto_migrate_enabled")) {
        d->config.auto_migrate_enabled = atoi(value) ? 1 : 0;
        return 0;
    } else if (!strcmp(key, "debug_logging_enabled")) {
        d->config.debug_logging_enabled = atoi(value) ? 1 : 0;
        return 0;
    }
    return NUMA_STRATEGY_EINVAL;
}

static int tinylfu_get_config(numa_strategy_t *strategy,
                              const char *key, char *buf, size_t buf_len) {
    tinylfu_data_t *d = strategy->private_data;
    if (!d) return NUMA_STRATEGY_ERR;

    if (!strcmp(key, "migrate_threshold"))
        snprintf(buf, buf_len, "%u", d->config.migrate_threshold);
    else if (!strcmp(key, "reset_interval"))
        snprintf(buf, buf_len, "%u", d->config.reset_interval);
    else if (!strcmp(key, "migration_budget"))
        snprintf(buf, buf_len, "%u", d->config.migration_budget);
    else if (!strcmp(key, "auto_migrate_enabled"))
        snprintf(buf, buf_len, "%d", d->config.auto_migrate_enabled);
    else if (!strcmp(key, "cms_width"))
        snprintf(buf, buf_len, "%u", d->config.cms_width);
    else if (!strcmp(key, "ring_size"))
        snprintf(buf, buf_len, "%u", d->config.ring_size);
    else if (!strcmp(key, "total_ops"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->total_ops);
    else if (!strcmp(key, "stat_accesses"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses);
    else if (!strcmp(key, "stat_doorkeeper_filtered"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_doorkeeper_filtered);
    else if (!strcmp(key, "stat_candidates_enqueued"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_candidates_enqueued);
    else if (!strcmp(key, "stat_migrations_done"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_migrations_done);
    else if (!strcmp(key, "stat_migrations_failed"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_migrations_failed);
    else if (!strcmp(key, "stat_resets"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_resets);
    else if (!strcmp(key, "stat_accesses_local"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses_local);
    else if (!strcmp(key, "stat_accesses_remote"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses_remote);
    else if (!strcmp(key, "stat_accesses_node0"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses_node0);
    else if (!strcmp(key, "stat_accesses_node1"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses_node1);
    else if (!strcmp(key, "stat_accesses_node2"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses_node2);
    else if (!strcmp(key, "stat_accesses_node3"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses_node3);
    else if (!strcmp(key, "stat_accesses_unknown"))
        snprintf(buf, buf_len, "%llu", (unsigned long long)d->stat_accesses_unknown);
    else
        return NUMA_STRATEGY_EINVAL;

    return NUMA_STRATEGY_OK;
}

/* ========== vtable ========== */

static const numa_strategy_vtable_t tinylfu_vtable = {
    .init = tinylfu_init,
    .execute = tinylfu_execute,
    .cleanup = tinylfu_cleanup,
    .get_name = tinylfu_get_name,
    .get_description = tinylfu_get_description,
    .set_config = tinylfu_set_config,
    .get_config = tinylfu_get_config,
};

/* ========== 工厂 ========== */

numa_strategy_t* tinylfu_create(void) {
    numa_strategy_t *s = zcalloc(sizeof(numa_strategy_t));
    if (!s) return NULL;
    s->vtable = &tinylfu_vtable;
    s->type = STRATEGY_TYPE_PERIODIC;
    s->priority = STRATEGY_PRIORITY_HIGH;
    s->execute_interval_us = 1000000; /* 1 秒 */
    s->enabled = 1;
    s->name = "tinylfu";
    s->description = "TinyLFU frequency-based hot data migration";
    return s;
}

void tinylfu_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    if (strategy->vtable && strategy->vtable->cleanup)
        strategy->vtable->cleanup(strategy);
    zfree(strategy);
}

static const numa_strategy_factory_t tinylfu_factory = {
    .name = "tinylfu",
    .description = "TinyLFU frequency-based hot data migration (CMS + Doorkeeper)",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,
    .create = tinylfu_create,
    .destroy = tinylfu_destroy,
};

int numa_tinylfu_register(void) {
    return numa_strategy_register_factory(&tinylfu_factory);
}

/* ========== 主线程节点设置 ========== */

void tinylfu_set_main_thread_node(int node) {
    g_main_thread_node = node;
}

#endif /* HAVE_NUMA */
