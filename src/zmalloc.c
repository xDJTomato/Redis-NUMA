/* zmalloc - malloc wrapper with memory usage statistics.
 *
 * Copyright (c) 2009-2010, Salvatore Sanfilippo <antirez at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "fmacros.h"
#include "config.h"
#include "solarisfixes.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <assert.h>

#ifdef __linux__
#include <sys/mman.h>
#endif

/* This function provide us access to the original libc free(). This is useful
 * for instance to free results obtained by backtrace_symbols(). We need
 * to define this function before including zmalloc.h that may shadow the
 * free implementation if we use jemalloc or another non standard allocator. */
void zlibc_free(void *ptr) {
    free(ptr);
}

#include <string.h>
#include <pthread.h>
#include "zmalloc.h"
#include "atomicvar.h"

#ifdef HAVE_NUMA
#include <numa.h>
#include <sched.h>
#include <unistd.h>
#include "numa_pool.h"
#include "numa_configurable_strategy.h"
/* numaGetNodePressure() declaration: weak-symbol fallback for redis-benchmark/cli. */
__attribute__((weak)) double numaGetNodePressure(int node_id) {
    (void)node_id;
    return 0.0;
}
/* numa_config_get_best_node weak-symbol fallback: default to node 0 when the policy module is not linked. */
__attribute__((weak)) int numa_config_get_best_node(size_t size) {
    (void)size;
    return 0;
}

/* NUMA global context - kept for compatibility and future extensions. */
static struct {
    int numa_available;
    int num_nodes;
    int current_node;
    int allocation_strategy;
    int *node_distance_order;
} numa_ctx = {0};

/* Thread-local storage: the NUMA node the current thread is bound to. */
static __thread int tls_current_node = -1;

/* Forward declarations */
static void init_class_lookup(void);

/* Initialize NUMA support: initialize the slab allocator and sort nodes by distance. */
void numa_init(void)
{
    /* Initialize the slab allocator (covering 8B-64KB). */
    if (numa_slab_init() != 0) {
        numa_ctx.numa_available = 0;
        return;
    }

    /* Initialize the O(1) size class lookup table and tcache infrastructure. */
    init_class_lookup();

    /* Check NUMA availability. */
    if (numa_available() < 0) {
        numa_ctx.numa_available = 0;
        return;
    }

    numa_ctx.numa_available = 1;
    numa_ctx.num_nodes = numa_max_node() + 1;

    /* Get the current node. */
    int cpu = sched_getcpu();
    if (cpu >= 0) {
        numa_ctx.current_node = numa_node_of_cpu(cpu);
    } else {
        numa_ctx.current_node = 0;
    }

    /* Switch to an interleaved allocation strategy for cross-node load balancing. */
    numa_ctx.allocation_strategy = NUMA_STRATEGY_INTERLEAVE;

    /* Initialize the node distance ordering. */
    numa_ctx.node_distance_order = malloc(numa_ctx.num_nodes * sizeof(int));
    if (!numa_ctx.node_distance_order) {
        numa_ctx.numa_available = 0;
        return;
    }

    for (int i = 0; i < numa_ctx.num_nodes; i++) {
        numa_ctx.node_distance_order[i] = i;
    }

    /* Sort by distance. */
    for (int i = 0; i < numa_ctx.num_nodes - 1; i++) {
        for (int j = 0; j < numa_ctx.num_nodes - i - 1; j++) {
            int dist1 = numa_distance(numa_ctx.current_node, numa_ctx.node_distance_order[j]);
            int dist2 = numa_distance(numa_ctx.current_node, numa_ctx.node_distance_order[j + 1]);
            if (dist1 > dist2) {
                int temp = numa_ctx.node_distance_order[j];
                numa_ctx.node_distance_order[j] = numa_ctx.node_distance_order[j + 1];
                numa_ctx.node_distance_order[j + 1] = temp;
            }
        }
    }
}

/* Clean up NUMA resources: free the slab allocator and the node distance ordering array. */
void numa_cleanup(void)
{
    numa_slab_cleanup();

    if (numa_ctx.node_distance_order) {
        free(numa_ctx.node_distance_order);
        numa_ctx.node_distance_order = NULL;
    }
}

/* Set the NUMA allocation policy (LOCAL_FIRST / INTERLEAVE). */
int numa_set_strategy(int strategy)
{
    if (strategy != NUMA_STRATEGY_LOCAL_FIRST && strategy != NUMA_STRATEGY_INTERLEAVE)
    {
        return -1;
    }
    numa_ctx.allocation_strategy = strategy;
    return 0;
}

/* Get the current NUMA allocation policy. */
int numa_get_strategy(void)
{
    return numa_ctx.allocation_strategy;
}

#endif /* HAVE_NUMA */

#define update_zmalloc_stat_alloc(__n) atomicIncr(used_memory, (__n))
#define update_zmalloc_stat_free(__n) atomicDecr(used_memory, (__n))

/* The NUMA allocator must use the PREFIX_SIZE strategy (even when
 * HAVE_MALLOC_SIZE is defined), because libnuma cannot query the size of an
 * already allocated region. The prefix flag fields also distinguish pool
 * allocations from direct ones. */
#ifdef HAVE_NUMA
/* The NUMA allocator needs PREFIX_SIZE to track sizes and record allocation source flags. */
typedef struct {
    size_t size;           /* 8 bytes - actual allocated memory size. */
    char from_pool;        /* 1 byte - source: 0=direct allocation, 1=slab (name kept for compatibility). */
    char node_id;          /* 1 byte - NUMA node ID where the allocation lives. */
    /* Heat tracking fields (reused from padding) */
    uint8_t hotness;       /* 1 byte - hotness level (0-7), 0=cold, 7=hot. */
    uint8_t access_count;  /* 1 byte - access counter (circular counter). */
    uint16_t last_access;  /* 2 bytes - low 16 bits of the LRU clock (last access time). */
    char migrated;         /* 1 byte - migration affinity flag: 1=migrated, keep node affinity on UPDATE. */
    char reserved[1];      /* 1 byte - reserved. */
} numa_alloc_prefix_t;

/* Hotness tracking constants. */
#define NUMA_HOTNESS_MAX     7
#define NUMA_HOTNESS_MIN     0
#define NUMA_HOTNESS_DEFAULT 1

#define PREFIX_SIZE (sizeof(numa_alloc_prefix_t))
#define ASSERT_NO_SIZE_OVERFLOW(sz) assert((sz) <= SIZE_MAX - PREFIX_SIZE)

static redisAtomic size_t used_memory = 0;
#ifdef HAVE_NUMA
static redisAtomic size_t used_memory_node[ZMALLOC_MAX_NUMA_NODES];
#endif

/* Allocation path counters: current bytes and cumulative allocation counts per path. */
static redisAtomic size_t numa_alloc_slab_bytes   = 0;
static redisAtomic size_t numa_alloc_direct_bytes = 0;
static redisAtomic size_t numa_alloc_slab_count   = 0;
static redisAtomic size_t numa_alloc_direct_count = 0;

/* tcache hit rate counters. */
static redisAtomic size_t numa_tcache_alloc_hit  = 0;
static redisAtomic size_t numa_tcache_alloc_miss = 0;
static redisAtomic size_t numa_tcache_free_hit   = 0;
static redisAtomic size_t numa_tcache_free_miss  = 0;

/* ── Thread-Local Cache (tcache) ──
 *
 * Per-thread bin of recently freed slab objects. On alloc, check the tcache
 * first; on free, push into the tcache instead of returning to the slab.
 * This eliminates CAS bitmap contention, mutex slow-path, and the O(24)
 * size class lookup on cache hit.
 */

/* Forward declarations for tcache */
static inline numa_alloc_prefix_t *numa_get_prefix(void *user_ptr);

