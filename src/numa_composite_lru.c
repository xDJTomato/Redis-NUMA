/*
 * NUMA复合LRU策略实现（插槽1默认策略）
 *
 * 双通道迁移决策：
 *   快速通道：访问时写入候选池，serverCron 优先处理
 *   兜底通道：serverCron 每次渐进扫描 key_heat_map 中 scan_batch_size 个 key
 *   执行时始终重读 PREFIX 当前热度，不依赖快照
 */

#define _GNU_SOURCE
#include "numa_composite_lru.h"
#include "zmalloc.h"
#include "numa_bw_monitor.h"
#include "evict.h"        /* numaGetNodePressure() */
#include "numa_key_migrate.h"  /* numa_migrate_single_key() */
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <numa.h>

/* ========== 日志输出 ========== */

#ifdef NUMA_STRATEGY_STANDALONE
#define _serverLog(level, fmt, ...) printf("[%s] " fmt "\n", level, ##__VA_ARGS__)
#else
extern void _serverLog(int level, const char *fmt, ...);
extern struct redisServer server;
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#endif

/* ========== 辅助函数 ========== */

static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static uint16_t get_lru_clock(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint16_t)(tv.tv_sec & 0xFFFF);
}

static uint16_t calculate_time_delta(uint16_t current, uint16_t last) {
    if (current >= last) return current - last;
    return (0xFFFF - last) + current + 1;
}

static uint8_t compute_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < LAZY_DECAY_STEP1_SECS) return 0;
    if (elapsed_secs < LAZY_DECAY_STEP2_SECS) return 1;
    if (elapsed_secs < LAZY_DECAY_STEP3_SECS) return 2;
    if (elapsed_secs < LAZY_DECAY_STEP4_SECS) return 3;
    return COMPOSITE_LRU_HOTNESS_MAX;
}

static int get_current_numa_node(void) {
    if (numa_available() < 0) return 0;
    return numa_node_of_cpu(sched_getcpu());
}

/*
 * compute_target_node - 选择迁移目标节点
 *
 * CXL 场景：热 key 拉回本地 DRAM，冷 key 推到远程 CXL。
 * 策略：
 *   - key 在远程节点 → 拉回本地（current_node），减少远程访问延迟
 *   - key 已在本地   → 无需迁移（返回 -1，跳过）
 */
static int compute_target_node(int mem_node, int current_node) {
    if (mem_node != current_node)
        return current_node;   /* 远程热 key → 拉回本地 */
    return -1;                 /* 本地 key → 不迁移 */
}

/* ========== 热度图字典回调 ========== */

static uint64_t heat_map_hash(const void *key) {
    return (uint64_t)(uintptr_t)key;
}

static int heat_map_key_compare(void *privdata, const void *key1, const void *key2) {
    (void)privdata;
    return key1 == key2;
}

static void heat_map_val_destructor(void *privdata, void *val) {
    (void)privdata;
    zfree(val);
}

static dictType heat_map_dict_type = {
    .hashFunction = heat_map_hash,
    .keyDup = NULL,
    .valDup = NULL,
    .keyCompare = heat_map_key_compare,
    .keyDestructor = NULL,
    .valDestructor = heat_map_val_destructor
};

/* ========== JSON 配置局所辅助 ========== */

/* 去除字符串左右空白 */
static char *trim_spaces(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return s;
}

/*
 * composite_lru_config_defaults - 填充默认配置
 */
void composite_lru_config_defaults(composite_lru_config_t *cfg) {
    if (!cfg) return;
    cfg->decay_threshold_sec       = 10;
    cfg->migrate_hotness_threshold = 3;
    cfg->stability_count           = 3;
    cfg->hot_candidates_size       = 1024;
    cfg->scan_batch_size           = 500;
    cfg->overload_threshold        = 0.8;
    cfg->bandwidth_threshold       = 0.9;
    cfg->pressure_threshold        = 0.7;
    cfg->auto_migrate_enabled      = 1;
}

/*
 * composite_lru_load_config - 从 JSON 文件加载配置
 *
 * 支持顶层扁平 key-value 格式，不依赖外部 JSON 库。
 * 未存在或无法解析的字段保持默认值。
 */
