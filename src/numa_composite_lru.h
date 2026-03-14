/* numa_composite_lru.h - NUMA复合LRU策略（插槽1，默认策略）
 *
 * 本模块实现基于Redis原生LRU机制的默认NUMA迁移策略，
 * 提供稳定性优先的热度管理和智能迁移触发机制。
 *
 * 主要特性：
 * - 稳定性优先：避免热度频繁波动导致的无效迁移
 * - 资源感知：综合考虑节点内存占用和带宽利用率
 * - 阶梯式惰性衰减：在下次访问时按空闲时长计算衰减量，无需后台扫描
 * - 与Key迁移模块深度集成
 */

#ifndef NUMA_COMPOSITE_LRU_H
#define NUMA_COMPOSITE_LRU_H

#include "numa_strategy_slots.h"
#include "dict.h"
#include "adlist.h"
#include <stdint.h>

/* ========== 配置常量 ========== */

/* 核心参数 */
#define COMPOSITE_LRU_DEFAULT_DECAY_THRESHOLD    10000000  /* 衰减阈值：10秒（微秒） */
#define COMPOSITE_LRU_DEFAULT_STABILITY_COUNT    3         /* 连续3次未触发迁移才算稳定 */
#define COMPOSITE_LRU_DEFAULT_MIGRATE_THRESHOLD  5         /* 热度≥5时触发迁移 */

/* 阶梯式惰性衰减阈值（LRU时钟秒数，在每次访问时应用） */
#define LAZY_DECAY_STEP1_SECS    10    /* 空闲 < 10秒  ：衰减0（短暂停顿，完全豁免） */
#define LAZY_DECAY_STEP2_SECS    60    /* 空闲 < 60秒  ：衰减1 */
#define LAZY_DECAY_STEP3_SECS   300    /* 空闲 < 5分钟 ：衰减2 */
#define LAZY_DECAY_STEP4_SECS  1800    /* 空闲 < 30分钟：衰减3 */
                                       /* 空闲 ≥ 30分钟：完全清零（衰减 = HOTNESS_MAX） */

/* 资源阈值 */
#define COMPOSITE_LRU_DEFAULT_OVERLOAD_THRESHOLD     0.8   /* 节点内存使用率>80%视为过载 */
#define COMPOSITE_LRU_DEFAULT_BANDWIDTH_THRESHOLD    0.9   /* 带宽利用率>90%视为饱和 */
#define COMPOSITE_LRU_DEFAULT_PRESSURE_THRESHOLD     0.7   /* 迁移压力>70%时限流 */

/* 性能参数 */
#define COMPOSITE_LRU_MAX_PENDING_MIGRATIONS     1000      /* 迁移队列最大容量 */
#define COMPOSITE_LRU_PENDING_TIMEOUT            30000000  /* 队列中任务超时：30秒（微秒） */

/* 热度级别范围 */
#define COMPOSITE_LRU_HOTNESS_MAX     7   /* 最高热度 */
#define COMPOSITE_LRU_HOTNESS_MIN     0   /* 最低热度（冷数据） */

/* 资源状态码 */
#define RESOURCE_AVAILABLE              0   /* 资源充足，可正常迁移 */
#define RESOURCE_OVERLOADED             1   /* 节点内存过载 */
#define RESOURCE_BANDWIDTH_SATURATED    2   /* 带宽已饱和 */
#define RESOURCE_MIGRATION_PRESSURE     3   /* 迁移压力过高 */

/* ========== 数据结构 ========== */

/* Key热度信息 */
typedef struct {
    uint8_t  hotness;                   /* 热度级别（0-7） */
    uint8_t  stability_counter;         /* 稳定性计数器（连续未迁移次数） */
    uint16_t last_access;               /* 上次访问时间（LRU_CLOCK低16位） */
    uint64_t access_count;              /* 累计访问次数 */
    int      current_node;              /* 当前所在NUMA节点 */
    int      preferred_node;            /* 迁移目标节点（优选） */
} composite_lru_heat_info_t;

/* 待处理迁移任务 */
typedef struct {
    void    *key;                       /* 目标Key指针（robj*） */
    int      target_node;               /* 目标节点 */
    uint64_t enqueue_time;              /* 入队时间（微秒） */
    uint8_t  priority;                  /* 迁移优先级 */
} pending_migration_t;

/* 策略私有数据 */
typedef struct {
    /* 热度控制参数 */
    uint32_t decay_threshold;           /* 衰减触发间隔（微秒） */
    uint8_t  stability_count;           /* 稳定性计数阈值 */
    uint8_t  migrate_hotness_threshold; /* 触发迁移的热度阈值 */

    /* 资源监控参数 */
    double   overload_threshold;        /* 节点过载阈值（0.0~1.0） */
    double   bandwidth_threshold;       /* 带宽饱和阈值 */
    double   pressure_threshold;        /* 迁移压力阈值 */

    /* 内部状态 */
    uint64_t last_decay_time;           /* 上次执行衰减的时间 */
    dict    *key_heat_map;              /* Key热度哈希表 */
    list    *pending_migrations;        /* 待迁移任务队列 */

    /* 统计信息 */
    uint64_t heat_updates;              /* 热度更新次数 */
    uint64_t migrations_triggered;      /* 已触发的迁移次数 */
    uint64_t decay_operations;          /* 衰减操作次数 */
    uint64_t migrations_completed;      /* 已完成的迁移次数 */
    uint64_t migrations_failed;         /* 失败的迁移次数 */
    uint64_t pending_timeouts;          /* 超时丢弃的待迁移任务数 */
} composite_lru_data_t;

/* ========== 公共接口 ========== */

/* 模块初始化：向策略管理器注册工厂 */
int numa_composite_lru_register(void);

/* 策略工厂函数（也对外暴露用于测试） */
numa_strategy_t* composite_lru_create(void);
void composite_lru_destroy(numa_strategy_t *strategy);

/* 策略操作函数（也对外暴露用于测试） */
int composite_lru_init(numa_strategy_t *strategy);
int composite_lru_execute(numa_strategy_t *strategy);
void composite_lru_cleanup(numa_strategy_t *strategy);

/* 热度管理 */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val);
void composite_lru_decay_heat(composite_lru_data_t *data);

/* 统计信息查询 */
void composite_lru_get_stats(numa_strategy_t *strategy,
                             uint64_t *heat_updates,
                             uint64_t *migrations_triggered,
                             uint64_t *decay_operations);

#endif /* NUMA_COMPOSITE_LRU_H */