#define TCACHE_BIN_MAX     64
#define TCACHE_DRAIN_COUNT 32

typedef struct {
    void *ptrs[TCACHE_BIN_MAX];
    uint16_t count;
} tcache_bin_t;

typedef struct {
    tcache_bin_t bins[NUMA_POOL_SIZE_CLASSES];
} numa_tcache_t;

static __thread numa_tcache_t tls_tcache;
static __thread int tls_tcache_inited = 0;

/* O(1) size → class index lookup table (covers 0..65536 in 8-byte steps) */
#define CLASS_LOOKUP_ENTRIES 8193
static int g_class_lookup[CLASS_LOOKUP_ENTRIES];
static int g_class_lookup_ready = 0;

/* ── Direct Path Large Object Cache ──
 *
 * Thread-local FIFO cache for >64KB objects that bypass the slab allocator.
 * Eliminates per-alloc mmap+mbind / munmap syscall overhead by reusing
 * recently freed blocks. Matches by exact total_size + same NUMA node.
 */

#define DIRECT_CACHE_MAX      16
#define DIRECT_CACHE_MIN_SIZE (SLAB_MAX_OBJECT_SIZE + PREFIX_SIZE + 1)
#define DIRECT_CACHE_MAX_SIZE (2UL << 20)

typedef struct {
    void  *ptrs[DIRECT_CACHE_MAX];
    size_t sizes[DIRECT_CACHE_MAX];
    int    nodes[DIRECT_CACHE_MAX];
    int    count;
} direct_cache_t;

static __thread direct_cache_t tls_direct_cache;

static redisAtomic size_t numa_direct_cache_hit   = 0;
static redisAtomic size_t numa_direct_cache_miss  = 0;
static redisAtomic size_t numa_direct_cache_evict = 0;

static inline void direct_cache_ensure_init(void) {
    if (!tls_tcache_inited) {
        memset(&tls_tcache, 0, sizeof(tls_tcache));
        memset(&tls_direct_cache, 0, sizeof(tls_direct_cache));
        tls_tcache_inited = 1;
    }
}

static inline void *direct_cache_pop(int node, size_t total_size) {
    direct_cache_t *dc = &tls_direct_cache;
    for (int i = dc->count - 1; i >= 0; i--) {
        if (dc->sizes[i] == total_size && dc->nodes[i] == node) {
            void *ptr = dc->ptrs[i];
            dc->count--;
            if (i < dc->count) {
                dc->ptrs[i]  = dc->ptrs[dc->count];
                dc->sizes[i] = dc->sizes[dc->count];
                dc->nodes[i] = dc->nodes[dc->count];
            }
            atomicIncr(numa_direct_cache_hit, 1);
            return ptr;
        }
    }
    atomicIncr(numa_direct_cache_miss, 1);
    return NULL;
}

static inline void direct_cache_push(void *raw_ptr, size_t total_size, int node) {
    direct_cache_t *dc = &tls_direct_cache;
    if (dc->count >= DIRECT_CACHE_MAX) {
        numa_free(dc->ptrs[0], dc->sizes[0]);
        atomicIncr(numa_direct_cache_evict, 1);
        memmove(&dc->ptrs[0],  &dc->ptrs[1],  (DIRECT_CACHE_MAX - 1) * sizeof(void *));
        memmove(&dc->sizes[0], &dc->sizes[1], (DIRECT_CACHE_MAX - 1) * sizeof(size_t));
        memmove(&dc->nodes[0], &dc->nodes[1], (DIRECT_CACHE_MAX - 1) * sizeof(int));
        dc->count--;
    }
    dc->ptrs[dc->count]  = raw_ptr;
    dc->sizes[dc->count] = total_size;
    dc->nodes[dc->count] = node;
    dc->count++;
}

static void init_class_lookup(void) {
    int cls = 0;
    for (int i = 0; i < CLASS_LOOKUP_ENTRIES; i++) {
        size_t sz = (size_t)i << 3;
        while (cls < NUMA_POOL_SIZE_CLASSES - 1 &&
               sz > numa_pool_size_classes[cls])
            cls++;
        g_class_lookup[i] = cls;
    }
    g_class_lookup_ready = 1;
}

static inline int fast_size_class(size_t size) {
    if (size > SLAB_MAX_OBJECT_SIZE) return -1;
    return g_class_lookup[(size + 7) >> 3];
}

static inline size_t numa_slab_class_size(size_t size) {
    int cls = fast_size_class(size);
    return (cls >= 0) ? numa_pool_size_classes[cls] : 0;
}

static void tcache_drain_bin(int cls) {
    tcache_bin_t *bin = &tls_tcache.bins[cls];
    int drain = TCACHE_DRAIN_COUNT;
    while (drain > 0 && bin->count > 0) {
        void *user_ptr = bin->ptrs[--bin->count];
        numa_alloc_prefix_t *prefix = numa_get_prefix(user_ptr);
        size_t total_size = prefix->size + PREFIX_SIZE;
        int node_id = (int)prefix->node_id;
        /* tcache drain: the logical used-memory counters were already
         * released when the object entered the tcache.  Only return the
         * block to the slab and update pool instrumentation here. */
        void *raw_ptr = (char *)user_ptr - PREFIX_SIZE;
        numa_slab_free(raw_ptr, total_size, node_id);
        atomicDecr(numa_alloc_slab_bytes, total_size);
        atomicDecr(numa_alloc_slab_count, 1);
        drain--;
    }
}

#else
/* Standard allocator can use HAVE_MALLOC_SIZE if available */
#define UNUSED(x) ((void)(x))

#ifdef HAVE_MALLOC_SIZE
#define PREFIX_SIZE (0)
#else
/* Use at least 8 bits alignment on all systems. */
#if SIZE_MAX < 0xffffffffffffffffull
#define PREFIX_SIZE 8
#else
#define PREFIX_SIZE (sizeof(size_t))
#endif
#endif
#endif /* HAVE_NUMA */

/* When using the libc allocator, use a minimum allocation size to match the
 * jemalloc behavior that doesn't return NULL in this case.
 */
#define MALLOC_MIN_SIZE(x) ((x) > 0 ? (x) : sizeof(long))

/* Explicitly override malloc/free etc when using tcmalloc. */
#if defined(USE_TCMALLOC)
#define malloc(size) tc_malloc(size)
#define calloc(count, size) tc_calloc(count, size)
#define realloc(ptr, size) tc_realloc(ptr, size)
#define free(ptr) tc_free(ptr)
#elif defined(USE_JEMALLOC)
#define malloc(size) je_malloc(size)
#define calloc(count, size) je_calloc(count, size)
#define realloc(ptr, size) je_realloc(ptr, size)
#define free(ptr) je_free(ptr)
#define mallocx(size, flags) je_mallocx(size, flags)
#define dallocx(ptr, flags) je_dallocx(ptr, flags)
#endif

static void zmalloc_default_oom(size_t size)
{
    fprintf(stderr, "zmalloc: Out of memory trying to allocate %lu bytes\n",
            (unsigned long)size);
    fflush(stderr);
    abort();
}

static void (*zmalloc_oom_handler)(size_t) = zmalloc_default_oom;

#ifdef HAVE_NUMA
/* Helper: initialize the PREFIX metadata of an allocation (size, source, node ID, hotness). */
static inline void numa_init_prefix(void *ptr, size_t size, int from_slab, int node_id)
{
    numa_alloc_prefix_t *prefix = (numa_alloc_prefix_t *)ptr;
    /* Zero the whole PREFIX: when a memory pool entry is reused the prefix may
     * hold stale data, and leaving it uninitialized would make the migrated
     * flag and padding read garbage, causing wrong migration affinity. */
    memset(prefix, 0, sizeof(numa_alloc_prefix_t));
    prefix->size = size;
    prefix->from_pool = from_slab;  /* 1=Slab, 0=Direct */
    prefix->node_id = (char)node_id;
    /* Initialize the hotness tracking fields. */
    prefix->hotness = NUMA_HOTNESS_DEFAULT;  /* Set the default hotness. */
    prefix->access_count = 0;
    prefix->last_access = 0;
}