int composite_lru_load_config(const char *path, composite_lru_config_t *out) {
    if (!path || !out) return NUMA_STRATEGY_EINVAL;

    composite_lru_config_defaults(out);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        _serverLog(LL_WARNING, "[Composite LRU] Cannot open config file: %s", path);
        return NUMA_STRATEGY_ERR;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* 忽略注释行和空行 */
        char *p = trim_spaces(line);
        if (*p == '/' || *p == '#' || *p == '{' || *p == '}' || *p == '\0') continue;

        /* 尝试匹配 "key": value 格式 */
        char *colon = strchr(p, ':');
        if (!colon) continue;

        /* 提取 key */
        *colon = '\0';
        char *k = trim_spaces(p);
        /* 去掉引号 */
        if (*k == '"') k++;
        char *ke = strchr(k, '"');
        if (ke) *ke = '\0';

        /* 提取 value */
        char *v = trim_spaces(colon + 1);
        /* 去掉尾部逗号 */
        char *ve = v + strlen(v) - 1;
        while (ve >= v && (*ve == ',' || *ve == ' ' || *ve == '\t')) *ve-- = '\0';

        /* 匹配字段名并设置值 */
        if (strcmp(k, "decay_threshold_sec") == 0) {
            out->decay_threshold_sec = (uint32_t)atoi(v);
        } else if (strcmp(k, "migrate_hotness_threshold") == 0) {
            int t = atoi(v);
            out->migrate_hotness_threshold = (t >= 1 && t <= 7) ? (uint8_t)t : 5;
        } else if (strcmp(k, "stability_count") == 0) {
            out->stability_count = (uint8_t)atoi(v);
        } else if (strcmp(k, "hot_candidates_size") == 0) {
            uint32_t sz = (uint32_t)atoi(v);
            out->hot_candidates_size = (sz > 0) ? sz : 256;
        } else if (strcmp(k, "scan_batch_size") == 0) {
            uint32_t bs = (uint32_t)atoi(v);
            out->scan_batch_size = (bs > 0) ? bs : 200;
        } else if (strcmp(k, "overload_threshold") == 0) {
            out->overload_threshold = atof(v);
        } else if (strcmp(k, "bandwidth_threshold") == 0) {
            out->bandwidth_threshold = atof(v);
        } else if (strcmp(k, "pressure_threshold") == 0) {
            out->pressure_threshold = atof(v);
        } else if (strcmp(k, "auto_migrate_enabled") == 0) {
            out->auto_migrate_enabled = atoi(v);
        } else if (strncmp(k, "max_bandwidth_node", 18) == 0) {
            /* 解析 max_bandwidth_nodeX_mbps, X=节点号 */
            int node_id = atoi(k + 18);  /* "max_bandwidth_node" 后面的数字 */
            double mbps = atof(v);
            if (node_id >= 0 && node_id < NUMA_BW_MAX_NODES && mbps > 0) {
                numa_bw_set_max_bandwidth(node_id, mbps);
                _serverLog(LL_NOTICE,
                    "[Composite LRU] Set node %d max bandwidth: %.0f MB/s", node_id, mbps);
            }
        }
    }

    fclose(fp);
    _serverLog(LL_NOTICE,
        "[Composite LRU] Config loaded: %s (threshold=%d, candidates=%u, scan_batch=%u, auto=%d)",
        path, out->migrate_hotness_threshold, out->hot_candidates_size,
        out->scan_batch_size, out->auto_migrate_enabled);
    return NUMA_STRATEGY_OK;
}

/*
 * composite_lru_apply_config - 将配置应用到运行中的策略实例
 *
 * 如果候选池大小发生变化，重建候选池数组。
 */
int composite_lru_apply_config(numa_strategy_t *strategy, const composite_lru_config_t *cfg) {
    if (!strategy || !strategy->private_data || !cfg) return NUMA_STRATEGY_EINVAL;
    composite_lru_data_t *data = strategy->private_data;

    int rebuild_pool = (cfg->hot_candidates_size != data->config.hot_candidates_size);

    if (rebuild_pool) {
        zfree(data->hot_candidates);
        data->hot_candidates = zcalloc(cfg->hot_candidates_size * sizeof(hot_candidate_t));
        if (!data->hot_candidates) {
            _serverLog(LL_WARNING, "[Composite LRU] apply_config: failed to allocate candidate pool");
            return NUMA_STRATEGY_ERR;
        }
        data->candidates_head  = 0;
        data->candidates_count = 0;
    }

    /* 重置扫描游标，避免使用旧配置的步进次数 */
    if (data->scan_iter) {
        dictReleaseIterator(data->scan_iter);
        data->scan_iter = NULL;
    }

    data->config = *cfg;
    _serverLog(LL_NOTICE,
        "[Composite LRU] Config applied: decay=%us, threshold=%d, pool=%u, batch=%u, auto=%d",
        cfg->decay_threshold_sec, cfg->migrate_hotness_threshold,
        cfg->hot_candidates_size, cfg->scan_batch_size, cfg->auto_migrate_enabled);
    return NUMA_STRATEGY_OK;
}

