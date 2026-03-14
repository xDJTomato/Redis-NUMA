/* numa_pool.c - NUMA感知内存池分配器实现
 *
 * 实现说明：
 * - 内部元数据（chunk头部、池结构）使用libc malloc/free
 * - 实际内存chunk使用numa_alloc_onnode/numa_free
 * - 分配路径中不输出printf/调试信息，避免递归
 * - 通过每池互斥锁保证线程安全
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

/* 内存池大小分类 - 扩展到16级 */
const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES] = {
    16, 32, 48, 64,          /* 细粒度小对象 */
    96, 128, 192, 256,       /* 中小对象 */
    384, 512, 768, 1024,     /* 中型对象 */
    1536, 2048, 3072, 4096   /* 大型对象 */
};

/* 根据对象大小获取最优chunk大小 */
size_t get_chunk_size_for_object(size_t obj_size) {
    if (obj_size <= 256) {
        return CHUNK_SIZE_SMALL;    /* 小对象使甖16KB */
    } else if (obj_size <= 1024) {
        return CHUNK_SIZE_MEDIUM;   /* 中型对象使甖64KB */
    } else if (obj_size <= 4096) {
        return CHUNK_SIZE_LARGE;    /* 大型对象使用256KB */
    } else {
        return 0;  /* 超大对象直接分配 */
    }
}

/* 前向声明 */
typedef struct numa_pool_chunk numa_pool_chunk_t;
typedef struct free_block free_block_t;

/* P1优化：空闲块结构，用于空闲列表管理 */
struct free_block {
    void *ptr;                     /* 已释放内存指针 */
    size_t size;                   /* 已释放块的大小 */
    struct free_block *next;       /* 列表中的下一个空闲块 */
};

/* 内存池chunk结构 */
struct numa_pool_chunk {
    void *memory;                  /* NUMA分配的内存 */
    size_t size;                   /* chunk大小 */
    size_t offset;                 /* 当前分配偏移量 */
    size_t used_bytes;             /* 实际已分配字节数（P1：利用率跟踪） */
    struct numa_pool_chunk *next;  /* 链表中的下一个chunk */
};

/* 大小分类池 */
typedef struct {
    size_t obj_size;               /* 该分类的对象大小 */
    numa_pool_chunk_t *chunks;     /* chunk链表 */
    free_block_t *free_list;       /* P1：该大小分类的空闲列表 */
    pthread_mutex_t lock;          /* 线程安全 */
    size_t chunks_count;           /* 统计信息 */
} numa_size_class_pool_t;

/* 每节点内存池 */
typedef struct numa_node_pool {
    int node_id;
    numa_size_class_pool_t pools[NUMA_POOL_SIZE_CLASSES];
    numa_pool_stats_t stats;
} numa_node_pool_t;

/* 全局池上下文 */
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

/* 线程局部当前节点 */
static __thread int tls_current_node = -1;

/* 初始化内存池系统 */
int numa_pool_init(void)
{
    pthread_mutex_lock(&pool_ctx.init_lock);
    
    if (pool_ctx.initialized) {
        pthread_mutex_unlock(&pool_ctx.init_lock);
        return 0;
    }
    
    /* 检查NUMA可用性 */
    if (numa_available() == -1) {
        pool_ctx.numa_available = 0;
        pool_ctx.initialized = 1;
        pthread_mutex_unlock(&pool_ctx.init_lock);
        return 0;
    }
    
    pool_ctx.numa_available = 1;
    pool_ctx.num_nodes = numa_max_node() + 1;
    
    /* 获取当前节点 */
    int cpu = sched_getcpu();
    if (cpu >= 0) {
        pool_ctx.current_node = numa_node_of_cpu(cpu);
    } else {
        pool_ctx.current_node = 0;
    }
    tls_current_node = pool_ctx.current_node;
    
    /* 分配节点池数组 */
    pool_ctx.node_pools = calloc(pool_ctx.num_nodes, sizeof(numa_node_pool_t));
    if (!pool_ctx.node_pools) {
        pool_ctx.numa_available = 0;
        pool_ctx.initialized = 1;
        pthread_mutex_unlock(&pool_ctx.init_lock);
        return -1;
    }
    
    /* 初始化每个节点的内存池 */
    for (int i = 0; i < pool_ctx.num_nodes; i++) {
        pool_ctx.node_pools[i].node_id = i;
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            pool_ctx.node_pools[i].pools[j].obj_size = numa_pool_size_classes[j];
            pool_ctx.node_pools[i].pools[j].chunks = NULL;
            pool_ctx.node_pools[i].pools[j].free_list = NULL;  /* P1：初始化空闲列表 */
            pool_ctx.node_pools[i].pools[j].chunks_count = 0;
            pthread_mutex_init(&pool_ctx.node_pools[i].pools[j].lock, NULL);
        }
        memset(&pool_ctx.node_pools[i].stats, 0, sizeof(numa_pool_stats_t));
    }
    
    pool_ctx.initialized = 1;
    pthread_mutex_unlock(&pool_ctx.init_lock);
    return 0;
}