/* Helper: recover the PREFIX pointer from a user pointer. */
static inline numa_alloc_prefix_t *numa_get_prefix(void *user_ptr)
{
    return (numa_alloc_prefix_t *)((char *)user_ptr - PREFIX_SIZE);
}

/* Helper: turn a raw pointer (including PREFIX) into a user-visible pointer. */
static inline void *numa_to_user_ptr(void *raw_ptr)
{
    return (char *)raw_ptr + PREFIX_SIZE;
}

/* NUMA-aware memory allocation (with size tracking): slab (8B-64KB) then direct (>64KB). */
static void *numa_alloc_with_size_onnode(size_t size, int target_node)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    if (target_node < 0 || target_node >= numa_ctx.num_nodes)
        target_node = 0;

    /* ── tcache fast path ── */
    if (g_class_lookup_ready && should_use_slab(size)) {
        direct_cache_ensure_init();
        int cls = fast_size_class(size);
        if (cls >= 0 && tls_tcache.bins[cls].count > 0) {
            tcache_bin_t *bin = &tls_tcache.bins[cls];
            void *user_ptr = bin->ptrs[bin->count - 1];
            numa_alloc_prefix_t *prefix = numa_get_prefix(user_ptr);
            if ((int)prefix->node_id == target_node) {
                bin->count--;
                prefix->size = size;
                prefix->hotness = NUMA_HOTNESS_DEFAULT;
                prefix->access_count = 0;
                prefix->last_access = 0;
                /* tcache hit: the object becomes live again, so charge the
                 * logical used-memory counters now.  They were released when
                 * the object entered the tcache. */
                size_t total_size = size + PREFIX_SIZE;
                update_zmalloc_stat_alloc(total_size);
                if (target_node >= 0 && target_node < ZMALLOC_MAX_NUMA_NODES)
                    atomicIncr(used_memory_node[target_node], total_size);
                atomicIncr(numa_tcache_alloc_hit, 1);
                return user_ptr;
            }
        }
    }

    size_t total_size = size + PREFIX_SIZE;
    size_t alloc_size;

    void *raw_ptr = NULL;
    int used_slab = 0;

    /* Slab path (8B-64KB): always go through the slab allocator. */
    if (should_use_slab(size)) {
        raw_ptr = numa_slab_alloc(size, target_node, &alloc_size);
        if (raw_ptr) used_slab = 1;
    }

    /* Direct path (>64KB or slab failure): check the cache first, then allocate via NUMA. */
    if (!raw_ptr) {
        if (total_size >= DIRECT_CACHE_MIN_SIZE && total_size <= DIRECT_CACHE_MAX_SIZE) {
            direct_cache_ensure_init();
            raw_ptr = direct_cache_pop(target_node, total_size);
        }
        if (!raw_ptr) {
            raw_ptr = numa_alloc_onnode(total_size, target_node);
        }
        alloc_size = total_size;
    }

    if (!raw_ptr)
        return NULL;

    /* Instrumentation: update the counters by path. */
    if (used_slab) {
        atomicIncr(numa_alloc_slab_bytes, total_size);
        atomicIncr(numa_alloc_slab_count, 1);
    } else {
        atomicIncr(numa_alloc_direct_bytes, total_size);
        atomicIncr(numa_alloc_direct_count, 1);
    }

    /* Record whether the allocation came from a slab (used for free routing). */
    int from_slab = (should_use_slab(size) && used_slab) ? 1 : 0;

    numa_init_prefix(raw_ptr, size, from_slab, target_node);
    update_zmalloc_stat_alloc(total_size);
    if (target_node >= 0 && target_node < ZMALLOC_MAX_NUMA_NODES)
        atomicIncr(used_memory_node[target_node], total_size);
    if (should_use_slab(size))
        atomicIncr(numa_tcache_alloc_miss, 1);
    return numa_to_user_ptr(raw_ptr);
}

static int numa_select_allocation_node(size_t size)
{
    if (tls_current_node >= 0) {
        return tls_current_node;
    } else if (numa_ctx.num_nodes <= 1) {
        return 0;
    } else {
        return numa_config_get_best_node(size);
    }
}

static void *numa_alloc_with_size(size_t size)
{
    return numa_alloc_with_size_onnode(size, numa_select_allocation_node(size));
}

/* NUMA-aware memory free (with size tracking): route to slab or direct based on the PREFIX. */
static void numa_free_with_size(void *user_ptr)
{
    if (user_ptr == NULL)
        return;

    numa_alloc_prefix_t *prefix = numa_get_prefix(user_ptr);
    size_t total_size = prefix->size + PREFIX_SIZE;
    int node_id = (int)prefix->node_id;

    /* ── tcache fast path: cache slab objects instead of returning to slab ── */
    if (prefix->from_pool && g_class_lookup_ready) {
        direct_cache_ensure_init();
        int cls = fast_size_class(prefix->size);
        if (cls >= 0) {
            tcache_bin_t *bin = &tls_tcache.bins[cls];
            if (bin->count >= TCACHE_BIN_MAX) {
                tcache_drain_bin(cls);
            }
            bin->ptrs[bin->count++] = user_ptr;
            atomicIncr(numa_tcache_free_hit, 1);
            /* tcache put: the object is no longer live from Redis' point of
             * view.  Release the logical used-memory counters immediately;
             * otherwise maxmemory accounting sees phantom live bytes and the
             * eviction loop ends in OOM even though plenty of blocks were
             * recycled.  RSS accounting remains independent of this counter. */
            update_zmalloc_stat_free(total_size);
            if (node_id >= 0 && node_id < ZMALLOC_MAX_NUMA_NODES)
                atomicDecr(used_memory_node[node_id], total_size);
            return;
        }
    }

    /* Non-tcache path: decrement the statistics immediately. */
    update_zmalloc_stat_free(total_size);
    if (node_id >= 0 && node_id < ZMALLOC_MAX_NUMA_NODES)
        atomicDecr(used_memory_node[node_id], total_size);

    void *raw_ptr = (char *)user_ptr - PREFIX_SIZE;

    /* Slab path: return to the slab. */
    if (prefix->from_pool) {
        numa_slab_free(raw_ptr, total_size, node_id);
        atomicDecr(numa_alloc_slab_bytes, total_size);
        atomicDecr(numa_alloc_slab_count, 1);
    } else {
        /* Direct path: try caching instead of freeing immediately. */
        if (total_size >= DIRECT_CACHE_MIN_SIZE &&
            total_size <= DIRECT_CACHE_MAX_SIZE) {
            direct_cache_ensure_init();
            direct_cache_push(raw_ptr, total_size, node_id);
        } else {
            numa_free(raw_ptr, total_size);
        }
        atomicDecr(numa_alloc_direct_bytes, total_size);
        atomicDecr(numa_alloc_direct_count, 1);
    }
}

/* Flush all tcache bins back to slab (call before thread exit or cleanup) */
void numa_tcache_flush(void)
{
    if (!tls_tcache_inited) return;
    for (int cls = 0; cls < NUMA_POOL_SIZE_CLASSES; cls++) {
        while (tls_tcache.bins[cls].count > 0) {
            tcache_drain_bin(cls);
        }
    }
    /* Flush direct cache: return all cached large blocks to OS */
    for (int i = 0; i < tls_direct_cache.count; i++) {
        numa_free(tls_direct_cache.ptrs[i], tls_direct_cache.sizes[i]);
    }
    tls_direct_cache.count = 0;
}