/* ========== 资源监控 ========== */

/* 检查目标节点的资源状态 */
static int check_resource_status(composite_lru_data_t *data, int node_id) {
    /* 1. 内存过载检查 */
    double pressure = numaGetNodePressure(node_id);
    if (pressure >= data->config.overload_threshold) {
        _serverLog(LL_DEBUG,
            "[Composite LRU] Node %d resource check: OVERLOADED (pressure=%.2f >= %.2f)",
            node_id, pressure, data->config.overload_threshold);
        return RESOURCE_OVERLOADED;
    }

    /* 2. 带宽饱和检查（实时采样数据） */
    double bw_usage = numa_bw_get_usage(node_id);
    if (bw_usage >= data->config.bandwidth_threshold) {
        _serverLog(LL_DEBUG,
            "[Composite LRU] Node %d resource check: BW_SATURATED (bw=%.2f >= %.2f)",
            node_id, bw_usage, data->config.bandwidth_threshold);
        return RESOURCE_BANDWIDTH_SATURATED;
    }

    /* 3. 综合迁移压力检查（内存 60% + 带宽 40%） */
    double combined = pressure * 0.6 + bw_usage * 0.4;
    if (combined >= data->config.pressure_threshold) {
        _serverLog(LL_DEBUG,
            "[Composite LRU] Node %d resource check: MIGRATION_PRESSURE (combined=%.2f >= %.2f)",
            node_id, combined, data->config.pressure_threshold);
        return RESOURCE_MIGRATION_PRESSURE;
    }

    return RESOURCE_AVAILABLE;
}

/* ========== 热度管理 ========== */

/*
 * composite_lru_record_access - 访问路径热度更新
 *
 * 设计原则：只更新热度，不入队。
 * 若本次访问使热度恰好越过阈值，且内存在远程节点，则写入候选池（快速通道）。
 */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val) {
    if (!strategy || !strategy->private_data || !key) return;

    composite_lru_data_t *data = strategy->private_data;
    int current_node = get_current_numa_node();
    uint16_t current_time = get_lru_clock();

    if (val) {
        /* ---- PREFIX 路径（主路径）---- */
        uint8_t hotness = numa_get_hotness(val);
        int mem_node   = numa_get_node_id(val);

        /* 阶梯式惰性衰减：一次性结算上次访问以来的衰减债 */
        uint16_t last_access = numa_get_last_access(val);
        uint16_t elapsed = calculate_time_delta(current_time, last_access);
        uint8_t decay = compute_lazy_decay_steps(elapsed);
        if (decay > 0) {
            uint8_t decayed = (decay >= hotness) ? 0 : (hotness - decay);
            if (decayed != hotness) {
                numa_set_hotness(val, decayed);
                data->decay_operations++;
                hotness = decayed;
            }
        }

        /* 记录衰减前的热度，用于判断是否刚越过阈值 */
        uint8_t hotness_before = hotness;

        /* 任何访问都递增热度（本地/远程均可） */
        if (hotness < COMPOSITE_LRU_HOTNESS_MAX) {
            hotness++;
            numa_set_hotness(val, hotness);
        }

        /* 更新 PREFIX 访问统计 */
        numa_increment_access_count(val);
        numa_set_last_access(val, current_time);
        data->heat_updates++;

        /*
         * 同步写入 key_heat_map：扫描通道依赖此字典发现热 key。
         * 热度 >= 阈值时写入或更新，低于阈值时仅更新已有条目。
         */
        uint8_t thr = data->config.migrate_hotness_threshold;
        {
            composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, key);
            if (info) {
                info->hotness         = hotness;
                info->last_access     = current_time;
                info->access_count++;
                info->current_node    = mem_node;
            } else if (hotness >= thr) {
                info = zmalloc(sizeof(*info));
                if (info) {
                    info->hotness         = hotness;
                    info->stability_counter = 0;
                    info->last_access     = current_time;
                    info->access_count    = 1;
                    info->current_node    = mem_node;
                    info->preferred_node  = -1;
                    dictAdd(data->key_heat_map, key, info);
                }
            }
        }

        /*
         * 快速通道写入条件：
         *   1. 本次访问恰好越过阈值（before < threshold <= after）
         *   2. key 在远程节点（需要拉回本地）
         *
         * target_node = current_node（拉回本地）。
         * 本地 key 不需要入候选池。
         */
        int target = compute_target_node(mem_node, current_node);
        if (target >= 0 && hotness_before < thr && hotness >= thr) {
            uint32_t idx = data->candidates_head % data->config.hot_candidates_size;
            data->hot_candidates[idx].key             = key;
            data->hot_candidates[idx].val             = val;
            data->hot_candidates[idx].target_node     = target;
            data->hot_candidates[idx].hotness_snapshot = hotness;
            data->candidates_head++;
            if (data->candidates_count < data->config.hot_candidates_size)
                data->candidates_count++;
            data->candidates_written++;
            _serverLog(LL_VERBOSE,
                "[Composite LRU] Candidate written: val=%p mem_node=%d cpu_node=%d hotness=%d target=%d",
                val, mem_node, current_node, hotness, target);
        }
    } else {
        /* ---- 字典回退路径（val 为 NULL 时） ---- */
        composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, key);

        if (!info) {
            info = zmalloc(sizeof(*info));
            if (!info) return;
            info->hotness         = 1;
            info->stability_counter = 0;
            info->last_access     = current_time;
            info->access_count    = 1;
            info->current_node    = current_node;
            info->preferred_node  = -1;
            dictAdd(data->key_heat_map, key, info);
            data->heat_updates++;
            return;
        }

        info->access_count++;
        uint16_t old_last = info->last_access;
        info->last_access = current_time;
        data->heat_updates++;

        /* 惰性衰减 */
        uint16_t elapsed = calculate_time_delta(current_time, old_last);
        uint8_t decay = compute_lazy_decay_steps(elapsed);
        if (decay > 0) {
            uint8_t before = info->hotness;
            info->hotness = (decay >= info->hotness) ? 0 : (info->hotness - decay);
            if (info->hotness != before)
                data->decay_operations++;
        }

        uint8_t hotness_before = info->hotness;
        if (info->hotness < COMPOSITE_LRU_HOTNESS_MAX)
            info->hotness++;
        info->stability_counter = 0;

        /* 字典路径候选池写入（热度刚越过阈值且 key 在远程节点） */
        uint8_t thr = data->config.migrate_hotness_threshold;
        int target = compute_target_node(info->current_node, current_node);
        if (target >= 0 && hotness_before < thr && info->hotness >= thr) {
            info->preferred_node = target;
            uint32_t idx = data->candidates_head % data->config.hot_candidates_size;
            data->hot_candidates[idx].key             = key;
            data->hot_candidates[idx].val             = NULL;  /* 字典路径无 val 指针 */
            data->hot_candidates[idx].target_node     = target;
            data->hot_candidates[idx].hotness_snapshot = info->hotness;
            data->candidates_head++;
            if (data->candidates_count < data->config.hot_candidates_size)
                data->candidates_count++;
            data->candidates_written++;
        }
    }
}

