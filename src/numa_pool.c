/* numa_pool.c - NUMA Slab 分配器实现（jemalloc 风格，覆盖 8B-64KB）
 *
 * 设计原则：
 * - 33 级大小 class：8B~64KB，消除内部碎片
 * - 小 slab 64KB（≤4KB 对象）+ 大 slab 2MB（>4KB 对象）
 * - 16 字节 PREFIX + 带回指针头部，O(1) free 查找
 * - >4KB 对象走大 slab，消除 per-object mmap page 对齐浪费
 */

#define _GNU_SOURCE
#include "numa_pool.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <numa.h>
#include <numaif.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>

/* 33 级 jemalloc 风格大小 class（覆盖 8B-64KB） */
const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES] = {
    8, 16, 24, 32, 48, 64, 80, 96, 128,              /* 小对象（8/16 字节粒度） */
    160, 192, 256, 320, 384, 512, 640, 768,            /* 中对象（32-64 字节粒度） */
    1024, 1280, 1536, 2048, 2560, 3072, 4096,          /* 大对象（128-256 字节粒度） */
    5120, 6144, 7168, 8192, 10240, 12288, 16384, 32768, 65536 /* 超大对象（>4KB，走大 slab） */
};

/* ============================================================================
 * Slab 分配器实现（jemalloc 风格，覆盖 8B-64KB）
 * ============================================================================
 * 设计：
 * - 33 级 jemalloc 风格大小 class，覆盖 8B~64KB
 * - 小 slab 64KB（≤4KB 对象）+ 大 slab 2MB（>4KB 对象）
 * - 带回指针的 Slab 头部，支持 O(1) free 查找
 * - 部分占用/全占用/空闲 三态链表管理
 * ========================================================================= */

/* 每个slab头部存储在slab开头，用于O(1) free查找 */
#define SLAB_HEADER_MAGIC 0x534C4142  /* ASCII中的"SLAB" */
#define LARGE_SLAB_HEADER_MAGIC 0x4C534C42  /* ASCII中的"LSLB" */
typedef struct numa_slab_header {
    uint32_t magic;                  /* 魔数，用于验证 */
    uint32_t class_idx;              /* 大小分类索引 */
    struct numa_slab *slab;          /* 回指针，指向slab结构 */
    void *raw_memory;                /* 原始未对齐内存，用于numa_free */
} numa_slab_header_t;

#define SLAB_HEADER_SIZE (sizeof(numa_slab_header_t))
#define SLAB_USABLE_SIZE (SLAB_SIZE - SLAB_HEADER_SIZE)
#define LARGE_SLAB_USABLE_SIZE (LARGE_SLAB_SIZE - SLAB_HEADER_SIZE)

/* 判断 class 是否为大 slab class（>4KB 对象） */
static inline int is_large_slab_class(int class_idx) {
    return class_idx >= 24;  /* class 24-32 对应 5KB-64KB */
}

/* 获取 slab 大小（按 class） */
static inline size_t slab_size_for_class(int class_idx) {
    return is_large_slab_class(class_idx) ? LARGE_SLAB_SIZE : SLAB_SIZE;
}

