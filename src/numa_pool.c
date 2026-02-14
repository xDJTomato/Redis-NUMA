/* numa_pool.c - NUMA-aware memory pool allocator implementation
 *
 * Implementation notes:
 * - Uses libc malloc/free for internal metadata (chunk headers, pool structures)
 * - Uses numa_alloc_onnode/numa_free for actual memory chunks
 * - NO printf/debug output in allocation path to avoid recursion
 * - Thread-safe via per-pool mutex locks
 */

#define _GNU_SOURCE
#include "numa_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <numa.h>
#include <sched.h>
#include <unistd.h>

/* Size classes for memory pool - expanded to 16 levels */
const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES] = {
    16, 32, 48, 64,          /* Fine-grained small objects */
    96, 128, 192, 256,       /* Medium-small objects */
    384, 512, 768, 1024,     /* Medium objects */
    1536, 2048, 3072, 4096   /* Large objects */
};

/* Get optimal chunk size based on object size */
size_t get_chunk_size_for_object(size_t obj_size) {
    if (obj_size <= 256) {
        return CHUNK_SIZE_SMALL;    /* 16KB for small objects */
    } else if (obj_size <= 1024) {
        return CHUNK_SIZE_MEDIUM;   /* 64KB for medium objects */
    } else if (obj_size <= 4096) {
        return CHUNK_SIZE_LARGE;    /* 256KB for large objects */
    } else {
        return 0;  /* Direct allocation for very large objects */
    }
}

/* Forward declarations */
typedef struct numa_pool_chunk numa_pool_chunk_t;
typedef struct free_block free_block_t;

/* P1 Optimization: Free block structure for free list management */
struct free_block {
    void *ptr;                     /* Pointer to freed memory */
    size_t size;                   /* Size of the freed block */
    struct free_block *next;       /* Next free block in list */
};

/* Memory pool chunk structure */
struct numa_pool_chunk {
    void *memory;                  /* NUMA-allocated memory */
    size_t size;                   /* Chunk size */
    size_t offset;                 /* Current allocation offset */
    size_t used_bytes;             /* Actually allocated bytes (P1: for utilization tracking) */
    struct numa_pool_chunk *next;  /* Next chunk in list */
};

/* Size class pool */
typedef struct {
    size_t obj_size;               /* Object size for this class */
    numa_pool_chunk_t *chunks;     /* Chunk list */
    free_block_t *free_list;       /* P1: Free list for this size class */
    pthread_mutex_t lock;          /* Thread safety */
    size_t chunks_count;           /* Statistics */
} numa_size_class_pool_t;

/* Per-node pool */
typedef struct numa_node_pool {
    int node_id;
    numa_size_class_pool_t pools[NUMA_POOL_SIZE_CLASSES];
    numa_pool_stats_t stats;
} numa_node_pool_t;

/* Global pool context */
static struct {
    int initialized;
    int numa_available;
    int num_nodes;
    int current_node;
    numa_node_pool_t *node_pools;
    pthread_mutex_t init_lock;
} pool_ctx = {
    .initialized = 0,
    .numa_available = 0,
    .num_nodes = 0,
    .current_node = 0,
    .node_pools = NULL
};

/* Thread-local current node */
static __thread int tls_current_node = -1;

/* Initialize memory pool system */
int numa_pool_init(void)
{
    pthread_mutex_lock(&pool_ctx.init_lock);
    
    if (pool_ctx.initialized) {
        pthread_mutex_unlock(&pool_ctx.init_lock);
        return 0;
    }
    
    /* Check NUMA availability */
    if (numa_available() == -1) {
        pool_ctx.numa_available = 0;
        pool_ctx.initialized = 1;
        pthread_mutex_unlock(&pool_ctx.init_lock);
        return 0;
    }
    
    pool_ctx.numa_available = 1;
    pool_ctx.num_nodes = numa_max_node() + 1;
    
    /* Get current node */
    int cpu = sched_getcpu();
    if (cpu >= 0) {
        pool_ctx.current_node = numa_node_of_cpu(cpu);
    } else {
        pool_ctx.current_node = 0;
    }
    tls_current_node = pool_ctx.current_node;
    
    /* Allocate node pools */
    pool_ctx.node_pools = calloc(pool_ctx.num_nodes, sizeof(numa_node_pool_t));
    if (!pool_ctx.node_pools) {
        pool_ctx.numa_available = 0;
        pool_ctx.initialized = 1;
        pthread_mutex_unlock(&pool_ctx.init_lock);
        return -1;
    }
    
    /* Initialize each node's pools */
    for (int i = 0; i < pool_ctx.num_nodes; i++) {
        pool_ctx.node_pools[i].node_id = i;
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            pool_ctx.node_pools[i].pools[j].obj_size = numa_pool_size_classes[j];
            pool_ctx.node_pools[i].pools[j].chunks = NULL;
            pool_ctx.node_pools[i].pools[j].free_list = NULL;  /* P1: Initialize free list */
            pool_ctx.node_pools[i].pools[j].chunks_count = 0;
            pthread_mutex_init(&pool_ctx.node_pools[i].pools[j].lock, NULL);
        }
        memset(&pool_ctx.node_pools[i].stats, 0, sizeof(numa_pool_stats_t));
    }
    
    pool_ctx.initialized = 1;
    pthread_mutex_unlock(&pool_ctx.init_lock);
    return 0;
}

