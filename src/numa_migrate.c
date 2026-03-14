/* numa_migrate.c - NUMA内存迁移模块实现
 *
 * 基础迁移功能，用于测试阶段。
 * 使用 numa_zmalloc_onnode 在目标节点分配内存，并拷贝数据。
 */

#define _GNU_SOURCE
#include "numa_migrate.h"
#include "zmalloc.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <numa.h>

/* 内部统计信息 */
static numa_migrate_stats_t migrate_stats = {0};
static int migrate_initialized = 0;

/* 获取当前时间（微秒） */
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* 初始化迁移模块 */
int numa_migrate_init(void)
{
    if (migrate_initialized)
        return 0;
    
    if (numa_available() == -1) {
        return NUMA_MIGRATE_ERR;
    }
    
    memset(&migrate_stats, 0, sizeof(migrate_stats));
    migrate_initialized = 1;
    return NUMA_MIGRATE_OK;
}

/* 清理迁移模块 */
void numa_migrate_cleanup(void)
{
    migrate_initialized = 0;
}

/* 将内存块从当前节点迁移到目标节点 */
void *numa_migrate_memory(void *ptr, size_t size, int target_node)
{
    if (!migrate_initialized || ptr == NULL || size == 0) {
        return NULL;
    }
    
    /* 校验目标节点 */
    if (target_node < 0 || target_node > numa_max_node()) {
        return NULL;
    }
    
    uint64_t start_time = get_time_us();
    
    /* 第一步：在目标节点分配新内存 */
    void *new_ptr = numa_zmalloc_onnode(size, target_node);
    if (!new_ptr) {
        migrate_stats.failed_migrations++;
        return NULL;
    }
    
    /* 第二步：将数据从旧地址拷贝到新地址 */
    memcpy(new_ptr, ptr, size);
    
    /* 第三步：释放旧内存 */
    zfree(ptr);
    
    /* 第四步：更新统计信息 */
    migrate_stats.total_migrations++;
    migrate_stats.bytes_migrated += size;
    migrate_stats.migration_time_us += (get_time_us() - start_time);
    
    return new_ptr;
}

/* 获取迁移统计信息 */
void numa_migrate_get_stats(numa_migrate_stats_t *stats)
{
    if (stats) {
        *stats = migrate_stats;
    }
}

/* 重置迁移统计信息 */
void numa_migrate_reset_stats(void)
{
    memset(&migrate_stats, 0, sizeof(migrate_stats));
}