/* 获取 slab 对齐大小（按 class） */
static inline size_t slab_align_for_class(int class_idx) {
    return is_large_slab_class(class_idx) ? LARGE_SLAB_SIZE : SLAB_SIZE;
}

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
    numa_slab_t *current_slab;       /* 快速路径使用的当前slab指针 */
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

    if (is_large_slab_class(class_idx)) {
        /* 大 slab（>4KB 对象）：mmap + mbind 方式，借鉴 memkind arena_extent_alloc
         * 分配 2x 大小，对齐到 LARGE_SLAB_SIZE，munmap 头尾减少 RSS */
        size_t alloc_size = LARGE_SLAB_SIZE * 2;
        void *raw_mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw_mem == MAP_FAILED) {
            free(slab);
            return NULL;
        }

        /* 对齐到 LARGE_SLAB_SIZE 边界 */
        uintptr_t raw_addr = (uintptr_t)raw_mem;
        uintptr_t aligned_addr = (raw_addr + LARGE_SLAB_SIZE - 1) & ~((uintptr_t)(LARGE_SLAB_SIZE - 1));

        /* munmap 未对齐的头部和尾部，减少 RSS 浪费 */
        size_t head_len = aligned_addr - raw_addr;
        if (head_len > 0) munmap(raw_mem, head_len);

        uintptr_t tail = aligned_addr + LARGE_SLAB_SIZE;
        size_t tail_len = (raw_addr + alloc_size) - tail;
        if (tail_len > 0) munmap((void *)tail, tail_len);

        /* mbind 到目标 NUMA 节点 */
        struct bitmask *nodemask = numa_allocate_nodemask();
        if (!nodemask) {
            munmap((void *)aligned_addr, LARGE_SLAB_SIZE);
            free(slab);
            return NULL;
        }
        numa_bitmask_setbit(nodemask, node);
        long rc = mbind((void *)aligned_addr, LARGE_SLAB_SIZE,
                        MPOL_BIND, nodemask->maskp, nodemask->size, 0);
        numa_free_nodemask(nodemask);
        if (rc < 0) {
            munmap((void *)aligned_addr, LARGE_SLAB_SIZE);
            free(slab);
            return NULL;
        }

        slab->memory = (void *)aligned_addr;

        /* 初始化头部 */
        numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
        header->magic = LARGE_SLAB_HEADER_MAGIC;
        header->class_idx = class_idx;
        header->slab = slab;
        header->raw_memory = NULL; /* 大 slab 直接 munmap，不需要 raw 指针 */
    } else {
        /* 小 slab（≤4KB 对象）：沿用 numa_alloc_onnode + 2x 对齐 */
        void *raw_mem = numa_alloc_onnode(SLAB_SIZE * 2, node);
        if (!raw_mem) {
            free(slab);
            return NULL;
        }

        uintptr_t raw_addr = (uintptr_t)raw_mem;
        uintptr_t aligned_addr = (raw_addr + SLAB_SIZE - 1) & ~((uintptr_t)(SLAB_SIZE - 1));
        slab->memory = (void *)aligned_addr;

        numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
        header->magic = SLAB_HEADER_MAGIC;
        header->class_idx = class_idx;
        header->slab = slab;
        header->raw_memory = raw_mem;
    }
    
    /* 初始化slab */
    memset(slab->bitmap, 0, sizeof(slab->bitmap));
    /* 使用可用大小（头部后）计算每个slab的对象数 */
    size_t usable_size = is_large_slab_class(class_idx) ? LARGE_SLAB_USABLE_SIZE : SLAB_USABLE_SIZE;
    slab->objects_per_slab = usable_size / obj_size;
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
        numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
        if (is_large_slab_class(slab->class_idx)) {
            /* 大 slab：munmap 对齐后的 2MB 区域 */
            munmap(slab->memory, LARGE_SLAB_SIZE);
        } else {
            /* 小 slab：numa_free 原始 2x 对齐区域 */
            numa_free(header->raw_memory, SLAB_SIZE * 2);
        }
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

            /* 初始化所有 33 个大小 class（8B-64KB 统一走 Slab） */
            size_t obj_size = numa_pool_size_classes[j];

            class->obj_size = obj_size + 16;  /* 包含PREFIX */
            class->partial_slabs = NULL;
            class->full_slabs = NULL;
            class->empty_slabs = NULL;
            class->current_slab = NULL;
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