/* Cleanup all memory pools */
void numa_pool_cleanup(void)
{
    pthread_mutex_lock(&pool_ctx.init_lock);
    
    if (!pool_ctx.initialized || !pool_ctx.node_pools) {
        pthread_mutex_unlock(&pool_ctx.init_lock);
        return;
    }
    
    for (int i = 0; i < pool_ctx.num_nodes; i++) {
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            numa_size_class_pool_t *pool = &pool_ctx.node_pools[i].pools[j];
            numa_pool_chunk_t *chunk = pool->chunks;
            
            pthread_mutex_lock(&pool->lock);
            while (chunk) {
                numa_pool_chunk_t *next = chunk->next;
                if (chunk->memory) {
                    numa_free(chunk->memory, chunk->size);
                }
                free(chunk);
                chunk = next;
            }
            pool->chunks = NULL;
            pthread_mutex_unlock(&pool->lock);
            pthread_mutex_destroy(&pool->lock);
        }
    }
    
    free(pool_ctx.node_pools);
    pool_ctx.node_pools = NULL;
    pool_ctx.initialized = 0;
    pool_ctx.numa_available = 0;
    
    pthread_mutex_unlock(&pool_ctx.init_lock);
}

/* Internal: Allocate a new chunk for a pool with dynamic sizing */
static numa_pool_chunk_t *alloc_new_chunk(int node, size_t obj_size)
{
    numa_pool_chunk_t *chunk = malloc(sizeof(numa_pool_chunk_t));
    if (!chunk) {
        return NULL;
    }
    
    /* Get optimal chunk size based on object size */
    size_t chunk_size = get_chunk_size_for_object(obj_size);
    if (chunk_size == 0) {
        /* Object too large for pooling, should use direct allocation */
        free(chunk);
        return NULL;
    }
    
    chunk->memory = numa_alloc_onnode(chunk_size, node);
    if (!chunk->memory) {
        free(chunk);
        return NULL;
    }
    
    chunk->size = chunk_size;
    chunk->offset = 0;
    chunk->used_bytes = 0;        /* P1: Initialize utilization tracking */
    chunk->next = NULL;
    
    return chunk;
}