/* NUMA-aware zmalloc: trigger the OOM handler on allocation failure. */
void *numa_zmalloc(size_t size)
{
    void *ptr = numa_alloc_with_size(size);
    if (!ptr && size > 0)
        zmalloc_oom_handler(size);
    return ptr;
}

/* NUMA-aware zcalloc: allocate and zero. */
void *numa_zcalloc(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    void *ptr = numa_alloc_with_size(size);
    if (!ptr)
    {
        if (size > 0)
            zmalloc_oom_handler(size);
        return NULL;
    }

    memset(ptr, 0, size);
    return ptr;
}

/* NUMA-aware zrealloc: reallocate keeping the old data, return NULL on failure. */
static void *numa_tryrealloc(void *ptr, size_t size)
{
    /* Handle edge cases. */
    if (ptr == NULL)
        return numa_zmalloc(size);
    if (size == 0)
    {
        numa_zfree(ptr);
        return NULL;
    }

    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    size_t old_size = prefix->size;
    int old_node = (int)prefix->node_id;
    size_t old_total = old_size + PREFIX_SIZE;
    size_t new_total = size + PREFIX_SIZE;
    size_t old_class = should_use_slab(old_size) ? numa_slab_class_size(old_size) : 0;
    size_t new_class = should_use_slab(size) ? numa_slab_class_size(size) : 0;

    if (prefix->from_pool && should_use_slab(size) && old_class == new_class) {
        update_zmalloc_stat_free(old_total);
        update_zmalloc_stat_alloc(new_total);
        if (old_node >= 0 && old_node < ZMALLOC_MAX_NUMA_NODES) {
            atomicDecr(used_memory_node[old_node], old_total);
            atomicIncr(used_memory_node[old_node], new_total);
        }
        prefix->size = size;
        return ptr;
    }

    if (!prefix->from_pool && !should_use_slab(size)) {
        void *raw_ptr = (char *)ptr - PREFIX_SIZE;
        void *new_raw = numa_realloc(raw_ptr, old_total, new_total);
        if (!new_raw) return NULL;
        numa_alloc_prefix_t *new_prefix = (numa_alloc_prefix_t *)new_raw;
        new_prefix->size = size;
        update_zmalloc_stat_free(old_total);
        update_zmalloc_stat_alloc(new_total);
        if (old_node >= 0 && old_node < ZMALLOC_MAX_NUMA_NODES) {
            atomicDecr(used_memory_node[old_node], old_total);
            atomicIncr(used_memory_node[old_node], new_total);
        }
        return numa_to_user_ptr(new_raw);
    }

    void *new_ptr = numa_alloc_with_size_onnode(size, old_node);
    if (!new_ptr) return NULL;

    size_t copy_size = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);
    numa_free_with_size(ptr);

    return new_ptr;
}

void *numa_zrealloc(void *ptr, size_t size)
{
    void *result = numa_tryrealloc(ptr, size);
    if (!result && size != 0)
        zmalloc_oom_handler(size);
    return result;
}

/* NUMA-aware zfree. */
void numa_zfree(void *ptr)
{
    numa_free_with_size(ptr);
}

/* Set the NUMA node used for the current allocations. */
void numa_set_current_node(int node)
{
    if (node >= 0 && node < numa_ctx.num_nodes) {
        numa_ctx.current_node = node;
        tls_current_node = node;
    }
}

/* Get the current NUMA node. */
int numa_get_current_node(void)
{
    if (tls_current_node >= 0) {
        return tls_current_node;
    }
    return numa_ctx.current_node;
}

void numa_alloc_push_node(int node) {
    tls_current_node = node;
}

void numa_alloc_pop_node(void) {
    tls_current_node = -1;
}

/* Allocate memory on a specific NUMA node (used for key migration, bypassing the pool/slab). */
static void *numa_alloc_on_specific_node(size_t size, int node)
{
    return numa_alloc_with_size_onnode(size, node);
}

/* Allocate memory on a specific NUMA node (public interface). */
void *numa_zmalloc_onnode(size_t size, int node)
{
    if (node < 0 || node >= numa_ctx.num_nodes)
        return NULL;

    void *ptr = numa_alloc_on_specific_node(size, node);
    if (!ptr && size > 0)
        zmalloc_oom_handler(size);
    return ptr;
}

/* Reallocate memory on a specific NUMA node. */
void *numa_zrealloc_onnode(void *ptr, size_t size, int node)
{
    if (ptr == NULL)
        return numa_zmalloc_onnode(size, node);
    if (size == 0) {
        numa_zfree(ptr);
        return NULL;
    }
    if (node < 0 || node >= numa_ctx.num_nodes)
        return NULL;

    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    size_t old_size = prefix->size;
    int old_node = (int)prefix->node_id;
    size_t old_total = old_size + PREFIX_SIZE;
    size_t new_total = size + PREFIX_SIZE;
    size_t old_class = should_use_slab(old_size) ? numa_slab_class_size(old_size) : 0;
    size_t new_class = should_use_slab(size) ? numa_slab_class_size(size) : 0;

    if (old_node == node && prefix->from_pool && should_use_slab(size) && old_class == new_class) {
        update_zmalloc_stat_free(old_total);
        update_zmalloc_stat_alloc(new_total);
        if (old_node >= 0 && old_node < ZMALLOC_MAX_NUMA_NODES) {
            atomicDecr(used_memory_node[old_node], old_total);
            atomicIncr(used_memory_node[old_node], new_total);
        }
        prefix->size = size;
        return ptr;
    }

    if (old_node == node && !prefix->from_pool && !should_use_slab(size)) {
        void *raw_ptr = (char *)ptr - PREFIX_SIZE;
        void *new_raw = numa_realloc(raw_ptr, old_total, new_total);
        if (!new_raw) {
            zmalloc_oom_handler(size);
            return NULL;
        }
        numa_alloc_prefix_t *new_prefix = (numa_alloc_prefix_t *)new_raw;
        new_prefix->size = size;
        update_zmalloc_stat_free(old_total);
        update_zmalloc_stat_alloc(new_total);
        if (old_node >= 0 && old_node < ZMALLOC_MAX_NUMA_NODES) {
            atomicDecr(used_memory_node[old_node], old_total);
            atomicIncr(used_memory_node[old_node], new_total);
        }
        return numa_to_user_ptr(new_raw);
    }

    void *new_ptr = numa_alloc_with_size_onnode(size, node);
    if (!new_ptr) {
        zmalloc_oom_handler(size);
        return NULL;
    }

    size_t copy_size = (old_size < size) ? old_size : size;
    memcpy(new_ptr, ptr, copy_size);
    numa_free_with_size(ptr);
    return new_ptr;
}

/* Allocate and zero memory on a specific NUMA node. */
void *numa_zcalloc_onnode(size_t size, int node)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    void *ptr = numa_alloc_on_specific_node(size, node);
    if (!ptr)
    {
        if (size > 0)
            zmalloc_oom_handler(size);
        return NULL;
    }

    memset(ptr, 0, size);
    return ptr;
}

/* ========== Forced local node (Node 0 / DRAM) allocation ========== */