/* 从 slab 分配 - 快速路径只使用 current_slab 指针，避免链表遍历的 use-after-free */
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

    if (class_idx < 0) {
        return NULL;  /* 超出大小 class 范围 */
    }

    /* 验证节点 */
    if (node < 0 || node >= slab_ctx.num_nodes) {
        node = 0;
    }

    numa_slab_class_t *class = &slab_ctx.slab_nodes[node].classes[class_idx];
    size_t aligned_size = (size + 15) & ~15;  /* 16-byte align */
    *total_size = aligned_size + 16;  /* Include PREFIX */

    /* 快速路径：无锁尝试从 current_slab 分配 */
    numa_slab_t *slab = __atomic_load_n(&class->current_slab, __ATOMIC_ACQUIRE);
    if (slab) {
        int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
        if (free_bit >= 0) {
            /* 成功占用一个插槽 */
            uint16_t new_count = __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);

            /* 计算对象地址（跳过头部） */
            void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);

            /* 如果 slab 已满，清除 current_slab（慢路径会选新的） */
            if (new_count == 0) {
                __atomic_store_n(&class->current_slab, NULL, __ATOMIC_RELEASE);
            }
            return result;
        }
        /* current_slab 已满，清除它 */
        __atomic_store_n(&class->current_slab, NULL, __ATOMIC_RELEASE);
    }

    /* 慢速路径：加锁获取新 slab */
    pthread_mutex_lock(&class->lock);

    /* 重新检查 current_slab（可能被其他线程更新） */
    slab = class->current_slab;
    if (slab) {
        int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
        if (free_bit >= 0) {
            __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
            void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);
            if (__atomic_load_n(&slab->free_count, __ATOMIC_ACQUIRE) == 0) {
                class->current_slab = NULL;
                slab_list_remove(&class->partial_slabs, slab);
                slab_list_add_head(&class->full_slabs, slab);
                __atomic_store_n(&slab->list_type, SLAB_LIST_FULL, __ATOMIC_RELEASE);
            }
            pthread_mutex_unlock(&class->lock);
            return result;
        }
        class->current_slab = NULL;
    }

    /* 从 partial_slabs 链表找可用的 slab，已满的清理到 full_slabs */
    slab = class->partial_slabs;
    while (slab) {
        numa_slab_t *next = slab->next;
        int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
        if (free_bit >= 0) {
            __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
            void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);
            if (__atomic_load_n(&slab->free_count, __ATOMIC_ACQUIRE) == 0) {
                slab_list_remove(&class->partial_slabs, slab);
                slab_list_add_head(&class->full_slabs, slab);
                __atomic_store_n(&slab->list_type, SLAB_LIST_FULL, __ATOMIC_RELEASE);
            } else {
                class->current_slab = slab;
            }
            pthread_mutex_unlock(&class->lock);
            return result;
        }
        /* slab 已满，移到 full_slabs 避免后续重复遍历 */
        slab_list_remove(&class->partial_slabs, slab);
        slab_list_add_head(&class->full_slabs, slab);
        __atomic_store_n(&slab->list_type, SLAB_LIST_FULL, __ATOMIC_RELEASE);
        slab = next;
    }

    /* 从空闲缓存获取或分配新 slab（批量分配减少系统调用） */
    if (class->empty_slabs) {
        slab = class->empty_slabs;
        slab_list_remove(&class->empty_slabs, slab);
        class->empty_count--;
    } else {
        /* 批量分配：对象少的 class 多预分配几个 slab */
        int batch_size = (slab_ctx.slab_nodes[node].classes[class_idx].obj_size > 512) ? 4 : 2;

        for (int i = 0; i < batch_size; i++) {
            numa_slab_t *new_slab = alloc_new_slab(node, class->obj_size, class_idx);
            if (!new_slab) {
                if (i == 0) {
                    /* 一个都没分配成功 */
                    pthread_mutex_unlock(&class->lock);
                    return NULL;
                }
                /* 部分成功，使用已分配的 */
                break;
            }
            slab_list_add_head(&class->empty_slabs, new_slab);
            class->empty_count++;
            class->slabs_count++;
        }

        slab = class->empty_slabs;
        slab_list_remove(&class->empty_slabs, slab);
        class->empty_count--;
    }

    /* 添加到 partial 列表并设为 current_slab */
    slab_list_add_head(&class->partial_slabs, slab);
    __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
    class->current_slab = slab;

    /* 从新 slab 分配 */
    int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
    __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
    void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);

    /* 清理多余的 empty slabs */
    while (class->empty_count > SLAB_EMPTY_CACHE_MAX) {
        numa_slab_t *es = class->empty_slabs;
        if (!es) break;
        slab_list_remove(&class->empty_slabs, es);
        class->empty_count--;
        free_slab(es);
        class->slabs_count--;
    }

    pthread_mutex_unlock(&class->lock);
    return result;
}

/* 释放到slab - P2修复：使用原子操作的无锁快速路径 */
void numa_slab_free(void *ptr, size_t total_size, int node) {
    if (!slab_ctx.initialized || !ptr) {
        return;
    }

    /* P2修复：使用页对齐和slab头部实现O(1)slab查找
     * 先尝试小 slab 对齐（64KB），再尝试大 slab 对齐（2MB） */
    uintptr_t ptr_addr = (uintptr_t)ptr;

    /* 先尝试小 slab 对齐 */
    uintptr_t slab_base = ptr_addr & ~((uintptr_t)(SLAB_SIZE - 1));
    numa_slab_header_t *header = (numa_slab_header_t *)slab_base;

    if (header->magic != SLAB_HEADER_MAGIC) {
        /* 小 slab 未命中，尝试大 slab 对齐（2MB） */
        slab_base = ptr_addr & ~((uintptr_t)(LARGE_SLAB_SIZE - 1));
        header = (numa_slab_header_t *)slab_base;

        if (header->magic != LARGE_SLAB_HEADER_MAGIC) {
            return;
        }
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
            if (!class->current_slab) {
                class->current_slab = slab;
            }
        } else if (current_count == slab->objects_per_slab && current_list == SLAB_LIST_PARTIAL) {
            /* 从 partial 移到 empty/free */
            slab_list_remove(&class->partial_slabs, slab);
            if (class->current_slab == slab) {
                class->current_slab = NULL;
            }

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

/* 获取 NUMA 节点数量（兼容旧接口） */
int numa_pool_num_nodes(void)
{
    if (slab_ctx.initialized) {
        return slab_ctx.num_nodes;
    }
    /* 回退：直接查询 libnuma */
    if (numa_available() < 0) {
        return 1;
    }
    return numa_max_node() + 1;
}

/* 获取当前 NUMA 节点（兼容旧接口） */
int numa_pool_get_node(void)
{
    int cpu = sched_getcpu();
    if (cpu >= 0) {
        return numa_node_of_cpu(cpu);
    }
    return 0;
}

/* 检查 NUMA 是否可用（兼容旧接口） */
int numa_pool_available(void)
{
    return slab_ctx.initialized ? 1 : (numa_available() >= 0 ? 1 : 0);
}
