/* =============================================================================
 * nf_alloc.h - independent, NUMA-aware, low-fragmentation allocator (pure C11).
 *
 * Design (synthesized from recent literature and production allocators):
 *   - llalloc  : NUMA node identity via address ranges, not per-object headers;
 *   - snmalloc : no per-object header; size/node recovered from segment metadata;
 *   - mimalloc : per-thread free-list caching (tcache) with batch refill/flush;
 *   - jemalloc : precise size classes + slab/segment reuse to bound fragmentation.
 *
 * Key improvement over the previous libnuma slab allocator: small objects carry
 * NO 16-byte PREFIX (the old design added 16B to every allocation, i.e. 200%
 * overhead on an 8B object).  Instead a metamap maps an address to its segment,
 * giving O(1) free() and usable_size() without per-object metadata.
 *
 * The storage backend is abstract: on Linux+libnuma it is numa_alloc_onnode /
 * mmap+mbind; on any other host it is plain malloc, so the module builds and is
 * benchmarked on this Windows machine too.
 * ========================================================================== */
#ifndef NF_ALLOC_H
#define NF_ALLOC_H

#include "nf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nf_alloc nf_alloc_t;

/* Storage backend: allocate/free a contiguous chunk on a NUMA node. */
typedef struct {
    void *(*chunk_alloc)(void *ud, size_t size, int node); /* NULL on OOM */
    void  (*chunk_free)(void *ud, void *ptr, size_t size);
    int    num_nodes;                                     /* >= 1 */
    void  *ud;
} nf_alloc_backend_t;

/* Create a NUMA-aware allocator. `backend` may be NULL for a default that
 * emulates 2 logical nodes on top of malloc. */
nf_alloc_t *nf_alloc_create(const nf_alloc_backend_t *backend);
void        nf_alloc_destroy(nf_alloc_t *a);

void  *nf_alloc_malloc(nf_alloc_t *a, size_t size);              /* default node */
void  *nf_alloc_calloc(nf_alloc_t *a, size_t n, size_t size);
void  *nf_alloc_malloc_onnode(nf_alloc_t *a, size_t size, int node);
void  *nf_alloc_realloc(nf_alloc_t *a, void *ptr, size_t size);
void   nf_alloc_free(nf_alloc_t *a, void *ptr);

/* Exact usable size for a live allocation (0 if ptr is unknown). */
size_t nf_alloc_usable_size(nf_alloc_t *a, const void *ptr);

typedef struct {
    uint64_t alloc_count;
    uint64_t free_count;
    uint64_t requested_bytes;   /* cumulative bytes asked for */
    uint64_t usable_bytes;      /* cumulative bytes carved out */
    uint64_t cur_usable;        /* current live bytes carved out */
    uint64_t backend_bytes;     /* current live bytes from backend */
    uint64_t peak_backend_bytes;
    uint64_t segment_count;
    int      num_nodes;
    uint64_t node_alloc_count[NF_MAX_NODES];
} nf_alloc_stats_t;

void   nf_alloc_get_stats(nf_alloc_t *a, nf_alloc_stats_t *out);
/* Internal fragmentation ratio: (usable - requested) / requested. */
double nf_alloc_internal_frag(nf_alloc_t *a);
/* External (segment-header/large-header) overhead: backend / cur_usable - 1. */
double nf_alloc_external_overhead(nf_alloc_t *a);

/* Number of configured size classes (for tests/bench). */
int    nf_alloc_num_classes(void);
size_t nf_alloc_class_size(int idx);

#ifdef __cplusplus
}
#endif

#endif /* NF_ALLOC_H */