/* Internal function: shares the slab-to-direct path with numa_alloc_with_size(), target_node fixed to 0. */
static void *numa_alloc_dram(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);

    size_t total_size = size + PREFIX_SIZE;
    size_t alloc_size;
    int target_node = 0;

    void *raw_ptr = NULL;
    int used_slab = 0;

    if (should_use_slab(size)) {
        raw_ptr = numa_slab_alloc(size, target_node, &alloc_size);
        if (raw_ptr) used_slab = 1;
    }

    if (!raw_ptr) {
        if (total_size >= DIRECT_CACHE_MIN_SIZE && total_size <= DIRECT_CACHE_MAX_SIZE) {
            direct_cache_ensure_init();
            raw_ptr = direct_cache_pop(target_node, total_size);
        }
        if (!raw_ptr) {
            raw_ptr = numa_alloc_onnode(total_size, target_node);
        }
        alloc_size = total_size;
    }

    if (!raw_ptr)
        return NULL;

    if (used_slab) {
        atomicIncr(numa_alloc_slab_bytes, total_size);
        atomicIncr(numa_alloc_slab_count, 1);
    } else {
        atomicIncr(numa_alloc_direct_bytes, total_size);
        atomicIncr(numa_alloc_direct_count, 1);
    }

    int from_slab = (should_use_slab(size) && used_slab) ? 1 : 0;

    numa_init_prefix(raw_ptr, size, from_slab, target_node);
    update_zmalloc_stat_alloc(total_size);
    if (target_node >= 0 && target_node < ZMALLOC_MAX_NUMA_NODES)
        atomicIncr(used_memory_node[target_node], total_size);
    return numa_to_user_ptr(raw_ptr);
}

void *zmalloc_local(size_t size)
{
    void *ptr = numa_alloc_dram(size);
    if (!ptr && size > 0)
        zmalloc_oom_handler(size);
    return ptr;
}

void *zcalloc_local(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = numa_alloc_dram(size);
    if (!ptr) {
        if (size > 0)
            zmalloc_oom_handler(size);
        return NULL;
    }
    memset(ptr, 0, size);
    return ptr;
}

void *ztrycalloc_local(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);
    void *ptr = numa_alloc_dram(size);
    if (!ptr)
        return NULL;
    memset(ptr, 0, size);
    return ptr;
}

/* ========== NUMA hotness tracking API ========== */

/* Read the hotness level from a user pointer. */
uint8_t numa_get_hotness(void *ptr)
{
    if (!ptr) return NUMA_HOTNESS_MIN;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return prefix->hotness;
}

/* Set the hotness level of the memory pointed to by a user pointer. */
void numa_set_hotness(void *ptr, uint8_t hotness)
{
    if (!ptr) return;
    if (hotness > NUMA_HOTNESS_MAX) hotness = NUMA_HOTNESS_MAX;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->hotness = hotness;
}

/* Get the access counter. */
uint8_t numa_get_access_count(void *ptr)
{
    if (!ptr) return 0;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return prefix->access_count;
}

/* Increment the access counter. */
void numa_increment_access_count(void *ptr)
{
    if (!ptr) return;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->access_count++;
}

/* Get the last access time (LRU clock). */
uint16_t numa_get_last_access(void *ptr)
{
    if (!ptr) return 0;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return prefix->last_access;
}

/* Set the last access time. */
void numa_set_last_access(void *ptr, uint16_t lru_clock)
{
    if (!ptr) return;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->last_access = lru_clock;
}

/* Get the NUMA node ID where the allocation lives. */
int numa_get_node_id(void *ptr)
{
    if (!ptr) return -1;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return (int)prefix->node_id;
}

/* Set the NUMA node ID of an allocation (used to update the flag after migration). */
void numa_set_node_id(void *ptr, int node_id)
{
    if (!ptr) return;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->node_id = (char)node_id;
}

int numa_get_migrated(void *ptr)
{
    if (!ptr) return 0;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    return (int)prefix->migrated;
}

void numa_set_migrated(void *ptr, int migrated)
{
    if (!ptr) return;
    numa_alloc_prefix_t *prefix = numa_get_prefix(ptr);
    prefix->migrated = (char)migrated;
}

/* Read the current bytes and cumulative allocation counts per path. */
void numa_get_alloc_stats(size_t *slab_bytes, size_t *pool_bytes,
                          size_t *direct_bytes,
                          size_t *slab_count, size_t *pool_count,
                          size_t *direct_count)
{
    atomicGet(numa_alloc_slab_bytes,   *slab_bytes);
    atomicGet(numa_alloc_direct_bytes, *direct_bytes);
    atomicGet(numa_alloc_slab_count,   *slab_count);
    atomicGet(numa_alloc_direct_count, *direct_count);
    /* The pool path was removed, return 0. */
    *pool_bytes = 0;
    *pool_count = 0;
}

void numa_get_direct_cache_stats(size_t *hit, size_t *miss, size_t *evict)
{
    atomicGet(numa_direct_cache_hit,   *hit);
    atomicGet(numa_direct_cache_miss,  *miss);
    atomicGet(numa_direct_cache_evict, *evict);
}

#endif /* HAVE_NUMA */

#ifdef HAVE_MALLOC_SIZE
void *extend_to_usable(void *ptr, size_t size) {
    UNUSED(size);
    return ptr;
}
#endif

/* Try allocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
static inline void *ztrymalloc_usable_internal(size_t size, size_t *usable) {
    /* Possible overflow, return NULL, so that the caller can panic or handle a failed allocation. */
    if (size >= SIZE_MAX/2) return NULL;

#ifdef HAVE_NUMA
    /* Use the NUMA allocator when NUMA is available. */
    if (numa_ctx.numa_available)
    {
        void *ptr = numa_alloc_with_size(size);
        if (!ptr)
            return NULL;
        if (usable)
            *usable = size;
        return ptr;
    }
#endif

    /* Fall back to the standard allocator. */
    void *ptr = malloc(MALLOC_MIN_SIZE(size) + PREFIX_SIZE);
    if (!ptr) return NULL;

#ifdef HAVE_MALLOC_SIZE
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
    if (usable)
        *usable = size;
    return ptr;
#else
    *((size_t *)ptr) = size;
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    if (usable)
        *usable = size;
    return (char *)ptr + PREFIX_SIZE;
#endif
}

void *ztrymalloc_usable(size_t size, size_t *usable) {
    size_t usable_size = 0;
    void *ptr = ztrymalloc_usable_internal(size, &usable_size);
#ifdef HAVE_MALLOC_SIZE
    ptr = extend_to_usable(ptr, usable_size);
#endif
    if (usable) *usable = usable_size;
    return ptr;
}

/* Allocate memory or panic */
void *zmalloc(size_t size) {
    void *ptr = ztrymalloc_usable_internal(size, NULL);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
void *ztrymalloc(size_t size) {
    void *ptr = ztrymalloc_usable_internal(size, NULL);
    return ptr;
}

/* Allocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zmalloc_usable(size_t size, size_t *usable) {
    size_t usable_size = 0;
    void *ptr = ztrymalloc_usable_internal(size, &usable_size);
    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    ptr = extend_to_usable(ptr, usable_size);
#endif
    if (usable) *usable = usable_size;
    return ptr;
}

/* Allocate/free functions that bypass the thread cache and operate directly on the arena.
 * Currently implemented only for jemalloc, used for online defragmentation. */
#ifdef HAVE_DEFRAG
void *zmalloc_no_tcache(size_t size) {
    if (size >= SIZE_MAX/2) zmalloc_oom_handler(size);
    void *ptr = mallocx(size+PREFIX_SIZE, MALLOCX_TCACHE_NONE);
    if (!ptr) zmalloc_oom_handler(size);
    update_zmalloc_stat_alloc(zmalloc_size(ptr));
    return ptr;
}

void zfree_no_tcache(void *ptr)
{
    if (ptr == NULL)
        return;
    update_zmalloc_stat_free(zmalloc_size(ptr));
    dallocx(ptr, MALLOCX_TCACHE_NONE);
}
#endif

/* Try allocating memory and zero it, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
static inline void *ztrycalloc_usable_internal(size_t size, size_t *usable) {
    /* Possible overflow, return NULL, so that the caller can panic or handle a failed allocation. */
    if (size >= SIZE_MAX/2) return NULL;

#ifdef HAVE_NUMA
    /* Use the NUMA allocator when NUMA is available. */
    if (numa_ctx.numa_available)
    {
        void *ptr = numa_alloc_with_size(size);
        if (!ptr)
            return NULL;
        memset(ptr, 0, size);
        if (usable)
            *usable = size;
        return ptr;
    }
#endif

    void *ptr = calloc(1, MALLOC_MIN_SIZE(size) + PREFIX_SIZE);
    if (ptr == NULL)
        return NULL;

#ifdef HAVE_MALLOC_SIZE
    size = zmalloc_size(ptr);
    update_zmalloc_stat_alloc(size);
    if (usable)
        *usable = size;
    return ptr;
#else
    *((size_t *)ptr) = size;
    update_zmalloc_stat_alloc(size + PREFIX_SIZE);
    if (usable)
        *usable = size;
    return (char *)ptr + PREFIX_SIZE;
#endif
}