/* composite_lru_decay_heat 保留：供外部显式调用（字典路径兜底衰减）*/
void composite_lru_decay_heat(composite_lru_data_t *data) {
    if (!data || !data->key_heat_map) return;

    dictIterator *di = dictGetSafeIterator(data->key_heat_map);
    dictEntry *de;
    uint16_t current_time = get_lru_clock();
    uint16_t decay_thr_sec = (uint16_t)data->config.decay_threshold_sec;

    while ((de = dictNext(di)) != NULL) {
        composite_lru_heat_info_t *info = dictGetVal(de);
        uint16_t elapsed = calculate_time_delta(current_time, info->last_access);
        if (elapsed > decay_thr_sec) {
            info->stability_counter++;
            if (info->stability_counter > data->config.stability_count) {
                if (info->hotness > COMPOSITE_LRU_HOTNESS_MIN) {
                    info->hotness--;
                    data->decay_operations++;
                }
                info->stability_counter = 0;
            }
        } else {
            info->stability_counter = 0;
        }
    }
    dictReleaseIterator(di);
}

/* ========== 渐进扫描（兜底通道）========== */

/*
 * composite_lru_scan_once - 推进一批渐进扫描
 *
 * 每次从 scan_iter 当前位置扫描最多 batch_size 个 key_heat_map 条目。
 * 对热度达到阈值且在远程节点的 key 直接调用 numa_migrate_single_key。
 * 扫描到末尾后将 scan_iter 重置为 NULL，下一次调用时从头开始。
 *
 * @scanned_out : 本次扫描的 key 数（可为 NULL）
 * @migrated_out: 本次触发迁移的 key 数（可为 NULL）
 */