/* 清理所有内存池 */
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

/* 内部：动态分配内存池新chunk */
static numa_pool_chunk_t *alloc_new_chunk(int node, size_t obj_size)
{
    numa_pool_chunk_t *chunk = malloc(sizeof(numa_pool_chunk_t));
    if (!chunk) {
        return NULL;
    }
    
    /* 根据对象大小获取最优chunk大小 */
    size_t chunk_size = get_chunk_size_for_object(obj_size);
    if (chunk_size == 0) {
        /* 超大对象，应使用直接分配 */
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
    chunk->used_bytes = 0;        /* P1：初始化利用率跟踪 */
    chunk->next = NULL;
    
    return chunk;
}

/* 从内存池分配 - 优化快速路径 */
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
    
    /* 小对象尝试内存池分配 */
    if (alloc_size <= NUMA_POOL_MAX_ALLOC && pool_ctx.node_pools) {
        /* 二分查找式快速大小分类查找 */
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
            
            /* 快速路径1：尝试空闲列表头部（O(1)复用） */
            free_block_t *free_block = pool->free_list;
            if (free_block && free_block->size >= aligned_size) {
                result = free_block->ptr;
                pool->free_list = free_block->next;
                free(free_block);
                from_pool = 1;
            }
            
            /* 快速路径2：直接尝试第一个chunk（热缓存） */
            if (!result) {
                numa_pool_chunk_t *chunk = pool->chunks;
                if (chunk && chunk->offset + aligned_size <= chunk->size) {
                    result = (char *)chunk->memory + chunk->offset;
                    chunk->offset += aligned_size;
                    chunk->used_bytes += aligned_size;
                    from_pool = 1;
                }
            }
            
            /* 慢速路径：按需分配新chunk */
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
    
    /* 回退到直接NUMA分配 */
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

/* 释放内存 - P1：将已释放块添加到空闲列表 */
void numa_pool_free(void *ptr, size_t total_size, int from_pool)
{
    if (!ptr) {
        return;
    }
    
    if (!from_pool) {
        /* 只释放直接NUMA分配 */
        numa_free(ptr, total_size);
        return;
    }
    
    /* P1：对于池内分配，添加到空闲列表 */
    if (!pool_ctx.initialized || !pool_ctx.node_pools) {
        return;  /* 内存池未初始化，默认泄漏 */
    }
    
    /* 找到此大小对应的大小分类 */
    size_t aligned_size = (total_size + 15) & ~15;
    int class_idx = -1;
    
    for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {
        if (aligned_size <= numa_pool_size_classes[i]) {
            class_idx = i;
            break;
        }
    }
    
    if (class_idx < 0) {
        return;  /* 大小不匹配任何分类，跳过 */
    }
    
    /* 不知道属于哪个节点，尝试查找或直接使用节点0（现有局限，当前可接受） */
    int node = 0;
    if (pool_ctx.current_node >= 0 && pool_ctx.current_node < pool_ctx.num_nodes) {
        node = pool_ctx.current_node;
    }
    
    /* 创建空闲块 */
    free_block_t *free_block = malloc(sizeof(free_block_t));
    if (!free_block) {
        return;  /* 无法记录空闲块，默认泄漏 */
    }
    
    free_block->ptr = ptr;
    free_block->size = aligned_size;
    
    /* 添加到池的空闲列表 */
    numa_size_class_pool_t *pool = &pool_ctx.node_pools[node].pools[class_idx];
    
    pthread_mutex_lock(&pool->lock);
    free_block->next = pool->free_list;
    pool->free_list = free_block;
    pthread_mutex_unlock(&pool->lock);
}

/* 设置当前 NUMA 节点 */
void numa_pool_set_node(int node)
{
    if (node >= 0 && node < pool_ctx.num_nodes) {
        pool_ctx.current_node = node;
        tls_current_node = node;
    }
}

/* 获取当前 NUMA 节点 */
int numa_pool_get_node(void)
{
    if (tls_current_node >= 0) {
        return tls_current_node;
    }
    return pool_ctx.current_node;
}

/* 获取 NUMA 节点数量 */
int numa_pool_num_nodes(void)
{
    return pool_ctx.num_nodes;
}

/* 检查 NUMA 是否可用 */
int numa_pool_available(void)
{
    return pool_ctx.numa_available;
}

/* 获取池统计信息 */
void numa_pool_get_stats(int node, numa_pool_stats_t *stats)
{
    if (!stats || node < 0 || node >= pool_ctx.num_nodes || !pool_ctx.node_pools) {
        return;
    }
    
    *stats = pool_ctx.node_pools[node].stats;
}

/* 重置池统计信息 */
void numa_pool_reset_stats(void)
{
    if (!pool_ctx.node_pools) {
        return;
    }
    
    for (int i = 0; i < pool_ctx.num_nodes; i++) {
        memset(&pool_ctx.node_pools[i].stats, 0, sizeof(numa_pool_stats_t));
    }
}

/* P1优化：获取chunk利用率 */
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

/* P1优化：尝试压缩低利用率chunk */
int numa_pool_try_compact(void)
{
    if (!pool_ctx.initialized || !pool_ctx.node_pools) {
        return 0;
    }
    
    int compacted_count = 0;
    
    /* 遍历所有节点和大小分类 */
    for (int node = 0; node < pool_ctx.num_nodes; node++) {
        for (int class_idx = 0; class_idx < NUMA_POOL_SIZE_CLASSES; class_idx++) {
            numa_size_class_pool_t *pool = &pool_ctx.node_pools[node].pools[class_idx];
            
            pthread_mutex_lock(&pool->lock);
            
            /* 清理可能来自已压缩chunk的空闲列表条目 */
            /* 简化处理：直接释放空闲列表 */
            free_block_t *free_block = pool->free_list;
            int free_count = 0;
            while (free_block) {
                free_block_t *next = free_block->next;
                free_count++;
                free_block = next;
            }
            
            /* 仅当空闲块数量较多时执行压缩 */
            if (free_count > 10) {
                /* 清空空闲列表 */
                free_block = pool->free_list;
                while (free_block) {
                    free_block_t *next = free_block->next;
                    free(free_block);
                    free_block = next;
                }
                pool->free_list = NULL;
                compacted_count++;
            }
            
            /* 查找并释放低利用率chunk */
            numa_pool_chunk_t **prev_ptr = &pool->chunks;
            numa_pool_chunk_t *chunk = pool->chunks;
            
            while (chunk) {
                float utilization = 0.0f;
                if (chunk->size > 0) {
                    utilization = (float)chunk->used_bytes / (float)chunk->size;
                }
                
                /* 如果chunk低于阈值且有较大空闲空间 */
                if (utilization < COMPACT_THRESHOLD && 
                    (1.0f - utilization) >= COMPACT_MIN_FREE_RATIO) {
                    
                    /* 从链表移除chunk并释放 */
                    *prev_ptr = chunk->next;
                    numa_free(chunk->memory, chunk->size);
                    free(chunk);
                    pool->chunks_count--;
                    compacted_count++;
                    
                    /* 不推进prev_ptr，直接移到下一个 */
                    chunk = *prev_ptr;
                    continue;
                }
                
                /* 移到下一个chunk */
                prev_ptr = &chunk->next;
                chunk = chunk->next;
            }
            
            pthread_mutex_unlock(&pool->lock);
        }
    }
    
    return compacted_count;
}

/* ============================================================================
 * P2优化：Slab分配器实现
 * ============================================================================
 * 设计：
 * - 4KB slab用于小对象（<=512B）
 * - 位图O(1)分配
 * - 每大小分类slab池
 * - 与已有Pool共存，用于大对象（>512B）
 * - P2修复：带回指针的Slab头部，支持O(1)free查找
 * ========================================================================= */

/* 每个slab头部存储在slab开头，用于O(1)信free查找 */
#define SLAB_HEADER_MAGIC 0x534C4142  /* ASCII中的"SLAB" */
typedef struct numa_slab_header {
    uint32_t magic;                  /* 魔数，用于验证 */
    uint32_t class_idx;              /* 大小分类索引 */
    struct numa_slab *slab;          /* 回指针，指向slab结构 */
    void *raw_memory;                /* 原始未对齐内存，用于numa_free */
} numa_slab_header_t;

#define SLAB_HEADER_SIZE (sizeof(numa_slab_header_t))
#define SLAB_USABLE_SIZE (SLAB_SIZE - SLAB_HEADER_SIZE)

/* Slab结构 - P2修复：使用原子计数器实现无锁操作 */
typedef struct numa_slab {
    void *memory;                    /* 实际内存地址（NUMA分配） */
    struct numa_slab *next;          /* 链表中的下一个slab */
    struct numa_slab *prev;          /* 上一个slab（P2修复：O(1)移除） */
    uint32_t bitmap[SLAB_BITMAP_SIZE]; /* 对象分配用128位位图（原子访问） */
    _Atomic uint16_t free_count;     /* 空闲对象数（原子） */
    uint16_t objects_per_slab;       /* 每个slab的对象总数 */
    int node_id;                     /* 该slab的NUMA节点ID */
    int class_idx;                   /* 大小分类索引 */
    _Atomic int list_type;           /* 0=部分占用, 1=全占用, 2=空闲（原子） */
} numa_slab_t;

/* 链表类型常量 */
#define SLAB_LIST_PARTIAL 0
#define SLAB_LIST_FULL    1
#define SLAB_LIST_EMPTY   2

/* 大小分类（每个大小分类一个） */
typedef struct {
    size_t obj_size;                 /* 对象大小（包含PREFIX） */
    numa_slab_t *partial_slabs;      /* 部分使用的slabs */
    numa_slab_t *full_slabs;         /* 已全占用的slabs */
    numa_slab_t *empty_slabs;        /* 空闲的slabs（缓存） */
    size_t empty_count;              /* 缓存的空闲slab数 */
    pthread_mutex_t lock;            /* 线程安全 */
    size_t slabs_count;              /* 已分配slab总数 */
} numa_slab_class_t;

/* 每节点slab池 */
typedef struct {
    int node_id;
    numa_slab_class_t classes[NUMA_POOL_SIZE_CLASSES];
} numa_slab_node_t;

/* 全局slab上下文 */
static struct {
    int initialized;
    int num_nodes;
    numa_slab_node_t *slab_nodes;
} slab_ctx = {
    .initialized = 0,
    .num_nodes = 0,
    .slab_nodes = NULL
};

/* 位图操作 - P2修复：使用原子操作实现无锁 */
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

/* 尝试原子设置一个bit，返回1表示成功（bit原来为0），0表示已设置 */
static inline int bitmap_try_set(uint32_t *bitmap, int bit) {
    uint32_t mask = 1U << (bit % 32);
    uint32_t old = __atomic_fetch_or(&bitmap[bit / 32], mask, __ATOMIC_ACQ_REL);
    return (old & mask) == 0;  /* 返回1表示成功设置，0表示已设置 */
}

/* 使用CPU内属查找位图第一个空闲bit（每32bit字O(1)）
 * P2修复：使用原子读取的无锁版本 */
static int bitmap_find_first_free(uint32_t *bitmap, int max_bits) {
    int num_words = (max_bits + 31) / 32;
    for (int i = 0; i < num_words; i++) {
        /* 原子读取以获得当前位图状态 */
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

/* 无锁查找并设置：查找空闲bit并原子设置它
 * 成功返回bit索引，无空闲bit返回-1 */
static int bitmap_find_and_set(uint32_t *bitmap, int max_bits) {
    int num_words = (max_bits + 31) / 32;
    for (int i = 0; i < num_words; i++) {
        uint32_t word = __atomic_load_n(&bitmap[i], __ATOMIC_ACQUIRE);
        while (~word != 0) {  /* 当有空闲bit时 */
            uint32_t inverted = ~word;
            int bit_pos = __builtin_ffs(inverted) - 1;
            int global_pos = i * 32 + bit_pos;
            if (global_pos >= max_bits) break;
            
            /* 尝试原子设置该bit */
            uint32_t mask = 1U << bit_pos;
            uint32_t expected = word;
            uint32_t desired = word | mask;
            if (__atomic_compare_exchange_n(&bitmap[i], &expected, desired,
                                           0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return global_pos;  /* 成功占用该bit */
            }
            /* CAS失败，重新加载并重试 */
            word = expected;
        }
    }
    return -1;
}

/* P2修复：辅助函数 - 从双向链表移除slab - O(1) */
static inline void slab_list_remove(numa_slab_t **list_head, numa_slab_t *slab) {
    if (slab->prev) {
        slab->prev->next = slab->next;
    } else {
        /* slab是头节点 */
        *list_head = slab->next;
    }
    if (slab->next) {
        slab->next->prev = slab->prev;
    }
    slab->prev = NULL;
    slab->next = NULL;
}

/* P2修复：辅助函数 - 将slab添加到双向链表头部 - O(1) */
static inline void slab_list_add_head(numa_slab_t **list_head, numa_slab_t *slab) {
    slab->prev = NULL;
    slab->next = *list_head;
    if (*list_head) {
        (*list_head)->prev = slab;
    }
    *list_head = slab;
}

/* 分配内存对齐的新slab */
static numa_slab_t *alloc_new_slab(int node, size_t obj_size, int class_idx) {
    /* 分配slab结构 */
    numa_slab_t *slab = (numa_slab_t *)malloc(sizeof(numa_slab_t));
    if (!slab) return NULL;
    
    /* P2修复：分配对齐的slab内存，支持O(1)free查找
     * 分配2倍大小并手动对齐到SLAB_SIZE边界 */
    void *raw_mem = numa_alloc_onnode(SLAB_SIZE * 2, node);
    if (!raw_mem) {
        free(slab);
        return NULL;
    }
    
    /* 对齐到SLAB_SIZE边界 */
    uintptr_t raw_addr = (uintptr_t)raw_mem;
    uintptr_t aligned_addr = (raw_addr + SLAB_SIZE - 1) & ~((uintptr_t)(SLAB_SIZE - 1));
    slab->memory = (void *)aligned_addr;
    /* 存储稿后释放用的原始指针，将偏移量存入头部 */
    
    /* P2修复：初始化带回指针的slab头部，支持O(1)free查找 */
    numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
    header->magic = SLAB_HEADER_MAGIC;
    header->class_idx = class_idx;
    header->slab = slab;
    header->raw_memory = raw_mem;  /* 存储原始指针用于numa_free */
    
    /* 初始化slab */
    memset(slab->bitmap, 0, sizeof(slab->bitmap));
    /* 使用可用大小（头部后）计算每个slab的对象数 */
    slab->objects_per_slab = SLAB_USABLE_SIZE / obj_size;
    __atomic_store_n(&slab->free_count, slab->objects_per_slab, __ATOMIC_RELEASE);
    slab->next = NULL;
    slab->prev = NULL;  /* P2修复：初始化prev指针 */
    slab->node_id = node;
    slab->class_idx = class_idx;
    __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
    
    return slab;
}

/* 释放一个slab */
static void free_slab(numa_slab_t *slab) {
    if (slab->memory) {
        /* 从头部获取原始指针并释放2倍大小 */
        numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
        numa_free(header->raw_memory, SLAB_SIZE * 2);
    }
    free(slab);
}

/* 初始化slab分配器 */
int numa_slab_init(void) {
    if (slab_ctx.initialized) {
        return 0;
    }
    
    /* 检查NUMA可用性 */
    if (numa_available() < 0) {
        slab_ctx.num_nodes = 1;
    } else {
        slab_ctx.num_nodes = numa_max_node() + 1;
    }
    
    /* 分配节点结构 */
    slab_ctx.slab_nodes = (numa_slab_node_t *)calloc(
        slab_ctx.num_nodes, sizeof(numa_slab_node_t));
    if (!slab_ctx.slab_nodes) {
        return -1;
    }
    
    /* 初始化每个节点的slab分类 */
    for (int i = 0; i < slab_ctx.num_nodes; i++) {
        slab_ctx.slab_nodes[i].node_id = i;
        
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            numa_slab_class_t *class = &slab_ctx.slab_nodes[i].classes[j];
            
            /* 只初始化小对象（<=512B） */
            size_t obj_size = numa_pool_size_classes[j];
            if (obj_size > SLAB_MAX_OBJECT_SIZE) {
                continue;
            }
            
            class->obj_size = obj_size + 16;  /* 包含PREFIX */
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

/* 清理slab分配器 */
void numa_slab_cleanup(void) {
    if (!slab_ctx.initialized) {
        return;
    }
    
    for (int i = 0; i < slab_ctx.num_nodes; i++) {
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            numa_slab_class_t *class = &slab_ctx.slab_nodes[i].classes[j];
            
            if (class->obj_size == 0) continue;
            
            /* 释放所有列表中的所有slab */
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

/* 从 slab 分配 - P2修复：无锁快速路径 */
void *numa_slab_alloc(size_t size, int node, size_t *total_size) {
    if (!slab_ctx.initialized) {
        return NULL;
    }
    
    /* 查找合适的大小分类 */
    int class_idx = -1;
    for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {
        if (size <= numa_pool_size_classes[i]) {
            class_idx = i;
            break;
        }
    }
    
    if (class_idx < 0 || numa_pool_size_classes[class_idx] > SLAB_MAX_OBJECT_SIZE) {
        return NULL;  /* 超出slab大小限制 */
    }
    
    /* 验证节点 */
    if (node < 0 || node >= slab_ctx.num_nodes) {
        node = 0;
    }
    
    numa_slab_class_t *class = &slab_ctx.slab_nodes[node].classes[class_idx];
    size_t aligned_size = (size + 15) & ~15;  /* 16-byte align */
    *total_size = aligned_size + 16;  /* Include PREFIX */
    
    /* 快速路径：无锁尝试从现有部分slab分配 */
    numa_slab_t *slab = __atomic_load_n(&class->partial_slabs, __ATOMIC_ACQUIRE);
    while (slab) {
        /* Try lock-free allocation from this slab */
        int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
        if (free_bit >= 0) {
            /* 成功占用一个插槽 */
            uint16_t new_count = __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
            
            /* 计算对象地址（跳过头部） */
            void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);
            
            /* 如果slab已满，移入full列表（需加锁进行链表操作） */
            if (new_count == 0) {
                pthread_mutex_lock(&class->lock);
                /* 加锁后双重检查 */
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
        /* 该slab已满，尝试下一个 */
        slab = __atomic_load_n(&slab->next, __ATOMIC_ACQUIRE);
    }
    
    /* 慢速路径：需要获取新slab（需加锁） */
    pthread_mutex_lock(&class->lock);
    
    /* 加锁后重新检查partial_slabs */
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
    
    /* 从空闲缓存获取或分配新slab */
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
    
    /* 添加到partial列表 */
    slab_list_add_head(&class->partial_slabs, slab);
    __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
    
    /* 从新slab分配 */
    int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
    __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
    void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);
    
    pthread_mutex_unlock(&class->lock);
    return result;
}

/* 释放到slab - P2修复：使用原子操作的无锁快速路径 */
void numa_slab_free(void *ptr, size_t total_size, int node) {
    if (!slab_ctx.initialized || !ptr) {
        return;
    }
    
    /* P2修复：使用页对齐和slab头部实现O(1)slab查找 */
    uintptr_t ptr_addr = (uintptr_t)ptr;
    uintptr_t slab_base = ptr_addr & ~((uintptr_t)(SLAB_SIZE - 1));
    numa_slab_header_t *header = (numa_slab_header_t *)slab_base;
    
    /* 验证魔数 */
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
    
    /* 计算对象索引（考虑头部） */
    size_t offset = (char *)ptr - (char *)slab->memory - SLAB_HEADER_SIZE;
    int obj_index = offset / class->obj_size;
    
    if (obj_index < 0 || obj_index >= (int)slab->objects_per_slab) {
        return;
    }
    
    /* 无锁：原子清除bit并增加free_count */
    bitmap_clear(slab->bitmap, obj_index);
    uint16_t old_count = __atomic_fetch_add(&slab->free_count, 1, __ATOMIC_ACQ_REL);
    uint16_t new_count = old_count + 1;
    
    /* 检查是否需要在列表间移动slab（需加锁） */
    int was_full = (old_count == 0);
    int is_empty = (new_count == slab->objects_per_slab);
    
    if (was_full || is_empty) {
        pthread_mutex_lock(&class->lock);
        
        int current_list = __atomic_load_n(&slab->list_type, __ATOMIC_ACQUIRE);
        uint16_t current_count = __atomic_load_n(&slab->free_count, __ATOMIC_ACQUIRE);
        
        if (was_full && current_list == SLAB_LIST_FULL) {
            /* 从 full 移到 partial */
            slab_list_remove(&class->full_slabs, slab);
            slab_list_add_head(&class->partial_slabs, slab);
            __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
        } else if (current_count == slab->objects_per_slab && current_list == SLAB_LIST_PARTIAL) {
            /* 从 partial 移到 empty/free */
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