/* Allocate memory from pool - Optimized fast path */
void *numa_pool_alloc(size_t size, int node, size_t *total_size)
{
    if (!pool_ctx.initialized) {
        return NULL;
    }
    
    if (node < 0 || node >= pool_ctx.num_nodes) {
        node = pool_ctx.current_node;
    }
    
    size_t alloc_size = size;
    int from_pool = 0;
    void *result = NULL;
    
    /* Try pool allocation for small sizes */
    if (alloc_size <= NUMA_POOL_MAX_ALLOC && pool_ctx.node_pools) {
        /* Fast size class lookup using binary search style */
        int class_idx = -1;
        if (alloc_size <= 64) {
            class_idx = (alloc_size <= 16) ? 0 : (alloc_size <= 32) ? 1 : (alloc_size <= 48) ? 2 : 3;
        } else if (alloc_size <= 256) {
            class_idx = (alloc_size <= 96) ? 4 : (alloc_size <= 128) ? 5 : (alloc_size <= 192) ? 6 : 7;
        } else if (alloc_size <= 1024) {
            class_idx = (alloc_size <= 384) ? 8 : (alloc_size <= 512) ? 9 : (alloc_size <= 768) ? 10 : 11;
        } else if (alloc_size <= 4096) {
            class_idx = (alloc_size <= 1536) ? 12 : (alloc_size <= 2048) ? 13 : (alloc_size <= 3072) ? 14 : 15;
        }
        
        if (class_idx >= 0) {
            numa_node_pool_t *node_pool = &pool_ctx.node_pools[node];
            numa_size_class_pool_t *pool = &node_pool->pools[class_idx];
            
            pthread_mutex_lock(&pool->lock);
            
            size_t aligned_size = (alloc_size + 15) & ~15;  /* 16-byte align */
            
            /* Fast path 1: Try free list head (O(1) reuse) */
            free_block_t *free_block = pool->free_list;
            if (free_block && free_block->size >= aligned_size) {
                result = free_block->ptr;
                pool->free_list = free_block->next;
                free(free_block);
                from_pool = 1;
            }
            
            /* Fast path 2: Try first chunk directly (hot cache) */
            if (!result) {
                numa_pool_chunk_t *chunk = pool->chunks;
                if (chunk && chunk->offset + aligned_size <= chunk->size) {
                    result = (char *)chunk->memory + chunk->offset;
                    chunk->offset += aligned_size;
                    chunk->used_bytes += aligned_size;
                    from_pool = 1;
                }
            }
            
            /* Slow path: Allocate new chunk if needed */
            if (!result) {
                numa_pool_chunk_t *new_chunk = alloc_new_chunk(node, alloc_size);
                if (new_chunk) {
                    result = new_chunk->memory;
                    new_chunk->offset = aligned_size;
                    new_chunk->used_bytes = aligned_size;
                    new_chunk->next = pool->chunks;
                    pool->chunks = new_chunk;
                    pool->chunks_count++;
                    from_pool = 1;
                }
            }
            
            pthread_mutex_unlock(&pool->lock);
            
            if (from_pool) {
                pool_ctx.node_pools[node].stats.pool_hits++;
                pool_ctx.node_pools[node].stats.total_from_pool += alloc_size;
            }
        }
    }
    
    /* Fall back to direct NUMA allocation */
    if (!result) {
        result = numa_alloc_onnode(alloc_size, node);
        from_pool = 0;
        if (result && pool_ctx.node_pools) {
            pool_ctx.node_pools[node].stats.pool_misses++;
            pool_ctx.node_pools[node].stats.total_direct += alloc_size;
        }
    }
    
    if (result && pool_ctx.node_pools) {
        pool_ctx.node_pools[node].stats.total_allocated += alloc_size;
    }
    
    if (total_size) {
        *total_size = alloc_size;
    }
    
    return result;
}

/* Free memory - P1: Add freed blocks to free list */
void numa_pool_free(void *ptr, size_t total_size, int from_pool)
{
    if (!ptr) {
        return;
    }
    
    if (!from_pool) {
        /* Only free direct NUMA allocations */
        numa_free(ptr, total_size);
        return;
    }
    
    /* P1: For pool allocations, add to free list */
    if (!pool_ctx.initialized || !pool_ctx.node_pools) {
        return;  /* Pool not initialized, just leak it */
    }
    
    /* Find the appropriate size class for this size */
    size_t aligned_size = (total_size + 15) & ~15;
    int class_idx = -1;
    
    for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {
        if (aligned_size <= numa_pool_size_classes[i]) {
            class_idx = i;
            break;
        }
    }
    
    if (class_idx < 0) {
        return;  /* Size doesn't match any class, skip */
    }
    
    /* We don't know which node, so try to find it or just use node 0 */
    /* This is a limitation but acceptable for now */
    int node = 0;
    if (pool_ctx.current_node >= 0 && pool_ctx.current_node < pool_ctx.num_nodes) {
        node = pool_ctx.current_node;
    }
    
    /* Create free block */
    free_block_t *free_block = malloc(sizeof(free_block_t));
    if (!free_block) {
        return;  /* Can't record free block, just leak it */
    }
    
    free_block->ptr = ptr;
    free_block->size = aligned_size;
    
    /* Add to pool's free list */
    numa_size_class_pool_t *pool = &pool_ctx.node_pools[node].pools[class_idx];
    
    pthread_mutex_lock(&pool->lock);
    free_block->next = pool->free_list;
    pool->free_list = free_block;
    pthread_mutex_unlock(&pool->lock);
}

/* Set current NUMA node */
void numa_pool_set_node(int node)
{
    if (node >= 0 && node < pool_ctx.num_nodes) {
        pool_ctx.current_node = node;
        tls_current_node = node;
    }
}

