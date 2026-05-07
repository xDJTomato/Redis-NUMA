/* numa_arena.h - jemalloc风格NUMA感知arena分配器
 *
 * 三层架构：tcache(线程缓存) → bin(中央池) → extent(大块内存)
 *
 * 启动时通过 numa-allocator-type 配置选择 arena 或 pool 分配器：
 *   numa-allocator-type arena  → 使用本文件接口
 *   numa-allocator-type pool   → 使用 numa_pool.h 接口（现有）
 */

#ifndef NUMA_ARENA_H
#define NUMA_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* ── 编译常量 ──────────────────────────────────────────── */

#define NUMA_ARENA_MAX_NODES      8
#define NUMA_ARENA_SIZE_CLASSES   20
#define NUMA_ARENA_MAX_ALLOC      4096      /* bin 路径上限 */
#define TCACHE_MAX_OBJECTS        128       /* 每 bin tcache 容量 */
#define TCACHE_FLUSH_BATCH        64        /* flush 时一次归还量 */

/* 20 级 size class（16B–4KB，覆盖 Redis 常见对象大小） */
extern const size_t numa_arena_size_classes[NUMA_ARENA_SIZE_CLASSES];

/* 每 class 的 slab 大小和 region 数在 init 时计算 */

/* ── 数据结构 ──────────────────────────────────────────── */

/* extent: numa_alloc_onnode 分配的大块，内部切成 region */
typedef struct numa_extent {
    void             *memory;       /* 基址 */
    size_t            size;        /* extent 总大小 */
    int               node_id;
    int               nfree;       /* 空闲 region 数 */
    int               nregs;       /* region 总数 */
    struct numa_extent *next;
    uint8_t           bitmap[];    /* 柔性数组：region 位图，必须在结构体末尾 */
} numa_extent_t;

/* bin: 每个 size-class 的中央池 */
typedef struct {
    pthread_mutex_t   lock;
    numa_extent_t    *slabcur;       /* 当前活跃 slab */
    numa_extent_t    *slabs_head;    /* 所有 slab 链表头（简化：不区分 partial/full） */
    size_t            reg_size;      /* 每个 region 大小 */
    size_t            slab_size;     /* 每个 slab 大小 */
    int               nregs_per_slab;/* 每个 slab 的 region 数 */
} numa_bin_t;

/* arena: per-NUMA-node */
typedef struct {
    int         node_id;
    numa_bin_t  bins[NUMA_ARENA_SIZE_CLASSES];
} numa_arena_t;

/* tcache bin: 线程本地无锁栈 */
typedef struct {
    void    *stack[TCACHE_MAX_OBJECTS];
    int      count;
} tcache_bin_t;

/* tcache: 每线程一套 */
typedef struct {
    tcache_bin_t bins[NUMA_ARENA_MAX_NODES][NUMA_ARENA_SIZE_CLASSES];
} tcache_t;

/* ── 公共接口 ──────────────────────────────────────────── */

int  numa_arena_init(void);
void numa_arena_cleanup(void);
int  numa_arena_num_nodes(void);

void *numa_arena_alloc(size_t size, int node, size_t *total_size, int *from_pool_out);
void  numa_arena_free(void *ptr, size_t total_size, int from_pool, int node_id);

#endif /* NUMA_ARENA_H */