void *ztrycalloc_usable(size_t size, size_t *usable) {
    size_t usable_size = 0;
    void *ptr = ztrycalloc_usable_internal(size, &usable_size);
#ifdef HAVE_MALLOC_SIZE
    ptr = extend_to_usable(ptr, usable_size);
#endif
    if (usable) *usable = usable_size;
    return ptr;
}

/* Allocate memory and zero it or panic.
 * We need this wrapper to have a calloc compatible signature */
void *zcalloc_num(size_t num, size_t size) {
    /* Ensure that the arguments to calloc(), when multiplied, do not wrap.
     * Division operations are susceptible to divide-by-zero errors so we also check it. */
    if ((size == 0) || (num > SIZE_MAX/size)) {
        zmalloc_oom_handler(SIZE_MAX);
        return NULL;
    }
    void *ptr = ztrycalloc_usable_internal(num*size, NULL);
    if (!ptr) zmalloc_oom_handler(num*size);
    return ptr;
}

/* Allocate memory and zero it or panic */
void *zcalloc(size_t size) {
    void *ptr = ztrycalloc_usable_internal(size, NULL);
    if (!ptr) zmalloc_oom_handler(size);
    return ptr;
}

/* Try allocating memory, and return NULL if failed. */
void *ztrycalloc(size_t size) {
    void *ptr = ztrycalloc_usable_internal(size, NULL);
    return ptr;
}

/* Allocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zcalloc_usable(size_t size, size_t *usable) {
    size_t usable_size = 0;
    void *ptr = ztrycalloc_usable_internal(size, &usable_size);
    if (!ptr) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    ptr = extend_to_usable(ptr, usable_size);
#endif
    if (usable) *usable = usable_size;
    return ptr;
}

/* Try reallocating memory, and return NULL if failed.
 * '*usable' is set to the usable size if non NULL. */
static inline void *ztryrealloc_usable_internal(void *ptr, size_t size, size_t *usable) {
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
#endif
    size_t oldsize;
    void *newptr;

    /* not allocating anything, just redirect to free. */
    if (size == 0 && ptr != NULL) {
        zfree(ptr);
        if (usable) *usable = 0;
        return NULL;
    }
    /* Not freeing anything, just redirect to malloc. */
    if (ptr == NULL)
        return ztrymalloc_usable(size, usable);

    /* Possible overflow, return NULL, so that the caller can panic or handle a failed allocation. */
    if (size >= SIZE_MAX/2) {
        zfree(ptr);
        if (usable) *usable = 0;
        return NULL;
    }

#ifdef HAVE_NUMA
    /* Use the NUMA realloc when NUMA is available. */
    if (numa_ctx.numa_available)
    {
        void *result = numa_tryrealloc(ptr, size);
        if (result && usable)
            *usable = size;  /* NUMA allocator returns exact requested size */
        return result;
    }
#endif

#ifdef HAVE_MALLOC_SIZE
    oldsize = zmalloc_size(ptr);
    newptr = realloc(ptr,size);
    if (newptr == NULL) {
        if (usable) *usable = 0;
        return NULL;
    }

    update_zmalloc_stat_free(oldsize);
    size = zmalloc_size(newptr);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return newptr;
#else
    realptr = (char*)ptr-PREFIX_SIZE;
    oldsize = *((size_t*)realptr);
    newptr = realloc(realptr,size+PREFIX_SIZE);
    if (newptr == NULL) {
        if (usable) *usable = 0;
        return NULL;
    }

    *((size_t*)newptr) = size;
    update_zmalloc_stat_free(oldsize);
    update_zmalloc_stat_alloc(size);
    if (usable) *usable = size;
    return (char*)newptr+PREFIX_SIZE;
#endif
}

void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable) {
    size_t usable_size = 0;
    ptr = ztryrealloc_usable_internal(ptr, size, &usable_size);
#ifdef HAVE_MALLOC_SIZE
    ptr = extend_to_usable(ptr, usable_size);
#endif
    if (usable) *usable = usable_size;
    return ptr;
}

/* Reallocate memory and zero it or panic */
void *zrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable_internal(ptr, size, NULL);
    if (!ptr && size != 0) zmalloc_oom_handler(size);
    return ptr;
}

/* Try Reallocating memory, and return NULL if failed. */
void *ztryrealloc(void *ptr, size_t size) {
    ptr = ztryrealloc_usable_internal(ptr, size, NULL);
    return ptr;
}

/* Reallocate memory or panic.
 * '*usable' is set to the usable size if non NULL. */
void *zrealloc_usable(void *ptr, size_t size, size_t *usable) {
    size_t usable_size = 0;
    ptr = ztryrealloc_usable(ptr, size, &usable_size);
    if (!ptr && size != 0) zmalloc_oom_handler(size);
#ifdef HAVE_MALLOC_SIZE
    ptr = extend_to_usable(ptr, usable_size);
#endif
    if (usable) *usable = usable_size;
    return ptr;
}

void zfree(void *ptr)
{
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL)
        return;

#ifdef HAVE_NUMA
    /* Use the NUMA free path when NUMA is available. */
    if (numa_ctx.numa_available)
    {
        numa_zfree(ptr);
        return;
    }
#endif

#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char *)ptr - PREFIX_SIZE;
    oldsize = *((size_t *)realptr);
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    free(realptr);
#endif
}

/* Like zfree, but also returns the actual size of the freed memory via usable. */
void zfree_usable(void *ptr, size_t *usable)
{
#ifndef HAVE_MALLOC_SIZE
    void *realptr;
    size_t oldsize;
#endif

    if (ptr == NULL)
        return;
#ifdef HAVE_NUMA
    /* The NUMA allocator must use the NUMA free path: memory from slabs or
     * numa_alloc_onnode cannot be released with libc free(), that is UB. */
    if (numa_ctx.numa_available)
    {
        if (usable)
            *usable = numa_get_prefix(ptr)->size;
        numa_zfree(ptr);
        return;
    }
#endif
#ifdef HAVE_MALLOC_SIZE
    update_zmalloc_stat_free(*usable = zmalloc_size(ptr));
    free(ptr);
#else
    realptr = (char *)ptr - PREFIX_SIZE;
    *usable = oldsize = *((size_t *)realptr);
    update_zmalloc_stat_free(oldsize + PREFIX_SIZE);
    free(realptr);
#endif
}

char *zstrdup(const char *s)
{
    size_t l = strlen(s) + 1;
    char *p = zmalloc(l);

    memcpy(p, s, l);
    return p;
}

size_t zmalloc_used_memory(void)
{
    size_t um;
    atomicGet(used_memory, um);
    return um;
}

