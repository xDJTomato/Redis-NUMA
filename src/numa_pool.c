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

/* Memory pool chunk structure */
typedef struct numa_pool_chunk {
    void *memory;                  /* NUMA-allocated memory */
    size_t size;                   /* Chunk size */
    size_t offset;                 /* Current allocation offset */
    struct numa_pool_chunk *next;  /* Next chunk in list */
} numa_pool_chunk_t;

/* Size class pool */
typedef struct {
    size_t obj_size;               /* Object size for this class */
    numa_pool_chunk_t *chunks;     /* Chunk list */
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
    chunk->next = NULL;
    
    return chunk;
}

/* Allocate memory from pool */
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
        /* Find appropriate size class */
        int class_idx = -1;
        for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {
            if (alloc_size <= numa_pool_size_classes[i]) {
                class_idx = i;
                break;
            }
        }
        
        if (class_idx >= 0) {
            numa_node_pool_t *node_pool = &pool_ctx.node_pools[node];
            numa_size_class_pool_t *pool = &node_pool->pools[class_idx];
            
            pthread_mutex_lock(&pool->lock);
            
            /* Try to allocate from existing chunks */
            numa_pool_chunk_t *chunk = pool->chunks;
            size_t aligned_size = (alloc_size + 15) & ~15;  /* 16-byte align */
            
            while (chunk) {
                if (chunk->offset + aligned_size <= chunk->size) {
                    result = (char *)chunk->memory + chunk->offset;
                    chunk->offset += aligned_size;
                    from_pool = 1;
                    break;
                }
                chunk = chunk->next;
            }
            
            /* Allocate new chunk if needed */
            if (!result) {
                numa_pool_chunk_t *new_chunk = alloc_new_chunk(node, alloc_size);
                if (new_chunk) {
                    new_chunk->offset = aligned_size;
                    new_chunk->next = pool->chunks;
                    pool->chunks = new_chunk;
                    pool->chunks_count++;
                    result = new_chunk->memory;
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

/* Free memory (only direct allocations are freed) */
void numa_pool_free(void *ptr, size_t total_size, int from_pool)
{
    if (!ptr) {
        return;
    }
    
    /* Only free direct NUMA allocations */
    if (!from_pool) {
        numa_free(ptr, total_size);
    }
    /* Pool memory is not individually freed */
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
