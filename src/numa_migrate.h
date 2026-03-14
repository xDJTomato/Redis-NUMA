/* numa_migrate.h - NUMA内存迁移模块
 *
 * 本模块提供将内存对象在NUMA节点间迁移的基础功能。
 * 阶段1：用于测试的基础迁移功能
 * 阶段2：结合热度追踪的自动负载均衡（规划中）
 */

#ifndef NUMA_MIGRATE_H
#define NUMA_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

/* 迁移操作返回码 */
#define NUMA_MIGRATE_OK        0    /* 操作成功 */
#define NUMA_MIGRATE_ERR      -1    /* 一般错误 */
#define NUMA_MIGRATE_INVALID  -2    /* 参数无效 */
#define NUMA_MIGRATE_NOMEM    -3    /* 内存不足 */

/* 迁移统计信息 */
typedef struct {
    uint64_t total_migrations;      /* 已完成的迁移次数 */
    uint64_t bytes_migrated;        /* 已迁移的总字节数 */
    uint64_t failed_migrations;     /* 失败的迁移次数 */
    uint64_t migration_time_us;     /* 迁移消耗的总时间（微秒） */
} numa_migrate_stats_t;

/* 初始化迁移模块 */
int numa_migrate_init(void);

/* 清理迁移模块，释放资源 */
void numa_migrate_cleanup(void);

/* 将内存块从当前节点迁移到目标节点
 *
 * @param ptr:         内存块的用户指针（非raw指针）
 * @param size:        内存块大小
 * @param target_node: 目标NUMA节点ID
 * @return:            成功时返回新指针，失败返回NULL
 *
 * 注意：迁移成功后，旧指针将失效，不可再使用
 */
void *numa_migrate_memory(void *ptr, size_t size, int target_node);

/* 获取迁移统计信息 */
void numa_migrate_get_stats(numa_migrate_stats_t *stats);

/* 重置迁移统计信息 */
void numa_migrate_reset_stats(void);

#endif /* NUMA_MIGRATE_H */