/* Get current NUMA node */
int numa_pool_get_node(void)
{
    if (tls_current_node >= 0) {
        return tls_current_node;
    }
    return pool_ctx.current_node;
}

/* Get number of NUMA nodes */
int numa_pool_num_nodes(void)
{
    return pool_ctx.num_nodes;
}

/* Check if NUMA is available */
int numa_pool_available(void)
{
    return pool_ctx.numa_available;
}

/* Get pool statistics */
void numa_pool_get_stats(int node, numa_pool_stats_t *stats)
{
    if (!stats || node < 0 || node >= pool_ctx.num_nodes || !pool_ctx.node_pools) {
        return;
    }
    
    *stats = pool_ctx.node_pools[node].stats;
}

/* Reset pool statistics */
void numa_pool_reset_stats(void)
{
    if (!pool_ctx.node_pools) {
        return;
    }
    
    for (int i = 0; i < pool_ctx.num_nodes; i++) {
        memset(&pool_ctx.node_pools[i].stats, 0, sizeof(numa_pool_stats_t));
    }
}

/* P1 Optimization: Get chunk utilization ratio */
float numa_pool_get_utilization(int node, int size_class_idx)
{
    if (!pool_ctx.initialized || !pool_ctx.node_pools) {
        return 0.0f;
    }
    
    if (node < 0 || node >= pool_ctx.num_nodes) {
        return 0.0f;
    }
    
    if (size_class_idx < 0 || size_class_idx >= NUMA_POOL_SIZE_CLASSES) {
        return 0.0f;
    }
    
    numa_size_class_pool_t *pool = &pool_ctx.node_pools[node].pools[size_class_idx];
    pthread_mutex_lock(&pool->lock);
    
    size_t total_size = 0;
    size_t used_bytes = 0;
    numa_pool_chunk_t *chunk = pool->chunks;
    
    while (chunk) {
        total_size += chunk->size;
        used_bytes += chunk->used_bytes;
        chunk = chunk->next;
    }
    
    pthread_mutex_unlock(&pool->lock);
    
    if (total_size == 0) {
        return 0.0f;
    }
    
    return (float)used_bytes / (float)total_size;
}

/* P1 Optimization: Try to compact low-utilization chunks */
int numa_pool_try_compact(void)
{
    if (!pool_ctx.initialized || !pool_ctx.node_pools) {
        return 0;
    }
    
    int compacted_count = 0;
    
    /* Iterate through all nodes and size classes */
    for (int node = 0; node < pool_ctx.num_nodes; node++) {
        for (int class_idx = 0; class_idx < NUMA_POOL_SIZE_CLASSES; class_idx++) {
            numa_size_class_pool_t *pool = &pool_ctx.node_pools[node].pools[class_idx];
            
            pthread_mutex_lock(&pool->lock);
            
            /* Clean up free list entries that might be from compacted chunks */
            /* For now, we just free the free list to simplify */
            free_block_t *free_block = pool->free_list;
            int free_count = 0;
            while (free_block) {
                free_block_t *next = free_block->next;
                free_count++;
                free_block = next;
            }
            
            /* Only compact if we have many free blocks */
            if (free_count > 10) {
                /* Clear free list */
                free_block = pool->free_list;
                while (free_block) {
                    free_block_t *next = free_block->next;
                    free(free_block);
                    free_block = next;
                }
                pool->free_list = NULL;
                compacted_count++;
            }
            
            /* Find and free low-utilization chunks */
            numa_pool_chunk_t **prev_ptr = &pool->chunks;
            numa_pool_chunk_t *chunk = pool->chunks;
            
            while (chunk) {
                float utilization = 0.0f;
                if (chunk->size > 0) {
                    utilization = (float)chunk->used_bytes / (float)chunk->size;
                }
                
                /* If chunk is below threshold and has significant free space */
                if (utilization < COMPACT_THRESHOLD && 
                    (1.0f - utilization) >= COMPACT_MIN_FREE_RATIO) {
                    
                    /* Remove chunk from list and free it */
                    *prev_ptr = chunk->next;
                    numa_free(chunk->memory, chunk->size);
                    free(chunk);
                    pool->chunks_count--;
                    compacted_count++;
                    
                    /* Move to next without advancing prev_ptr */
                    chunk = *prev_ptr;
                    continue;
                }
                
                /* Move to next chunk */
                prev_ptr = &chunk->next;
                chunk = chunk->next;
            }
            
            pthread_mutex_unlock(&pool->lock);
        }
    }
    
    return compacted_count;
}

