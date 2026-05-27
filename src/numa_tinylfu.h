/* numa_tinylfu.h - TinyLFU 热点数据快速发现与迁移策略（插槽 2）
 *
 * 基于 Caffeine 的 TinyLFU 算法，使用 Count-Min Sketch + Doorkeeper Bloom Filter
 * 以极低的内存开销（~50KB）实现 O(1) 的访问频率估计和热点数据发现。
 *
 * 与 Composite LRU (Slot 1) 的核心区别：
 *   - 频率而非热度：CMS 直接计数访问次数，无需阶梯衰减
 *   - 全局衰减：周期性将所有计数器减半（而非每键惰性衰减）
 *   - Doorkeeper：过滤一次性访问的 key，避免计数器污染
 *   - 内存固定：CMS + Bloom Filter 大小固定，与 key 数量无关
 */

#ifndef NUMA_TINYLFU_H
#define NUMA_TINYLFU_H

#include "sds.h"
#include "numa_strategy_slots.h"
#include <stdint.h>

typedef struct redisDb redisDb;

/* ========== Count-Min Sketch 参数 ========== */
#define TINYLFU_CMS_DEPTH       4
#define TINYLFU_CMS_WIDTH_DEFAULT  16384   /* 必须是 2 的幂 */
#define TINYLFU_COUNTER_MAX     15         /* 4-bit 计数器上限 */

/* ========== Doorkeeper Bloom Filter 参数 ========== */
#define TINYLFU_DK_HASH_COUNT   2

/* ========== 默认配置 ========== */
#define TINYLFU_DEFAULT_MIGRATE_THRESHOLD   2
#define TINYLFU_DEFAULT_RESET_INTERVAL      50000
#define TINYLFU_DEFAULT_RING_SIZE           1024
#define TINYLFU_DEFAULT_MIGRATION_BUDGET    512

/* ========== Count-Min Sketch ========== */
typedef struct {
    uint8_t *rows[TINYLFU_CMS_DEPTH]; /* 4-bit packed: 2 counters/byte */
    uint32_t width;                    /* 列数 (2 的幂) */
    uint32_t width_mask;               /* width - 1, 快速取模 */
    uint32_t bytes_per_row;            /* width / 2 */
} tinylfu_cms_t;

/* ========== Doorkeeper Bloom Filter ========== */
typedef struct {
    uint8_t *bits;
    uint32_t num_bits;                 /* 位数 = CMS_DEPTH * width */
    uint32_t num_bytes;
} tinylfu_doorkeeper_t;

/* ========== 迁移候选条目 ========== */
typedef struct {
    sds      key;
    void    *val;
    void    *data_ptr;
    int      target_node;
    uint8_t  freq_snapshot;
    uint32_t cost_units;
} tinylfu_candidate_t;

/* ========== 可配置参数 ========== */
typedef struct {
    uint32_t cms_width;                /* CMS 列数 */
    uint8_t  migrate_threshold;        /* 触发迁移的最低频率 */
    uint32_t reset_interval;           /* 每隔多少次访问执行一次全局衰减 */
    uint32_t ring_size;                /* 候选环形缓冲区大小 */
    uint32_t migration_budget;         /* 每次 serverCron 最多迁移数 */
    int      auto_migrate_enabled;     /* 1=开启自动迁移 */
    int      debug_logging_enabled;
} tinylfu_config_t;

/* ========== 策略私有数据 ========== */
typedef struct {
    redisDb *db;

    tinylfu_config_t config;

    /* Count-Min Sketch */
    tinylfu_cms_t cms;

    /* Doorkeeper Bloom Filter */
    tinylfu_doorkeeper_t doorkeeper;

    /* 全局操作计数（触发衰减） */
    uint64_t total_ops;

    /* 候选环形缓冲区 */
    tinylfu_candidate_t *ring;
    uint32_t ring_head;
    uint32_t ring_tail;
    uint32_t ring_count;

    /* 统计 */
    uint64_t stat_accesses;            /* 总访问次数 */
    uint64_t stat_doorkeeper_filtered; /* 被 doorkeeper 过滤（一次性访问） */
    uint64_t stat_candidates_enqueued; /* 入队候选数 */
    uint64_t stat_migrations_done;     /* 完成迁移数 */
    uint64_t stat_migrations_failed;   /* 迁移失败数 */
    uint64_t stat_resets;              /* 全局衰减次数 */
    uint64_t stat_accesses_local;
    uint64_t stat_accesses_remote;
    uint64_t stat_accesses_node0;
    uint64_t stat_accesses_node1;
    uint64_t stat_accesses_node2;
    uint64_t stat_accesses_node3;
    uint64_t stat_accesses_unknown;
} tinylfu_data_t;

/* ========== 公共接口 ========== */

/* 向策略管理器注册工厂 */
int numa_tinylfu_register(void);

/* 访问记录（在 lookupKey 中调用） */
void tinylfu_record_access(numa_strategy_t *strategy, void *key,
                           void *val, void *data_ptr);

/* 策略工厂函数 */
numa_strategy_t* tinylfu_create(void);
void tinylfu_destroy(numa_strategy_t *strategy);

/* 设置主线程 NUMA 节点（在 main() 中调用） */
void tinylfu_set_main_thread_node(int node);

/* vtable 实现 */
int  tinylfu_init(numa_strategy_t *strategy);
int  tinylfu_execute(numa_strategy_t *strategy);
int  tinylfu_execute_step(numa_strategy_t *strategy, uint64_t deadline_us, uint32_t budget);
void tinylfu_cleanup(numa_strategy_t *strategy);

#endif /* NUMA_TINYLFU_H */
