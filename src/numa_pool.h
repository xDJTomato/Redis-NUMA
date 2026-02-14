/* numa_pool.h - NUMA-aware memory pool allocator
 *
 * This module provides node-granular memory pool management for NUMA systems.
 * It separates the memory pool logic from zmalloc to allow future extension
 * when libNUMA adds node-granular allocator support.
 *
 * Design principles:
 * - 64KB chunks per size class per NUMA node
 * - 8 size classes: 16, 32, 64, 128, 256, 512, 1024, 2048 bytes
 * - Bump pointer allocation within chunks (O(1) time complexity)
 * - Pool memory is not individually freed (bulk free on cleanup)
 * - 16-byte PREFIX metadata for size tracking and allocation source marking
 */

#ifndef NUMA_POOL_H
#define NUMA_POOL_H

#include <stddef.h>

/* NUMA allocation strategies */
#define NUMA_STRATEGY_LOCAL_FIRST 0
#define NUMA_STRATEGY_INTERLEAVE  1

/* Memory pool configuration */
#define NUMA_POOL_SIZE_CLASSES 16
#define NUMA_POOL_MAX_ALLOC 4096           /* Maximum allocation from pool (increased for P2) */

/* P2 Optimization: Slab Allocator configuration */
#define SLAB_SIZE 4096                     /* 4KB slab size (one page) */
#define SLAB_MAX_OBJECT_SIZE 512           /* Use slab for objects <= 512B */
#define SLAB_BITMAP_SIZE 4                 /* 128 bits for up to 128 objects */
#define SLAB_EMPTY_CACHE_MAX 2             /* Max empty slabs to keep per class */

/* P1 Optimization: Compact thresholds */
#define COMPACT_THRESHOLD 0.3              /* Trigger compact when utilization < 30% */
#define COMPACT_MIN_FREE_RATIO 0.5         /* Chunk must have >50% free space to compact */
#define COMPACT_CHECK_INTERVAL 10          /* Check every N serverCron cycles */

/* Dynamic chunk size thresholds */
#define CHUNK_SIZE_SMALL    (16 * 1024)    /* 16KB for small objects (<= 256B) */
#define CHUNK_SIZE_MEDIUM   (64 * 1024)    /* 64KB for medium objects (<= 1KB) */
#define CHUNK_SIZE_LARGE    (256 * 1024)   /* 256KB for large objects (<= 4KB) */

/* Size classes for memory pool */
extern const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES];

/* Get optimal chunk size for object size */
size_t get_chunk_size_for_object(size_t obj_size);

/* Opaque pool handle */
typedef struct numa_pool numa_pool_t;

/* Pool statistics */
typedef struct {
    size_t total_allocated;     /* Total bytes allocated */
    size_t total_from_pool;     /* Bytes allocated from pool */
    size_t total_direct;        /* Bytes allocated directly via numa_alloc */
    size_t chunks_allocated;    /* Number of chunks allocated */
    size_t pool_hits;           /* Number of allocations from pool */
    size_t pool_misses;         /* Number of direct allocations */
} numa_pool_stats_t;

/* Initialize memory pool for all NUMA nodes
 * Returns 0 on success, -1 on failure */
int numa_pool_init(void);

/* Cleanup all memory pools */
void numa_pool_cleanup(void);

/* Allocate memory from pool on specified NUMA node
 * If pool allocation fails, falls back to direct NUMA allocation
 * Returns pointer with PREFIX metadata, or NULL on failure */
void *numa_pool_alloc(size_t size, int node, size_t *total_size);

/* Free memory allocated via numa_pool_alloc
 * P1 optimization: Records freed blocks in free list for reuse
 * Only direct allocations are actually freed to system */
void numa_pool_free(void *ptr, size_t total_size, int from_pool);

/* Set current NUMA node for allocations */
void numa_pool_set_node(int node);

/* Get current NUMA node */
int numa_pool_get_node(void);

/* Get number of NUMA nodes */
int numa_pool_num_nodes(void);

/* Check if NUMA is available */
int numa_pool_available(void);

/* Get pool statistics for a specific node */
void numa_pool_get_stats(int node, numa_pool_stats_t *stats);

/* Reset pool statistics */
void numa_pool_reset_stats(void);

/* P1 Optimization: Compact memory pools */
/* Try to compact low-utilization chunks across all nodes
 * Returns number of chunks compacted */
int numa_pool_try_compact(void);

/* Get chunk utilization ratio for a specific node and size class
 * Returns utilization percentage (0.0 - 1.0) */
float numa_pool_get_utilization(int node, int size_class_idx);

/* P2 Optimization: Slab Allocator functions */
/* Initialize slab allocator for all NUMA nodes
 * Returns 0 on success, -1 on failure */
int numa_slab_init(void);

/* Cleanup all slabs */
void numa_slab_cleanup(void);

/* Allocate memory from slab for small objects (<=512B)
 * Returns pointer with PREFIX metadata, or NULL on failure */
void *numa_slab_alloc(size_t size, int node, size_t *total_size);

/* Free memory allocated via numa_slab_alloc
 * Marks object as free in slab bitmap */
void numa_slab_free(void *ptr, size_t total_size, int node);

/* Check if size should use slab allocator
 * Returns 1 if size <= SLAB_MAX_OBJECT_SIZE, 0 otherwise */
static inline int should_use_slab(size_t size) {
    return size <= SLAB_MAX_OBJECT_SIZE;
}

#endif /* NUMA_POOL_H */