/* ============================================================================
 * P2 Optimization: Slab Allocator Implementation
 * ============================================================================
 * Design:
 * - 4KB slabs for small objects (<=512B)
 * - Bitmap management for O(1) allocation
 * - Per-size-class slab pools
 * - Coexists with existing Pool for large objects (>512B)
 * - P2 fix: Slab header with back-pointer for O(1) free lookup
 * ========================================================================= */

/* Slab header stored at the beginning of each slab for O(1) free lookup */
#define SLAB_HEADER_MAGIC 0x534C4142  /* "SLAB" in ASCII */
typedef struct numa_slab_header {
    uint32_t magic;                  /* Magic number for validation */
    uint32_t class_idx;              /* Size class index */
    struct numa_slab *slab;          /* Back-pointer to slab structure */
    void *raw_memory;                /* Original unaligned memory for numa_free */
} numa_slab_header_t;

#define SLAB_HEADER_SIZE (sizeof(numa_slab_header_t))
#define SLAB_USABLE_SIZE (SLAB_SIZE - SLAB_HEADER_SIZE)

/* Slab structure - P2 fix: Use atomic counters for lock-free operations */
typedef struct numa_slab {
    void *memory;                    /* Actual memory address (NUMA-allocated) */
    struct numa_slab *next;          /* Next slab in list */
    struct numa_slab *prev;          /* Previous slab in list (P2 fix: O(1) removal) */
    uint32_t bitmap[SLAB_BITMAP_SIZE]; /* 128-bit bitmap for object allocation (atomic access) */
    _Atomic uint16_t free_count;     /* Number of free objects (atomic) */
    uint16_t objects_per_slab;       /* Total objects per slab */
    int node_id;                     /* NUMA node ID for this slab */
    int class_idx;                   /* Size class index */
    _Atomic int list_type;           /* 0=partial, 1=full, 2=empty (atomic) */
} numa_slab_t;

/* List type constants */
#define SLAB_LIST_PARTIAL 0
#define SLAB_LIST_FULL    1
#define SLAB_LIST_EMPTY   2

/* Slab class (one per size class) */
typedef struct {
    size_t obj_size;                 /* Object size (includes PREFIX) */
    numa_slab_t *partial_slabs;      /* Partially used slabs */
    numa_slab_t *full_slabs;         /* Fully used slabs */
    numa_slab_t *empty_slabs;        /* Empty slabs (cache) */
    size_t empty_count;              /* Number of cached empty slabs */
    pthread_mutex_t lock;            /* Thread safety */
    size_t slabs_count;              /* Total slabs allocated */
} numa_slab_class_t;

/* Per-node slab pool */
typedef struct {
    int node_id;
    numa_slab_class_t classes[NUMA_POOL_SIZE_CLASSES];
} numa_slab_node_t;

/* Global slab context */
static struct {
    int initialized;
    int num_nodes;
    numa_slab_node_t *slab_nodes;
} slab_ctx = {
    .initialized = 0,
    .num_nodes = 0,
    .slab_nodes = NULL
};

/* Bitmap operations - P2 fix: Lock-free using atomic operations */
static inline int bitmap_test(uint32_t *bitmap, int bit) {
    uint32_t val = __atomic_load_n(&bitmap[bit / 32], __ATOMIC_ACQUIRE);
    return (val & (1U << (bit % 32))) != 0;
}

static inline void bitmap_set(uint32_t *bitmap, int bit) {
    __atomic_fetch_or(&bitmap[bit / 32], (1U << (bit % 32)), __ATOMIC_ACQ_REL);
}

static inline void bitmap_clear(uint32_t *bitmap, int bit) {
    __atomic_fetch_and(&bitmap[bit / 32], ~(1U << (bit % 32)), __ATOMIC_ACQ_REL);
}

/* Try to atomically set a bit, returns 1 if successful (bit was 0), 0 if already set */
static inline int bitmap_try_set(uint32_t *bitmap, int bit) {
    uint32_t mask = 1U << (bit % 32);
    uint32_t old = __atomic_fetch_or(&bitmap[bit / 32], mask, __ATOMIC_ACQ_REL);
    return (old & mask) == 0;  /* Return 1 if we set it, 0 if already set */
}

/* Find first free bit in bitmap using CPU intrinsic (O(1) per 32-bit word) 
 * P2 fix: Lock-free version using atomic load */
