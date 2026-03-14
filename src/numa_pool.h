/* numa_pool.h - NUMA感知内存池分配器
 *
 * 本模块为NUMA系统提供节点粒度的内存池管理，减少 numa_alloc_onnode 系统调用次数
 * 以提升内存分配性能。Pool分配器和Slab分配器均实现于 numa_pool.c 中。
 *
 * 设计原则：
 * - 动态chunk大小：小对象256KB、中等对象512KB、大对象1MB
 * - 16级大小分类：16/32/48/64/96/128/192/256/384/512/768/1024/1536/2048/3072/4096字节
 * - Bump Pointer分配（O(1)时间复杂度）+ Free List复用
 * - Slab分配器内置：≤128B小对象走Slab快速路径，原子位图O(1)管理
 * - 16字节PREFIX元数据：跟踪对象大小、来源标记和节点ID
 */

#ifndef NUMA_POOL_H
#define NUMA_POOL_H

#include <stddef.h>

/* NUMA分配策略 */
#define NUMA_STRATEGY_LOCAL_FIRST 0   /* 本地节点优先 */
#define NUMA_STRATEGY_INTERLEAVE  1   /* 交错分配（跨节点负载均衡） */

/* 内存池配置 */
#define NUMA_POOL_SIZE_CLASSES 16     /* 大小级别数量（P0优化后从8扩展为16） */
#define NUMA_POOL_MAX_ALLOC 4096      /* 内存池分配的最大对象大小（P2扩展至4096） */

/* P2优化：Slab分配器配置 */
#define SLAB_SIZE (16 * 1024)         /* 16KB slab大小（P2修复：从4KB增至16KB） */
#define SLAB_MAX_OBJECT_SIZE 128      /* Slab仅处理≤128B的小对象 */
#define SLAB_BITMAP_SIZE 16           /* 512bit位图，最多支持512个对象 */
#define SLAB_EMPTY_CACHE_MAX 2        /* 每个大小级别保留的空闲slab缓存数量 */

/* P1优化：Compact压缩阈值 */
#define COMPACT_THRESHOLD 0.3         /* 利用率低于30%时触发压缩 */
#define COMPACT_MIN_FREE_RATIO 0.5    /* chunk空闲率超过50%才参与压缩 */
#define COMPACT_CHECK_INTERVAL 10     /* 每N次serverCron检查一次 */

/* 动态chunk大小阈值（P0优化后增大以提升性能） */
#define CHUNK_SIZE_SMALL    (256 * 1024)   /* 256KB：用于≤256B的小对象 */
#define CHUNK_SIZE_MEDIUM   (512 * 1024)   /* 512KB：用于≤1KB的中等对象 */
#define CHUNK_SIZE_LARGE    (1024 * 1024)  /* 1MB：用于≤4KB的较大对象 */

/* 各大小级别的实际大小数组（16级） */
extern const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES];

/* 根据对象大小获取最优chunk大小 */
size_t get_chunk_size_for_object(size_t obj_size);

/* 内存池句柄（不透明类型） */
typedef struct numa_pool numa_pool_t;

/* 内存池统计信息 */
typedef struct {
    size_t total_allocated;     /* 已分配的总字节数 */
    size_t total_from_pool;     /* 从池中分配的字节数 */
    size_t total_direct;        /* 通过numa_alloc直接分配的字节数 */
    size_t chunks_allocated;    /* 已分配的chunk数量 */
    size_t pool_hits;           /* 命中内存池的次数（池命中） */
    size_t pool_misses;         /* 未命中内存池、直接分配的次数 */
} numa_pool_stats_t;

/* 初始化所有NUMA节点的内存池
 * 成功返回0，失败返回-1 */
int numa_pool_init(void);

/* 清理所有内存池，释放NUMA内存 */
void numa_pool_cleanup(void);

/* 从指定NUMA节点的内存池分配内存
 * 若池分配失败，回退至直接NUMA分配
 * 返回含PREFIX元数据的指针，失败返回NULL */
void *numa_pool_alloc(size_t size, int node, size_t *total_size);

/* 释放通过numa_pool_alloc分配的内存
 * P1优化：将释放的块加入free_list以供复用
 * 仅直接分配（from_pool=0）的内存才真正归还系统 */
void numa_pool_free(void *ptr, size_t total_size, int from_pool);

/* 设置当前线程的目标NUMA节点 */
void numa_pool_set_node(int node);

/* 获取当前NUMA节点 */
int numa_pool_get_node(void);

/* 获取NUMA节点总数 */
int numa_pool_num_nodes(void);

/* 检查NUMA是否可用 */
int numa_pool_available(void);

/* 获取指定节点的内存池统计信息 */
void numa_pool_get_stats(int node, numa_pool_stats_t *stats);

/* 重置内存池统计信息 */
void numa_pool_reset_stats(void);

/* P1优化：压缩低利用率chunk
 * 遍历所有节点和大小级别，回收利用率过低的chunk
 * 返回被压缩的chunk数量 */
int numa_pool_try_compact(void);

/* 获取指定节点和大小级别的chunk利用率（0.0~1.0） */
float numa_pool_get_utilization(int node, int size_class_idx);

/* ===== P2优化：Slab分配器接口（实现于numa_pool.c中） ===== */

/* 初始化所有NUMA节点的Slab分配器
 * 成功返回0，失败返回-1 */
int numa_slab_init(void);

/* 清理所有Slab，释放内存 */
void numa_slab_cleanup(void);

/* 从Slab分配小对象（≤128B）
 * 返回含PREFIX元数据的指针，失败返回NULL */
void *numa_slab_alloc(size_t size, int node, size_t *total_size);

/* 释放通过numa_slab_alloc分配的对象
 * 通过原子位图操作将该槽位标记为空闲 */
void numa_slab_free(void *ptr, size_t total_size, int node);

/* 判断给定大小是否应走Slab路径
 * size ≤ SLAB_MAX_OBJECT_SIZE(128B) 时返回1，否则返回0 */
static inline int should_use_slab(size_t size) {
    return size <= SLAB_MAX_OBJECT_SIZE;
}

#endif /* NUMA_POOL_H */
