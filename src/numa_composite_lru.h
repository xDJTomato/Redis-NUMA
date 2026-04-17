/* numa_composite_lru.h - NUMA复合LRU策略（插槽1，默认策略）
 *
 * 本模块实现基于Redis原生LRU机制的默认NUMA迁移策略，
 * 提供热点候选池 + 渐进式扫描的双通道迁移决策机制：
 *   - 访问路径（record_access）：只更新 PREFIX 热度，无入队开销
 *   - 候选池（快速通道）：访问时热度首次越过阈值写入环形缓冲区，serverCron 优先处理
 *   - 渐进扫描（兜底通道）：serverCron 每次扫描 scan_batch_size 个 key，游标推进
 *   - 执行时始终重读 PREFIX 当前热度，不依赖快照
 * 所有参数可通过 JSON 配置文件指定，运行时通过 NUMAMIGRATE CONFIG LOAD 热更新。
 */

#ifndef NUMA_COMPOSITE_LRU_H
#define NUMA_COMPOSITE_LRU_H

#include "numa_strategy_slots.h"
#include "dict.h"
#include <stdint.h>

/* 前向声明，避免引入 server.h 造成循环依赖 */
typedef struct redisDb redisDb;

/* ========== 阶梯衰减常量（不随 JSON 配置变化）========== */
#define LAZY_DECAY_STEP1_SECS    10    /* 空闲 < 10秒  ：衰减0（短暂停顿，完全豁免） */
#define LAZY_DECAY_STEP2_SECS    60    /* 空闲 < 60秒  ：衰减1 */
#define LAZY_DECAY_STEP3_SECS   300    /* 空闲 < 5分钟 ：衰减2 */
#define LAZY_DECAY_STEP4_SECS  1800    /* 空闲 < 30分钟：衰减3 */
                                       /* 空闲 ≥ 30分钟：完全清零 */

/* 热度级别范围 */
#define COMPOSITE_LRU_HOTNESS_MAX     7
#define COMPOSITE_LRU_HOTNESS_MIN     0

/* 资源状态码 */
#define RESOURCE_AVAILABLE              0
#define RESOURCE_OVERLOADED             1
#define RESOURCE_BANDWIDTH_SATURATED    2
#define RESOURCE_MIGRATION_PRESSURE     3

/* ========== 可配置参数结构体（对应 JSON 文件）========== */
typedef struct {
    uint32_t decay_threshold_sec;       /* 周期衰减间隔（秒），默认 10 */
    uint8_t  migrate_hotness_threshold; /* 触发迁移的热度阈值，默认 5 */
    uint8_t  stability_count;           /* 字典路径稳定性计数阈值，默认 3 */
    uint32_t hot_candidates_size;       /* 候选池环形缓冲区容量，默认 256 */
    uint32_t scan_batch_size;           /* 每次 serverCron 扫描 key 数，默认 200 */
    double   overload_threshold;        /* 节点内存过载阈值（0~1），默认 0.8 */
    double   bandwidth_threshold;       /* 带宽饱和阈值（0~1），默认 0.9 */
    double   pressure_threshold;        /* 迁移压力阈值（0~1），默认 0.7 */
    int      auto_migrate_enabled;      /* 1=开启后台自动迁移，0=仅手动触发，默认 1 */
} composite_lru_config_t;

/* ========== 数据结构 ========== */

/* Key热度信息（字典回退路径） */
typedef struct {
    uint8_t  hotness;                   /* 热度级别（0-7） */
    uint8_t  stability_counter;         /* 稳定性计数器 */
    uint16_t last_access;               /* 上次访问时间（LRU_CLOCK低16位） */
    uint64_t access_count;              /* 累计访问次数 */
    int      current_node;              /* 当前所在NUMA节点 */
    int      preferred_node;            /* 迁移目标节点 */
} composite_lru_heat_info_t;

/* 热点候选池条目（环形缓冲区元素）*/
typedef struct {
    void    *key;                       /* Key 指针（robj*） */
    void    *val;                       /* Value 指针（用于重读 PREFIX 热度）*/
    int      target_node;               /* 写入时的目标节点（CPU 所在节点）*/
    uint8_t  hotness_snapshot;          /* 写入时热度快照（仅用于优先级排序，执行前重读）*/
} hot_candidate_t;

/* 策略私有数据 */
typedef struct {
    /* 数据库上下文（用于实际迁移调用）*/
    redisDb *db;

    /* 运行时配置（从 JSON 加载）*/
    composite_lru_config_t config;

    /* 热点候选池（环形缓冲区，快速通道）*/
    hot_candidate_t *hot_candidates;    /* 大小 = config.hot_candidates_size */
    uint32_t  candidates_head;          /* 写入游标（模 size 取 slot，覆盖最旧）*/
    uint32_t  candidates_count;         /* 当前有效数量（最多 = hot_candidates_size）*/

    /* 渐进扫描游标（兜底通道）*/
    dictIterator *scan_iter;            /* 当前扫描位置，NULL 表示下一轮从头开始 */

    /* 内部状态 */
    uint64_t last_decay_time;           /* 上次执行周期衰减的时间（微秒）*/
    dict    *key_heat_map;              /* 字典回退路径热度表 */

    /* 统计信息 */
    uint64_t heat_updates;              /* 热度更新次数 */
    uint64_t migrations_triggered;      /* 已触发的迁移次数 */
    uint64_t decay_operations;          /* 衰减操作次数 */
    uint64_t migrations_completed;      /* 已完成的迁移次数 */
    uint64_t migrations_failed;         /* 失败的迁移次数 */
    uint64_t candidates_written;        /* 写入候选池的次数 */
    uint64_t scan_keys_checked;         /* 渐进扫描累计检查 key 数 */
    uint64_t migrations_bw_blocked;     /* 因带宽饱和被阻止的迁移次数 */
} composite_lru_data_t;

/* ========== 公共接口 ========== */

/* 模块初始化：向策略管理器注册工厂 */
int numa_composite_lru_register(void);

/* 策略工厂函数 */
numa_strategy_t* composite_lru_create(void);
void composite_lru_destroy(numa_strategy_t *strategy);

/* 策略操作函数 */
int  composite_lru_init(numa_strategy_t *strategy);
int  composite_lru_execute(numa_strategy_t *strategy);
void composite_lru_cleanup(numa_strategy_t *strategy);

/* 热度管理 */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val);
void composite_lru_decay_heat(composite_lru_data_t *data);

/* JSON 配置加载与应用 */
int  composite_lru_load_config(const char *path, composite_lru_config_t *out);
int  composite_lru_apply_config(numa_strategy_t *strategy, const composite_lru_config_t *cfg);
void composite_lru_config_defaults(composite_lru_config_t *cfg);

/* 手动触发一轮渐进扫描（供 NUMAMIGRATE SCAN 调用）*/
int  composite_lru_scan_once(numa_strategy_t *strategy, uint32_t batch_size,
                             uint64_t *scanned_out, uint64_t *migrated_out);

/* 统计信息查询 */
void composite_lru_get_stats(numa_strategy_t *strategy,
                             uint64_t *heat_updates,
                             uint64_t *migrations_triggered,
                             uint64_t *decay_operations);

#endif /* NUMA_COMPOSITE_LRU_H */