static int bitmap_find_first_free(uint32_t *bitmap, int max_bits) {
    int num_words = (max_bits + 31) / 32;
    for (int i = 0; i < num_words; i++) {
        /* Atomic load to get current bitmap state */
        uint32_t word = __atomic_load_n(&bitmap[i], __ATOMIC_ACQUIRE);
        uint32_t inverted = ~word;
        if (inverted != 0) {
            int bit_pos = __builtin_ffs(inverted) - 1;
            int global_pos = i * 32 + bit_pos;
            if (global_pos < max_bits) {
                return global_pos;
            }
        }
    }
    return -1;
}

/* Lock-free find and set: Find a free bit and atomically set it
 * Returns bit index on success, -1 if no free bits */
static int bitmap_find_and_set(uint32_t *bitmap, int max_bits) {
    int num_words = (max_bits + 31) / 32;
    for (int i = 0; i < num_words; i++) {
        uint32_t word = __atomic_load_n(&bitmap[i], __ATOMIC_ACQUIRE);
        while (~word != 0) {  /* While there are free bits */
            uint32_t inverted = ~word;
            int bit_pos = __builtin_ffs(inverted) - 1;
            int global_pos = i * 32 + bit_pos;
            if (global_pos >= max_bits) break;
            
            /* Try to atomically set this bit */
            uint32_t mask = 1U << bit_pos;
            uint32_t expected = word;
            uint32_t desired = word | mask;
            if (__atomic_compare_exchange_n(&bitmap[i], &expected, desired,
                                           0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return global_pos;  /* Successfully claimed this bit */
            }
            /* CAS failed, reload and retry */
            word = expected;
        }
    }
    return -1;
}

/* P2 fix: Helper to remove slab from doubly-linked list - O(1) */
static inline void slab_list_remove(numa_slab_t **list_head, numa_slab_t *slab) {
    if (slab->prev) {
        slab->prev->next = slab->next;
    } else {
        /* slab is the head */
        *list_head = slab->next;
    }
    if (slab->next) {
        slab->next->prev = slab->prev;
    }
    slab->prev = NULL;
    slab->next = NULL;
}

/* P2 fix: Helper to add slab to head of doubly-linked list - O(1) */
static inline void slab_list_add_head(numa_slab_t **list_head, numa_slab_t *slab) {
    slab->prev = NULL;
    slab->next = *list_head;
    if (*list_head) {
        (*list_head)->prev = slab;
    }
    *list_head = slab;
}

/* Allocate a new slab with aligned memory */
static numa_slab_t *alloc_new_slab(int node, size_t obj_size, int class_idx) {
    /* Allocate slab structure */
    numa_slab_t *slab = (numa_slab_t *)malloc(sizeof(numa_slab_t));
    if (!slab) return NULL;
    
    /* P2 fix: Allocate aligned slab memory for O(1) free lookup
     * We allocate 2x size and manually align to SLAB_SIZE boundary */
    void *raw_mem = numa_alloc_onnode(SLAB_SIZE * 2, node);
    if (!raw_mem) {
        free(slab);
        return NULL;
    }
    
    /* Align to SLAB_SIZE boundary */
    uintptr_t raw_addr = (uintptr_t)raw_mem;
    uintptr_t aligned_addr = (raw_addr + SLAB_SIZE - 1) & ~((uintptr_t)(SLAB_SIZE - 1));
    slab->memory = (void *)aligned_addr;
    /* Store raw pointer for freeing later - we'll use a trick: store offset in header */
    
    /* P2 fix: Initialize slab header with back-pointer for O(1) free lookup */
    numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
    header->magic = SLAB_HEADER_MAGIC;
    header->class_idx = class_idx;
    header->slab = slab;
    header->raw_memory = raw_mem;  /* Store original for numa_free */
    
    /* Initialize slab */
    memset(slab->bitmap, 0, sizeof(slab->bitmap));
    /* Calculate objects per slab using usable size (after header) */
    slab->objects_per_slab = SLAB_USABLE_SIZE / obj_size;
    __atomic_store_n(&slab->free_count, slab->objects_per_slab, __ATOMIC_RELEASE);
    slab->next = NULL;
    slab->prev = NULL;  /* P2 fix: Initialize prev pointer */
    slab->node_id = node;
    slab->class_idx = class_idx;
    __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
    
    return slab;
}

/* Free a slab */
static void free_slab(numa_slab_t *slab) {
    if (slab->memory) {
        /* Get raw pointer from header and free 2x size */
        numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
        numa_free(header->raw_memory, SLAB_SIZE * 2);
    }
    free(slab);
}

/* Initialize slab allocator */
int numa_slab_init(void) {
    if (slab_ctx.initialized) {
        return 0;
    }
    
    /* Check NUMA availability */
    if (numa_available() < 0) {
        slab_ctx.num_nodes = 1;
    } else {
        slab_ctx.num_nodes = numa_max_node() + 1;
    }
    
    /* Allocate node structures */
    slab_ctx.slab_nodes = (numa_slab_node_t *)calloc(
        slab_ctx.num_nodes, sizeof(numa_slab_node_t));
    if (!slab_ctx.slab_nodes) {
        return -1;
    }
    
    /* Initialize each node's slab classes */
    for (int i = 0; i < slab_ctx.num_nodes; i++) {
        slab_ctx.slab_nodes[i].node_id = i;
        
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            numa_slab_class_t *class = &slab_ctx.slab_nodes[i].classes[j];
            
            /* Only initialize for small objects (<=512B) */
            size_t obj_size = numa_pool_size_classes[j];
            if (obj_size > SLAB_MAX_OBJECT_SIZE) {
                continue;
            }
            
            class->obj_size = obj_size + 16;  /* Include PREFIX */
            class->partial_slabs = NULL;
            class->full_slabs = NULL;
            class->empty_slabs = NULL;
            class->empty_count = 0;
            class->slabs_count = 0;
            pthread_mutex_init(&class->lock, NULL);
        }
    }
    
    slab_ctx.initialized = 1;
    return 0;
}

