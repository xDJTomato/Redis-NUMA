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
#define NUMA_POOL_MAX_ALLOC 512            /* Maximum allocation from pool */

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
 * Only direct allocations are actually freed; pool memory is retained */
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

#endif /* NUMA_POOL_H */
