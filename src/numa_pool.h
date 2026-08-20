/* NUMA slab allocator (jemalloc-style, covering 8B-64KB).
 *
 * Design principles:
 * - 33 jemalloc-style size classes: 8B-64KB, eliminating internal fragmentation
 * - Small slabs of 64KB + large slabs of 2MB, with back-pointer headers for O(1) free lookup
 * - 16-byte PREFIX metadata: tracks object size, source flag, and node ID
 * - Objects <= 4KB use small slabs, 4KB-64KB use large slabs (no per-object page alignment waste)
 */

#ifndef NUMA_POOL_H
#define NUMA_POOL_H

#include <stddef.h>

/* NUMA allocation strategies. */
#define NUMA_STRATEGY_LOCAL_FIRST 0   /* Local node first. */
#define NUMA_STRATEGY_INTERLEAVE  1   /* Interleaved allocation (cross-node load balancing). */

/* Slab allocator configuration. */
#define NUMA_POOL_SIZE_CLASSES 33     /* 33 jemalloc-style size classes (8B-64KB). */
#define SLAB_SIZE (64 * 1024)         /* 64KB small slab. */
#define LARGE_SLAB_SIZE (2UL * 1024 * 1024) /* 2MB large slab (objects > 4KB). */
#define SLAB_MAX_OBJECT_SIZE 65536    /* Slabs handle objects of 8B-64KB. */
#define SLAB_BITMAP_SIZE 96           /* 3072-bit bitmap. */
#define SLAB_EMPTY_CACHE_MAX 8        /* Kept: empty slabs are no longer eagerly freed (see numa_slab_free). */

/* Actual size array per size class (33 jemalloc-style classes). */
extern const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES];

/* Initialize the slab allocators of all NUMA nodes.
 * Returns 0 on success, -1 on failure. */
int numa_slab_init(void);

/* Clean up all slabs and release memory. */
void numa_slab_cleanup(void);

/* Allocate an object from a slab (8B-64KB).
 * Returns a pointer including PREFIX metadata, NULL on failure. */
void *numa_slab_alloc(size_t size, int node, size_t *total_size);

/* Free an object allocated by numa_slab_alloc.
 * Marks the slot free via atomic bitmap operations. */
void numa_slab_free(void *ptr);

/* Return whether a given size should use the slab path.
 * Returns 1 when size <= SLAB_MAX_OBJECT_SIZE (64KB), otherwise 0. */
static inline int should_use_slab(size_t size) {
    return size <= SLAB_MAX_OBJECT_SIZE;
}

/* Get the number of NUMA nodes (compatibility with the old interface). */
int numa_pool_num_nodes(void);

/* Get the current NUMA node (compatibility with the old interface). */
int numa_pool_get_node(void);

/* Check whether NUMA is available (compatibility with the old interface). */
int numa_pool_available(void);

#endif /* NUMA_POOL_H */