/* Cleanup slab allocator */
void numa_slab_cleanup(void) {
    if (!slab_ctx.initialized) {
        return;
    }
    
    for (int i = 0; i < slab_ctx.num_nodes; i++) {
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            numa_slab_class_t *class = &slab_ctx.slab_nodes[i].classes[j];
            
            if (class->obj_size == 0) continue;
            
            /* Free all slabs in all lists */
            numa_slab_t *slab;
            
            slab = class->partial_slabs;
            while (slab) {
                numa_slab_t *next = slab->next;
                free_slab(slab);
                slab = next;
            }
            
            slab = class->full_slabs;
            while (slab) {
                numa_slab_t *next = slab->next;
                free_slab(slab);
                slab = next;
            }
            
            slab = class->empty_slabs;
            while (slab) {
                numa_slab_t *next = slab->next;
                free_slab(slab);
                slab = next;
            }
            
            pthread_mutex_destroy(&class->lock);
        }
    }
    
    free(slab_ctx.slab_nodes);
    slab_ctx.slab_nodes = NULL;
    slab_ctx.initialized = 0;
}

/* Allocate from slab - P2 fix: Lock-free fast path */
void *numa_slab_alloc(size_t size, int node, size_t *total_size) {
    if (!slab_ctx.initialized) {
        return NULL;
    }
    
    /* Find appropriate size class */
    int class_idx = -1;
    for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {
        if (size <= numa_pool_size_classes[i]) {
            class_idx = i;
            break;
        }
    }
    
    if (class_idx < 0 || numa_pool_size_classes[class_idx] > SLAB_MAX_OBJECT_SIZE) {
        return NULL;  /* Too large for slab */
    }
    
    /* Validate node */
    if (node < 0 || node >= slab_ctx.num_nodes) {
        node = 0;
    }
    
    numa_slab_class_t *class = &slab_ctx.slab_nodes[node].classes[class_idx];
    size_t aligned_size = (size + 15) & ~15;  /* 16-byte align */
    *total_size = aligned_size + 16;  /* Include PREFIX */
    
    /* Fast path: Try to allocate from existing partial slab without lock */
    numa_slab_t *slab = __atomic_load_n(&class->partial_slabs, __ATOMIC_ACQUIRE);
    while (slab) {
        /* Try lock-free allocation from this slab */
        int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
        if (free_bit >= 0) {
            /* Successfully claimed a slot */
            uint16_t new_count = __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
            
            /* Calculate object address (skip header) */
            void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);
            
            /* If slab is now full, move to full list (need lock for list ops) */
            if (new_count == 0) {
                pthread_mutex_lock(&class->lock);
                /* Double-check after acquiring lock */
                if (__atomic_load_n(&slab->free_count, __ATOMIC_ACQUIRE) == 0 &&
                    __atomic_load_n(&slab->list_type, __ATOMIC_ACQUIRE) == SLAB_LIST_PARTIAL) {
                    slab_list_remove(&class->partial_slabs, slab);
                    slab_list_add_head(&class->full_slabs, slab);
                    __atomic_store_n(&slab->list_type, SLAB_LIST_FULL, __ATOMIC_RELEASE);
                }
                pthread_mutex_unlock(&class->lock);
            }
            return result;
        }
        /* This slab is full, try next */
        slab = __atomic_load_n(&slab->next, __ATOMIC_ACQUIRE);
    }
    
    /* Slow path: Need to get a new slab (requires lock) */
    pthread_mutex_lock(&class->lock);
    
    /* Re-check partial_slabs after acquiring lock */
    slab = class->partial_slabs;
    if (slab) {
        int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
        if (free_bit >= 0) {
            __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
            void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);
            pthread_mutex_unlock(&class->lock);
            return result;
        }
    }
    
    /* Get from empty cache or allocate new */
    if (class->empty_slabs) {
        slab = class->empty_slabs;
        slab_list_remove(&class->empty_slabs, slab);
        class->empty_count--;
    } else {
        slab = alloc_new_slab(node, class->obj_size, class_idx);
        if (!slab) {
            pthread_mutex_unlock(&class->lock);
            return NULL;
        }
        class->slabs_count++;
    }
    
    /* Add to partial list */
    slab_list_add_head(&class->partial_slabs, slab);
    __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
    
    /* Allocate from the new slab */
    int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
    __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
    void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);
    
    pthread_mutex_unlock(&class->lock);
    return result;
}