int composite_lru_scan_once(numa_strategy_t *strategy, uint32_t batch_size,
                            uint64_t *scanned_out, uint64_t *migrated_out) {
    if (!strategy || !strategy->private_data) return NUMA_STRATEGY_EINVAL;
    composite_lru_data_t *data = strategy->private_data;

    if (!data->key_heat_map || dictSize(data->key_heat_map) == 0) {
        if (scanned_out)  *scanned_out  = 0;
        if (migrated_out) *migrated_out = 0;
        return NUMA_STRATEGY_OK;
    }

    /* 如果游标为 NULL，从头开始新一轮扫描 */
    if (!data->scan_iter) {
        data->scan_iter = dictGetSafeIterator(data->key_heat_map);
        if (!data->scan_iter) return NUMA_STRATEGY_ERR;
    }

    uint64_t scanned  = 0;
    uint64_t migrated = 0;
    uint8_t  thr = data->config.migrate_hotness_threshold;
    dictEntry *de;

    /* 判断当前节点压力，高压力时启用冷 key 推出 */
    int current_node = get_current_numa_node();
    double local_pressure = numaGetNodePressure(current_node);
    int demote_enabled = (numa_available() >= 0 && numa_max_node() >= 1 &&
                          local_pressure >= data->config.overload_threshold);

    while (scanned < batch_size && (de = dictNext(data->scan_iter)) != NULL) {
        composite_lru_heat_info_t *info = dictGetVal(de);
        scanned++;
        data->scan_keys_checked++;

        /* 路径 A：热 key 拉回本地（preferred_node 已由候选池设置） */
        if (info->hotness >= thr &&
            info->preferred_node >= 0 &&
            info->current_node != info->preferred_node) {

            int status = check_resource_status(data, info->preferred_node);
            if (status == RESOURCE_BANDWIDTH_SATURATED) {
                data->migrations_bw_blocked++;
            }
            if (status == RESOURCE_AVAILABLE) {
                _serverLog(LL_VERBOSE,
                    "[Composite LRU] Scan migrate (hot pull): key=%p node=%d->%d hotness=%d",
                    dictGetKey(de), info->current_node, info->preferred_node, info->hotness);
                int rc = -1;
                if (data->db) {
                    rc = numa_migrate_single_key(data->db, (robj *)dictGetKey(de),
                                                 info->preferred_node);
                }
                if (rc == 0) {
                    info->current_node = info->preferred_node;
                    info->preferred_node = -1;
                    data->migrations_completed++;
                } else {
                    data->migrations_failed++;
                }
                data->migrations_triggered++;
                migrated++;
            }
            continue;
        }

        /* 路径 B：冷 key 推出到远程（本地节点压力高时） */
        if (demote_enabled &&
            info->current_node == current_node &&
            info->hotness < thr) {
            int target = (current_node == 0) ? 1 : 0;
            int status = check_resource_status(data, target);
            if (status == RESOURCE_AVAILABLE) {
                _serverLog(LL_VERBOSE,
                    "[Composite LRU] Scan migrate (cold demote): key=%p node=%d->%d hotness=%d",
                    dictGetKey(de), current_node, target, info->hotness);
                int rc = -1;
                if (data->db) {
                    rc = numa_migrate_single_key(data->db, (robj *)dictGetKey(de), target);
                }
                if (rc == 0) {
                    info->current_node = target;
                    data->migrations_completed++;
                } else {
                    data->migrations_failed++;
                }
                data->migrations_triggered++;
                migrated++;
            }
        }
    }

    /* 若迭代器耗尽，释放并置 NULL，下次从头开始 */
    if (de == NULL) {
        dictReleaseIterator(data->scan_iter);
        data->scan_iter = NULL;
    }

    if (scanned_out)  *scanned_out  = scanned;
    if (migrated_out) *migrated_out = migrated;
    return NUMA_STRATEGY_OK;
}

/* ========== 策略虚函数表实现 ========== */

/* 策略初始化 */
int composite_lru_init(numa_strategy_t *strategy) {
    composite_lru_data_t *data = zmalloc(sizeof(*data));
    if (!data) return NUMA_STRATEGY_ERR;

    memset(data, 0, sizeof(*data));

    /* 加载默认配置 */
    composite_lru_config_defaults(&data->config);

    /* 创建热点候选池（环形缓冲区）*/
    data->hot_candidates = zcalloc(data->config.hot_candidates_size * sizeof(hot_candidate_t));
    if (!data->hot_candidates) {
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }
    data->candidates_head  = 0;
    data->candidates_count = 0;
    data->scan_iter        = NULL;

    /* 创建字典回退路径热度图 */
    data->key_heat_map = dictCreate(&heat_map_dict_type, NULL);
    if (!data->key_heat_map) {
        zfree(data->hot_candidates);
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }

    data->last_decay_time = get_current_time_us();

    /* 设置数据库上下文，供迁移调用使用 */
#ifndef NUMA_STRATEGY_STANDALONE
    data->db = server.db;
#else
    data->db = NULL;
#endif

    strategy->private_data = data;

    _serverLog(LL_NOTICE,
        "[Composite LRU] Strategy initialized: threshold=%d, candidates_size=%u, scan_batch=%u, auto=%d",
        data->config.migrate_hotness_threshold,
        data->config.hot_candidates_size,
        data->config.scan_batch_size,
        data->config.auto_migrate_enabled);
    return NUMA_STRATEGY_OK;
}

