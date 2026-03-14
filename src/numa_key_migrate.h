/* numa_key_migrate.h - NUMA Key级别迁移模块
 *
 * 本模块实现Redis Key在NUMA节点间的细粒度迁移功能，
 * 通过追踪Key的访问模式，将热Key迁移到最优节点以降低内存访问延迟。
 *
 * 主要特性：
 * - Key级别粒度迁移（以robj为迁移单元）
 * - LRU集成热度追踪（阶梯式惰性衰减）
 * - 各Redis数据类型的专用迁移适配器
 * - 原子指针切换确保迁移过程的一致性
 */

#ifndef NUMA_KEY_MIGRATE_H
#define NUMA_KEY_MIGRATE_H

#include "server.h"
#include <stdint.h>
#include <pthread.h>

/* 返回码 */
#define NUMA_KEY_MIGRATE_OK       0    /* 操作成功 */
#define NUMA_KEY_MIGRATE_ERR     -1    /* 一般错误 */
#define NUMA_KEY_MIGRATE_ENOENT  -2    /* Key不存在 */
#define NUMA_KEY_MIGRATE_EINVAL  -3    /* 参数无效 */
#define NUMA_KEY_MIGRATE_ENOMEM  -4    /* 内存不足 */
#define NUMA_KEY_MIGRATE_ETYPE   -5    /* 不支持的数据类型 */

/* 热度级别（0-7） */
#define HOTNESS_MIN_LEVEL  0   /* 最低热度（冷数据） */
#define HOTNESS_MAX_LEVEL  7   /* 最高热度 */
#define HOTNESS_DEFAULT    3   /* 初始默认热度 */
#define MIGRATION_HOTNESS_THRESHOLD  5   /* 触发迁移的热度阈值 */

/* 热度衰减阈值（LRU ticks单位，10秒），用于旧式周期性衰减（兼容保留） */
#define HEAT_DECAY_THRESHOLD  10000

/* 阶梯式惰性衰减阈值（LRU时钟秒数，在每次访问时应用）
 * 与 numa_composite_lru.h 中的常量保持一致，确保热度语义统一 */
#define KEY_LAZY_DECAY_STEP1_SECS    10    /* 空闲 < 10秒  ：衰减0（短暂停顿，豁免） */
#define KEY_LAZY_DECAY_STEP2_SECS    60    /* 空闲 < 60秒  ：衰减1 */
#define KEY_LAZY_DECAY_STEP3_SECS   300    /* 空闲 < 5分钟 ：衰减2 */
#define KEY_LAZY_DECAY_STEP4_SECS  1800    /* 空闲 < 30分钟：衰减3 */
                                           /* 空闲 ≥ 30分钟：完全清零 */

/* 配置参数 */
#define DEFAULT_MIGRATE_THRESHOLD   5    /* 默认迁移热度阈值 */
#define DEFAULT_BATCH_SIZE          50   /* 默认批量迁移数量 */

/* ========== 数据结构 ========== */

/* Key的NUMA元数据 */
typedef struct {
    int current_node;               /* 当前所在NUMA节点 */
    uint8_t hotness_level;          /* 热度级别（0-7） */
    uint16_t last_access_time;      /* 上次访问时间（LRU时钟） */
    size_t memory_footprint;        /* 内存占用大小（字节） */
    uint64_t access_count;          /* 累计访问次数 */
} key_numa_metadata_t;

/* 迁移请求 */
typedef struct {
    robj *key_obj;                  /* 目标Key对象 */
    int source_node;                /* 源NUMA节点 */
    int target_node;                /* 目标NUMA节点 */
    size_t data_size;               /* 待迁移数据大小 */
    uint64_t start_time;            /* 迁移开始时间（微秒） */
} migration_request_t;

/* 迁移统计信息 */
typedef struct {
    uint64_t total_migrations;              /* 总迁移次数 */
    uint64_t successful_migrations;         /* 成功迁移次数 */
    uint64_t failed_migrations;             /* 失败迁移次数 */
    uint64_t total_bytes_migrated;          /* 总迁移字节数 */
    uint64_t total_migration_time_us;       /* 总迁移耗时（微秒） */
    uint64_t peak_concurrent_migrations;    /* 峰值并发迁移数 */
} numa_key_migrate_stats_t;

/* 模块全局上下文 */
typedef struct {
    int initialized;                /* 初始化标志 */
    dict *key_metadata;             /* Key元数据哈希表（robj* → metadata） */
    pthread_mutex_t mutex;          /* 并发控制锁 */
    numa_key_migrate_stats_t stats; /* 迁移统计信息 */
} numa_key_migrate_ctx_t;

/* ========== 核心接口 ========== */

/* 模块初始化与清理 */
int numa_key_migrate_init(void);
void numa_key_migrate_cleanup(void);

/* 单Key迁移：将指定Key迁移到目标节点 */
int numa_migrate_single_key(redisDb *db, robj *key, int target_node);

/* 批量迁移：将列表中的所有Key迁移到目标节点 */
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node);

/* 模式迁移：将匹配pattern的所有Key迁移到目标节点 */
int numa_migrate_keys_by_pattern(redisDb *db, const char *pattern, int target_node);

/* 全库迁移：将整个数据库迁移到目标节点 */
int numa_migrate_entire_database(redisDb *db, int target_node);

/* ========== 热度追踪 ========== */

/* 记录Key访问（在lookupKey时调用） */
void numa_record_key_access(robj *key, robj *val);

/* 执行热度衰减（周期性调用） */
void numa_perform_heat_decay(void);

/* ========== 元数据管理 ========== */

/* 获取Key的NUMA元数据 */
key_numa_metadata_t* numa_get_key_metadata(robj *key);

/* 获取Key当前所在的NUMA节点 */
int numa_get_key_current_node(robj *key);

/* Key删除时通知NUMA模块清理元数据（防止内存泄漏） */
void numa_on_key_delete(robj *key);

/* ========== 统计信息 ========== */

/* 获取迁移统计信息 */
void numa_get_migration_statistics(numa_key_migrate_stats_t *stats);

/* 重置迁移统计信息 */
void numa_reset_migration_statistics(void);

/* ========== 各数据类型迁移适配器（内部函数，对外暴露用于测试） ========== */
int migrate_string_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node);

/* ========== Redis命令接口 ========== */

/* NUMAMIGRATE 命令处理函数 */
void numamigrateCommand(client *c);

#endif /* NUMA_KEY_MIGRATE_H */