/* Free to slab - P2 fix: Lock-free fast path using atomic operations */
void numa_slab_free(void *ptr, size_t total_size, int node) {
    if (!slab_ctx.initialized || !ptr) {
        return;
    }
    
    /* P2 fix: Use page alignment and slab header for O(1) slab lookup */
    uintptr_t ptr_addr = (uintptr_t)ptr;
    uintptr_t slab_base = ptr_addr & ~((uintptr_t)(SLAB_SIZE - 1));
    numa_slab_header_t *header = (numa_slab_header_t *)slab_base;
    
    /* Validate magic number */
    if (header->magic != SLAB_HEADER_MAGIC) {
        return;
    }
    
    numa_slab_t *slab = header->slab;
    if (!slab || slab->memory != (void *)slab_base) {
        return;
    }
    
    int class_idx = header->class_idx;
    int slab_node = slab->node_id;
    
    if (slab_node < 0 || slab_node >= slab_ctx.num_nodes) {
        return;
    }
    
    numa_slab_class_t *class = &slab_ctx.slab_nodes[slab_node].classes[class_idx];
    
    /* Calculate object index (account for header) */
    size_t offset = (char *)ptr - (char *)slab->memory - SLAB_HEADER_SIZE;
    int obj_index = offset / class->obj_size;
    
    if (obj_index < 0 || obj_index >= (int)slab->objects_per_slab) {
        return;
    }
    
    /* Lock-free: Clear bit and increment free_count atomically */
    bitmap_clear(slab->bitmap, obj_index);
    uint16_t old_count = __atomic_fetch_add(&slab->free_count, 1, __ATOMIC_ACQ_REL);
    uint16_t new_count = old_count + 1;
    
    /* Check if we need to move slab between lists (requires lock) */
    int was_full = (old_count == 0);
    int is_empty = (new_count == slab->objects_per_slab);
    
    if (was_full || is_empty) {
        pthread_mutex_lock(&class->lock);
        
        int current_list = __atomic_load_n(&slab->list_type, __ATOMIC_ACQUIRE);
        uint16_t current_count = __atomic_load_n(&slab->free_count, __ATOMIC_ACQUIRE);
        
        if (was_full && current_list == SLAB_LIST_FULL) {
            /* Move from full to partial */
            slab_list_remove(&class->full_slabs, slab);
            slab_list_add_head(&class->partial_slabs, slab);
            __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
        } else if (current_count == slab->objects_per_slab && current_list == SLAB_LIST_PARTIAL) {
            /* Move from partial to empty/free */
            slab_list_remove(&class->partial_slabs, slab);
            
            if (class->empty_count < SLAB_EMPTY_CACHE_MAX) {
                slab_list_add_head(&class->empty_slabs, slab);
                __atomic_store_n(&slab->list_type, SLAB_LIST_EMPTY, __ATOMIC_RELEASE);
                class->empty_count++;
            } else {
                free_slab(slab);
                class->slabs_count--;
            }
        }
        
        pthread_mutex_unlock(&class->lock);
    }
}