/*
 * composite_lru_execute - serverCron 每秒调用
 *
 * 流程：
 *   1. 若 auto_migrate_enabled == 0，直接返回
 *   2. 快速通道：处理候选池，重读 PREFIX 热度，仍满足条件则迁移
 *   3. 兜底通道：渐进扫描 key_heat_map，每次 scan_batch_size 个 key
 */
int composite_lru_execute(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return NUMA_STRATEGY_ERR;
    composite_lru_data_t *data = strategy->private_data;

    /* 自动迁移开关 */
    if (!data->config.auto_migrate_enabled) return NUMA_STRATEGY_OK;

    /* 定期执行日志（每10秒一次）*/
    {
        static uint64_t last_log_time = 0;
        static uint64_t exec_count = 0;
        exec_count++;
        uint64_t now = get_current_time_us();
        if (now - last_log_time > 10000000) {  /* 10秒 */
            _serverLog(LL_VERBOSE,
                "[NUMA Strategy Slot 1] Composite LRU executed "
                "(count: %llu, candidates: %u, heat_updates: %llu, "
                "migrations: %llu, bw_blocked: %llu, "
                "candidates_written: %llu, scan_checked: %llu, heat_map_size: %lu)",
                (unsigned long long)exec_count,
                data->candidates_count,
                (unsigned long long)data->heat_updates,
                (unsigned long long)data->migrations_triggered,
                (unsigned long long)data->migrations_bw_blocked,
                (unsigned long long)data->candidates_written,
                (unsigned long long)data->scan_keys_checked,
                (unsigned long)dictSize(data->key_heat_map));
            last_log_time = now;
        }
    }

    /* ---- 快速通道：处理热点候选池 ---- */
    uint32_t pool_size   = data->config.hot_candidates_size;
    uint32_t count       = data->candidates_count;
    /* 起始槽：最旧的条目（环形缓冲区从 head-count 开始）*/
    uint32_t start_slot  = (count < pool_size)
                           ? 0
                           : (data->candidates_head % pool_size);
    uint32_t processed   = 0;

    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start_slot + i) % pool_size;
        hot_candidate_t *cand = &data->hot_candidates[idx];
        if (!cand->key) continue;

        /* 重新读取 PREFIX 当前热度（不依赖快照）*/
        uint8_t cur_hotness;
        int mem_node;
        if (cand->val) {
            cur_hotness = numa_get_hotness(cand->val);
            mem_node    = numa_get_node_id(cand->val);
        } else {
            /* 字典路径：从 heat_map 重读 */
            composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, cand->key);
            if (!info) { cand->key = NULL; continue; }
            cur_hotness = info->hotness;
            mem_node    = info->current_node;
        }

        /* 带宽感知：源节点繁忙时降低迁移门槛 */
        int effective_threshold = data->config.migrate_hotness_threshold;
        double src_bw = numa_bw_get_usage(mem_node);  /* mem_node = key当前所在节点 */
        if (src_bw > 0.7 && effective_threshold > 1) {
            effective_threshold -= 1;
            _serverLog(LL_DEBUG,
                "[Composite LRU] Source node %d bw=%.2f > 0.7, lowering threshold to %d",
                mem_node, src_bw, effective_threshold);
        }

        if (cur_hotness >= effective_threshold && mem_node != cand->target_node) {
            int status = check_resource_status(data, cand->target_node);
            if (status == RESOURCE_BANDWIDTH_SATURATED) {
                data->migrations_bw_blocked++;
            }
            if (status == RESOURCE_AVAILABLE) {
                _serverLog(LL_VERBOSE,
                    "[Composite LRU] Fast-path migrate: key=%p node=%d->%d hotness=%d",
                    cand->key, mem_node, cand->target_node, cur_hotness);
                if (data->db && cand->key) {
                    int rc = numa_migrate_single_key(data->db, (robj *)cand->key,
                                                     cand->target_node);
                    if (rc == 0) {
                        data->migrations_completed++;
                        _serverLog(LL_VERBOSE,
                            "[Composite LRU] Fast-path migrated (actual): key=%p node=%d->%d",
                            cand->key, mem_node, cand->target_node);
                    } else {
                        data->migrations_failed++;
                        _serverLog(LL_DEBUG,
                            "[Composite LRU] Fast-path migrate failed: key=%p rc=%d",
                            cand->key, rc);
                    }
                }
                data->migrations_triggered++;
                processed++;
            }
        }
        /* 清空已处理槽位 */
        cand->key = NULL;
        cand->val = NULL;
    }
    /* 处理后重置候选池计数 */
    if (processed > 0 || count > 0) {
        data->candidates_count = 0;
        data->candidates_head  = 0;
    }

    /* ---- 兜底通道：渐进扫描 key_heat_map ---- */
    composite_lru_scan_once(strategy, data->config.scan_batch_size, NULL, NULL);

    return NUMA_STRATEGY_OK;
}

