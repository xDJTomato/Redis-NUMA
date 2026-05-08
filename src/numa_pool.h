/* NUMA Slab 分配器（jemalloc 风格，统一覆盖 8B-4KB）
 *
 * 设计原则：
 * - 24 级 jemalloc 风格大小 class：8B~4KB，消除内部碎片
 * - 16KB Slab + 512bit 位图 + 原子 CAS 无锁分配
 * - 16 字节 PREFIX 元数据：跟踪对象大小、来源标记和节点ID
 * - ≤128B 走原子位图快速路径，>128B 同样走 Slab 但 objects_per_slab 更小
 */

#ifndef NUMA_POOL_H
#define NUMA_POOL_H

#include <stddef.h>

/* NUMA分配策略 */
#define NUMA_STRATEGY_LOCAL_FIRST 0   /* 本地节点优先 */
#define NUMA_STRATEGY_INTERLEAVE  1   /* 交错分配（跨节点负载均衡） */

/* Slab 分配器配置 */
#define NUMA_POOL_SIZE_CLASSES 24     /* jemalloc 风格 24 级大小 class */
#define SLAB_SIZE (64 * 1024)         /* 64KB slab（大对象 2048B class 从 7 提升到 31 objects/slab） */
#define SLAB_MAX_OBJECT_SIZE 4096     /* Slab 处理 8B-4KB 的对象 */
#define SLAB_BITMAP_SIZE 96           /* 3072bit 位图，覆盖 64KB slab 最多 ~2729 个对象 */
#define SLAB_EMPTY_CACHE_MAX 8        /* 每个大小级别保留的空闲 slab 缓存数量 */

/* 各大小级别的实际大小数组（24级 jemalloc 风格） */
extern const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES];

/* 初始化所有NUMA节点的Slab分配器
 * 成功返回0，失败返回-1 */
int numa_slab_init(void);

/* 清理所有Slab，释放内存 */
void numa_slab_cleanup(void);

/* 从Slab分配对象（8B-4KB）
 * 返回含PREFIX元数据的指针，失败返回NULL */
void *numa_slab_alloc(size_t size, int node, size_t *total_size);

/* 释放通过numa_slab_alloc分配的对象
 * 通过原子位图操作将该槽位标记为空闲 */
void numa_slab_free(void *ptr, size_t total_size, int node);

/* 判断给定大小是否应走Slab路径
 * size ≤ SLAB_MAX_OBJECT_SIZE(4KB) 时返回1，否则返回0 */
static inline int should_use_slab(size_t size) {
    return size <= SLAB_MAX_OBJECT_SIZE;
}

/* 获取 NUMA 节点数量（兼容旧接口） */
int numa_pool_num_nodes(void);

/* 获取当前 NUMA 节点（兼容旧接口） */
int numa_pool_get_node(void);

/* 检查 NUMA 是否可用（兼容旧接口） */
int numa_pool_available(void);

#endif /* NUMA_POOL_H */