#ifdef HAVE_NUMA
size_t zmalloc_used_memory_node(int node_id)
{
    size_t um;
    if (node_id < 0 || node_id >= ZMALLOC_MAX_NUMA_NODES) return 0;
    atomicGet(used_memory_node[node_id], um);
    return um;
}
#endif

void zmalloc_set_oom_handler(void (*oom_handler)(size_t))
{
    zmalloc_oom_handler = oom_handler;
}

/* Use 'MADV_DONTNEED' to release memory to operating system quickly.
 * We do that in a fork child process to avoid CoW when the parent modifies
 * these shared pages. */
void zmadvise_dontneed(void *ptr) {
#if defined(USE_JEMALLOC) && defined(__linux__)
    static size_t page_size = 0;
    if (page_size == 0) page_size = sysconf(_SC_PAGESIZE);
    size_t page_size_mask = page_size - 1;

    size_t real_size = zmalloc_size(ptr);
    if (real_size < page_size) return;

    /* We need to align the pointer upwards according to page size, because
     * the memory address is increased upwards and we only can free memory
     * based on page. */
    char *aligned_ptr = (char *)(((size_t)ptr+page_size_mask) & ~page_size_mask);
    real_size -= (aligned_ptr-(char*)ptr);
    if (real_size >= page_size) {
        madvise((void *)aligned_ptr, real_size&~page_size_mask, MADV_DONTNEED);
    }
#else
    (void)(ptr);
#endif
}

/* Get the RSS information in an OS-specific way.
 *
 * WARNING: the function zmalloc_get_rss() is not designed to be fast
 * and may not be called in the busy loops where Redis tries to release
 * memory expiring or swapping out objects.
 *
 * Warning: this function is not designed for speed and must not be called on
 * the hot path of Redis eviction/swap-out. Use RedisEstimateRSS() for a quick
 * RSS estimate (faster but less accurate). */

#if defined(HAVE_PROC_STAT)
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

/* Get the i'th field from "/proc/self/stats" note i is 1 based as appears in the 'proc' man page */
int get_proc_stat_ll(int i, long long *res) {
#if defined(HAVE_PROC_STAT)
    char buf[4096];
    int fd, l;
    char *p, *x;

    if ((fd = open("/proc/self/stat",O_RDONLY)) == -1) return 0;
    if ((l = read(fd,buf,sizeof(buf)-1)) <= 0) {
        close(fd);
        return 0;
    }
    close(fd);
    buf[l] = '\0';
    if (buf[l-1] == '\n') buf[l-1] = '\0';

    /* Skip pid and process name (surrounded with parentheses) */
    p = strrchr(buf, ')');
    if (!p) return 0;
    p++;
    while (*p == ' ') p++;
    if (*p == '\0') return 0;
    i -= 3;
    if (i < 0) return 0;

    while (p && i--) {
        p = strchr(p, ' ');
        if (p) p++;
        else return 0;
    }
    x = strchr(p,' ');
    if (x) *x = '\0';

    *res = strtoll(p,&x,10);
    if (*x != '\0') return 0;
    return 1;
#else
    UNUSED(i);
    UNUSED(res);
    return 0;
#endif
}

#if defined(HAVE_PROC_STAT)
size_t zmalloc_get_rss(void) {
    int page = sysconf(_SC_PAGESIZE);
    long long rss;

    /* RSS is the 24th field in /proc/<pid>/stat */
    if (!get_proc_stat_ll(24, &rss)) return 0;
    rss *= page;
    return rss;
}
#elif defined(HAVE_TASKINFO)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <mach/task.h>
#include <mach/mach_init.h>

size_t zmalloc_get_rss(void)
{
    task_t task = MACH_PORT_NULL;
    struct task_basic_info t_info;
    mach_msg_type_number_t t_info_count = TASK_BASIC_INFO_COUNT;

    if (task_for_pid(current_task(), getpid(), &task) != KERN_SUCCESS)
        return 0;
    task_info(task, TASK_BASIC_INFO, (task_info_t)&t_info, &t_info_count);

    return t_info.resident_size;
}
#elif defined(__FreeBSD__) || defined(__DragonFly__)
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

size_t zmalloc_get_rss(void)
{
    struct kinfo_proc info;
    size_t infolen = sizeof(info);
    int mib[4];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();

    if (sysctl(mib, 4, &info, &infolen, NULL, 0) == 0)
#if defined(__FreeBSD__)
        return (size_t)info.ki_rssize * getpagesize();
#else
        return (size_t)info.kp_vm_rssize * getpagesize();
#endif

    return 0L;
}
#elif defined(__NetBSD__) || defined(__OpenBSD__)
#include <sys/types.h>
#include <sys/sysctl.h>

#if defined(__OpenBSD__)
#define kinfo_proc2 kinfo_proc
#define KERN_PROC2 KERN_PROC
#define __arraycount(a) (sizeof(a) / sizeof(a[0]))
#endif

size_t zmalloc_get_rss(void) {
    struct kinfo_proc2 info;
    size_t infolen = sizeof(info);
    int mib[6];
    mib[0] = CTL_KERN;
    mib[1] = KERN_PROC2;
    mib[2] = KERN_PROC_PID;
    mib[3] = getpid();
    mib[4] = sizeof(info);
    mib[5] = 1;
    if (sysctl(mib, __arraycount(mib), &info, &infolen, NULL, 0) == 0)
        return (size_t)info.p_vm_rssize * getpagesize();

    return 0L;
}
#elif defined(__HAIKU__)
#include <OS.h>

size_t zmalloc_get_rss(void) {
    area_info info;
    thread_info th;
    size_t rss = 0;
    ssize_t cookie = 0;

    if (get_thread_info(find_thread(0), &th) != B_OK)
        return 0;

    while (get_next_area_info(th.team, &cookie, &info) == B_OK)
        rss += info.ram_size;

    return rss;
}
#elif defined(HAVE_PSINFO)
#include <unistd.h>
#include <sys/procfs.h>
#include <fcntl.h>

size_t zmalloc_get_rss(void)
{
    struct prpsinfo info;
    char filename[256];
    int fd;

    snprintf(filename, 256, "/proc/%ld/psinfo", (long)getpid());

    if ((fd = open(filename, O_RDONLY)) == -1)
        return 0;
    if (ioctl(fd, PIOCPSINFO, &info) == -1)
    {
        close(fd);
        return 0;
    }

    close(fd);
    return info.pr_rssize;
}
#else
size_t zmalloc_get_rss(void)
{
    /* If we can't get the RSS in an OS-specific way for this system just
     * return the memory usage we estimated in zmalloc()..
     *
     * Fragmentation will appear to be always 1 (no fragmentation)
     * of course... */
    return zmalloc_used_memory();
}
#endif

#if defined(USE_JEMALLOC)

int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident)
{
    uint64_t epoch = 1;
    size_t sz;
    *allocated = *resident = *active = 0;
    /* Update the statistics cached by mallctl. */
    sz = sizeof(epoch);
    je_mallctl("epoch", &epoch, &sz, &epoch, sz);
    sz = sizeof(size_t);
    /* Unlike RSS, this does not include RSS from shared libraries and other non
     * heap mappings. */
    je_mallctl("stats.resident", resident, &sz, NULL, 0);
    /* Unlike resident, this doesn't not include the pages jemalloc reserves
     * for re-use (purge will clean that). */
    je_mallctl("stats.active", active, &sz, NULL, 0);
    /* Unlike zmalloc_used_memory, this matches the stats.resident by taking
     * into account all allocations done by this process (not only zmalloc). */
    je_mallctl("stats.allocated", allocated, &sz, NULL, 0);
    return 1;
}

void set_jemalloc_bg_thread(int enable)
{
    /* let jemalloc do purging asynchronously, required when there's no traffic
     * after flushdb */
    char val = !!enable;
    je_mallctl("background_thread", NULL, 0, &val, 1);
}