/* 策略清理 */
void composite_lru_cleanup(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return;
    composite_lru_data_t *data = strategy->private_data;

    _serverLog(LL_NOTICE,
        "[Composite LRU] Cleanup: heat_updates=%llu, migrations=%llu, decays=%llu, candidates=%llu, scan_checked=%llu",
        (unsigned long long)data->heat_updates,
        (unsigned long long)data->migrations_triggered,
        (unsigned long long)data->decay_operations,
        (unsigned long long)data->candidates_written,
        (unsigned long long)data->scan_keys_checked);

    if (data->scan_iter) {
        dictReleaseIterator(data->scan_iter);
        data->scan_iter = NULL;
    }
    if (data->hot_candidates) {
        zfree(data->hot_candidates);
        data->hot_candidates = NULL;
    }
    if (data->key_heat_map) {
        dictRelease(data->key_heat_map);
        data->key_heat_map = NULL;
    }

    zfree(data);
    strategy->private_data = NULL;
}

/* 获取策略名称 */
static const char* composite_lru_get_name(numa_strategy_t *strategy) {
    (void)strategy;
    return "composite-lru";
}

/* 获取策略描述 */
static const char* composite_lru_get_description(numa_strategy_t *strategy) {
    (void)strategy;
    return "插槽1默认策略：稳定性优先的复合LRU热度管理";
}

/* 设置配置参数（兼容旧接口，内部转发到 config 结构体）*/
static int composite_lru_set_config(numa_strategy_t *strategy,
                                    const char *key, const char *value) {
    if (!strategy || !strategy->private_data || !key || !value)
        return NUMA_STRATEGY_EINVAL;

    composite_lru_data_t *data = strategy->private_data;

    if (strcmp(key, "decay_threshold") == 0 ||
        strcmp(key, "decay_threshold_sec") == 0) {
        data->config.decay_threshold_sec = (uint32_t)atoi(value);
    } else if (strcmp(key, "stability_count") == 0) {
        data->config.stability_count = (uint8_t)atoi(value);
    } else if (strcmp(key, "migrate_threshold") == 0 ||
               strcmp(key, "migrate_hotness_threshold") == 0) {
        int t = atoi(value);
        data->config.migrate_hotness_threshold = (t >= 1 && t <= 7) ? (uint8_t)t : 5;
    } else if (strcmp(key, "overload_threshold") == 0) {
        data->config.overload_threshold = atof(value);
    } else if (strcmp(key, "bandwidth_threshold") == 0) {
        data->config.bandwidth_threshold = atof(value);
    } else if (strcmp(key, "pressure_threshold") == 0) {
        data->config.pressure_threshold = atof(value);
    } else if (strcmp(key, "hot_candidates_size") == 0) {
        uint32_t sz = (uint32_t)atoi(value);
        if (sz > 0 && sz != data->config.hot_candidates_size) {
            /* 重建候选池 */
            composite_lru_config_t newcfg = data->config;
            newcfg.hot_candidates_size = sz;
            composite_lru_apply_config(strategy, &newcfg);
        }
    } else if (strcmp(key, "scan_batch_size") == 0) {
        uint32_t bs = (uint32_t)atoi(value);
        if (bs > 0) data->config.scan_batch_size = bs;
    } else if (strcmp(key, "auto_migrate_enabled") == 0) {
        data->config.auto_migrate_enabled = atoi(value);
    } else {
        return NUMA_STRATEGY_EINVAL;
    }

    _serverLog(LL_VERBOSE, "[Composite LRU] Config set: %s = %s", key, value);
    return NUMA_STRATEGY_OK;
}

/* 获取配置参数 */
static int composite_lru_get_config(numa_strategy_t *strategy,
                                    const char *key, char *buf, size_t buf_len) {
    if (!strategy || !strategy->private_data || !key || !buf || buf_len == 0)
        return NUMA_STRATEGY_EINVAL;

    composite_lru_data_t *data = strategy->private_data;

    if (strcmp(key, "decay_threshold") == 0 ||
        strcmp(key, "decay_threshold_sec") == 0) {
        snprintf(buf, buf_len, "%u", data->config.decay_threshold_sec);
    } else if (strcmp(key, "stability_count") == 0) {
        snprintf(buf, buf_len, "%u", data->config.stability_count);
    } else if (strcmp(key, "migrate_threshold") == 0 ||
               strcmp(key, "migrate_hotness_threshold") == 0) {
        snprintf(buf, buf_len, "%u", data->config.migrate_hotness_threshold);
    } else if (strcmp(key, "overload_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->config.overload_threshold);
    } else if (strcmp(key, "bandwidth_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->config.bandwidth_threshold);
    } else if (strcmp(key, "pressure_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->config.pressure_threshold);
    } else if (strcmp(key, "hot_candidates_size") == 0) {
        snprintf(buf, buf_len, "%u", data->config.hot_candidates_size);
    } else if (strcmp(key, "scan_batch_size") == 0) {
        snprintf(buf, buf_len, "%u", data->config.scan_batch_size);
    } else if (strcmp(key, "auto_migrate_enabled") == 0) {
        snprintf(buf, buf_len, "%d", data->config.auto_migrate_enabled);
    } else if (strcmp(key, "heat_updates") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->heat_updates);
    } else if (strcmp(key, "migrations_triggered") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->migrations_triggered);
    } else if (strcmp(key, "decay_operations") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->decay_operations);
    } else if (strcmp(key, "candidates_written") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->candidates_written);
    } else if (strcmp(key, "scan_keys_checked") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->scan_keys_checked);
    } else {
        return NUMA_STRATEGY_EINVAL;
    }

    return NUMA_STRATEGY_OK;
}

/* 策略虚函数表 */
static const numa_strategy_vtable_t composite_lru_vtable = {
    .init = composite_lru_init,
    .execute = composite_lru_execute,
    .cleanup = composite_lru_cleanup,
    .get_name = composite_lru_get_name,
    .get_description = composite_lru_get_description,
    .set_config = composite_lru_set_config,
    .get_config = composite_lru_get_config
};

/* ========== 工厂函数 ========== */

/* 创建策略实例 */
numa_strategy_t* composite_lru_create(void) {
    numa_strategy_t *strategy = zmalloc(sizeof(*strategy));
    if (!strategy) return NULL;
    
    memset(strategy, 0, sizeof(*strategy));
    strategy->slot_id = 1;  /* 默认使用插槽1 */
    strategy->name = "composite-lru";
    strategy->description = "Stability-first composite LRU strategy (slot 1 default)";
    strategy->type = STRATEGY_TYPE_PERIODIC;
    strategy->priority = STRATEGY_PRIORITY_HIGH;
    strategy->enabled = 1;
    strategy->execute_interval_us = 1000000;  /* 1秒 */
    strategy->vtable = &composite_lru_vtable;
    
    return strategy;
}

/* 销毁策略实例 */
void composite_lru_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    
    if (strategy->vtable && strategy->vtable->cleanup) {
        strategy->vtable->cleanup(strategy);
    }
    
    zfree(strategy);
}

/* 策略工厂 */
static numa_strategy_factory_t composite_lru_factory = {
    .name = "composite-lru",
    .description = "Stability-first composite LRU hotness management (slot 1 default)",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,  /* 1秒 */
    .create = composite_lru_create,
    .destroy = composite_lru_destroy
};

/* ========== 公共注册接口 ========== */

/* 注册策略工厂 */
int numa_composite_lru_register(void) {
    return numa_strategy_register_factory(&composite_lru_factory);
}

/* ========== 统计信息查询 ========== */

void composite_lru_get_stats(numa_strategy_t *strategy, 
                             uint64_t *heat_updates,
                             uint64_t *migrations_triggered,
                             uint64_t *decay_operations) {
    if (!strategy || !strategy->private_data) return;
    
    composite_lru_data_t *data = strategy->private_data;
    
    if (heat_updates) *heat_updates = data->heat_updates;
    if (migrations_triggered) *migrations_triggered = data->migrations_triggered;
    if (decay_operations) *decay_operations = data->decay_operations;
}