int jemalloc_purge(void) {
    /* return all unused (reserved) pages to the OS */
    char tmp[32];
    unsigned narenas = 0;
    size_t sz = sizeof(unsigned);
    if (!je_mallctl("arenas.narenas", &narenas, &sz, NULL, 0)) {
        snprintf(tmp, sizeof(tmp), "arena.%d.purge", narenas);
        if (!je_mallctl(tmp, NULL, 0, NULL, 0))
            return 0;
    }
    return -1;
}

#else

int zmalloc_get_allocator_info(size_t *allocated,
                               size_t *active,
                               size_t *resident)
{
    *allocated = *resident = *active = 0;
    return 1;
}

void set_jemalloc_bg_thread(int enable)
{
    ((void)(enable));
}

int jemalloc_purge(void) {
    return 0;
}

#endif

#if defined(__APPLE__)
/* For proc_pidinfo() used later in zmalloc_get_smap_bytes_by_field().
 * Note that this file cannot be included in zmalloc.h because it includes
 * a Darwin queue.h file where there is a "LIST_HEAD" macro (!) defined
 * conficting with Redis user code. */
#include <libproc.h>
#endif

/* Read the total bytes of a given field from /proc/self/smaps (raw values are
 * in KB and converted to bytes automatically). The field name must include the
 * colon suffix, as in the smaps format. If pid is -1 the current process is
 * read, otherwise the given pid is used.
 * Example: zmalloc_get_smap_bytes_by_field("Rss:",-1) */
#if defined(HAVE_PROC_SMAPS)
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid)
{
    char line[1024];
    size_t bytes = 0;
    int flen = strlen(field);
    FILE *fp;

    if (pid == -1)
    {
        fp = fopen("/proc/self/smaps", "r");
    }
    else
    {
        char filename[128];
        snprintf(filename, sizeof(filename), "/proc/%ld/smaps", pid);
        fp = fopen(filename, "r");
    }

    if (!fp)
        return 0;
    while (fgets(line, sizeof(line), fp) != NULL)
    {
        if (strncmp(line, field, flen) == 0)
        {
            char *p = strchr(line, 'k');
            if (p)
            {
                *p = '\0';
                bytes += strtol(line + flen, NULL, 10) * 1024;
            }
        }
    }
    fclose(fp);
    return bytes;
}
#else
/* Get sum of the specified field from libproc api call.
 * As there are per page value basis we need to convert
 * them accordingly.
 *
 * Note that AnonHugePages is a no-op as THP feature
 * is not supported in this platform
 */
size_t zmalloc_get_smap_bytes_by_field(char *field, long pid)
{
#if defined(__APPLE__)
    struct proc_regioninfo pri;
    if (pid == -1)
        pid = getpid();
    if (proc_pidinfo(pid, PROC_PIDREGIONINFO, 0, &pri,
                     PROC_PIDREGIONINFO_SIZE) == PROC_PIDREGIONINFO_SIZE)
    {
        int pagesize = getpagesize();
        if (!strcmp(field, "Private_Dirty:"))
        {
            return (size_t)pri.pri_pages_dirtied * pagesize;
        }
        else if (!strcmp(field, "Rss:"))
        {
            return (size_t)pri.pri_pages_resident * pagesize;
        }
        else if (!strcmp(field, "AnonHugePages:"))
        {
            return 0;
        }
    }
    return 0;
#endif
    ((void)field);
    ((void)pid);
    return 0;
}
#endif

/* Get the total bytes of all pages marked as Private Dirty.
 *
 * Note: depending on the platform and process memory usage, this call can be
 * very slow, taking more than 1000ms! */
size_t zmalloc_get_private_dirty(long pid)
{
    return zmalloc_get_smap_bytes_by_field("Private_Dirty:", pid);
}

/* Get the physical memory (RAM) size in bytes.
 * Cross-platform implementation, see:
 * http://nadeausoftware.com/articles/2012/09/c_c_tip_how_get_physical_memory_size_system
 *
 * Copyright notes:
 * 1) Released under the CC Attribution license (http://creativecommons.org/licenses/by/3.0/deed.en_US)
 * 2) Original author: David Robert Nadeau
 * 3) Redis version modified by Matt Stancliff
 * 4) This comment is kept to comply with the original license requirements
 */
size_t zmalloc_get_memory_size(void)
{
#if defined(__unix__) || defined(__unix) || defined(unix) || \
    (defined(__APPLE__) && defined(__MACH__))
#if defined(CTL_HW) && (defined(HW_MEMSIZE) || defined(HW_PHYSMEM64))
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_MEMSIZE)
    mib[1] = HW_MEMSIZE; /* OSX. --------------------- */
#elif defined(HW_PHYSMEM64)
    mib[1] = HW_PHYSMEM64; /* NetBSD, OpenBSD. --------- */
#endif
    int64_t size = 0; /* 64-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L; /* Failed? */

#elif defined(_SC_PHYS_PAGES) && defined(_SC_PAGESIZE)
    /* FreeBSD, Linux, OpenBSD, and Solaris. -------------------- */
    return (size_t)sysconf(_SC_PHYS_PAGES) * (size_t)sysconf(_SC_PAGESIZE);

#elif defined(CTL_HW) && (defined(HW_PHYSMEM) || defined(HW_REALMEM))
    /* DragonFly BSD, FreeBSD, NetBSD, OpenBSD, and OSX. -------- */
    int mib[2];
    mib[0] = CTL_HW;
#if defined(HW_REALMEM)
    mib[1] = HW_REALMEM; /* FreeBSD. ----------------- */
#elif defined(HW_PHYSMEM)
    mib[1] = HW_PHYSMEM; /* Others. ------------------ */
#endif
    unsigned int size = 0; /* 32-bit */
    size_t len = sizeof(size);
    if (sysctl(mib, 2, &size, &len, NULL, 0) == 0)
        return (size_t)size;
    return 0L; /* Failed? */
#else
    return 0L; /* Unknown method to get the data. */
#endif
#else
    return 0L; /* Unknown OS. */
#endif
}

/* The NUMA allocator implements zmalloc_size itself (reads the size from the PREFIX). */
#ifdef HAVE_NUMA
size_t zmalloc_size(void *ptr)
{
    if (ptr == NULL)
        return 0;

    void *orig_ptr = (char *)ptr - PREFIX_SIZE;
    numa_alloc_prefix_t *prefix = (numa_alloc_prefix_t *)orig_ptr;
    return prefix->size;
}

size_t zmalloc_usable_size(void *ptr)
{
    return zmalloc_size(ptr);
}
#elif !defined(HAVE_MALLOC_SIZE)
size_t zmalloc_size(void *ptr)
{
    if (ptr == NULL)
        return 0;

    void *realptr = (char *)ptr - PREFIX_SIZE;
    size_t size = *((size_t *)realptr);
    return size;
}

size_t zmalloc_usable_size(void *ptr)
{
    return zmalloc_size(ptr);
}
#endif

#ifdef REDIS_TEST
int zmalloc_test(int argc, char **argv, int flags) {
    void *ptr;

    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);
    printf("Malloc prefix size: %d\n", (int) PREFIX_SIZE);
    printf("Initial used memory: %zu\n", zmalloc_used_memory());
    ptr = zmalloc(123);
    printf("Allocated 123 bytes; used: %lu\n", (unsigned long)zmalloc_used_memory());
    ptr = zrealloc(ptr, 456);
    printf("Reallocated to 456 bytes; used: %lu\n", (unsigned long)zmalloc_used_memory());
    zfree(ptr);
    printf("Freed pointer; used: %lu\n", (unsigned long)zmalloc_used_memory());
    return 0;
}
#endif
