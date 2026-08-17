/* numa_pool.c - NUMA slab allocator implementation (jemalloc-style, covering 8B-64KB).
 *
 * Design principles:
 * - 33 size classes: 8B-64KB, eliminating internal fragmentation
 * - Small slabs of 64KB (objects <= 4KB) + large slabs of 2MB (objects > 4KB)
 * - 16-byte PREFIX plus a back-pointer header for O(1) free lookup
 * - Objects larger than 4KB use large slabs, avoiding per-object mmap page alignment waste
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

/* 33 jemalloc-style size classes (covering 8B-64KB). */
const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES] = {
    8, 16, 24, 32, 48, 64, 80, 96, 128,              /* Small objects (8/16-byte granularity). */
    160, 192, 256, 320, 384, 512, 640, 768,            /* Medium objects (32-64-byte granularity). */
    1024, 1280, 1536, 2048, 2560, 3072, 4096,          /* Large objects (128-256-byte granularity). */
    5120, 6144, 7168, 8192, 10240, 12288, 16384, 32768, 65536 /* Extra-large objects (>4KB, use large slabs). */
};

/* ============================================================================
 * Slab allocator implementation (jemalloc-style, covering 8B-64KB).
 * ============================================================================
 * Design:
 * - 33 jemalloc-style size classes, covering 8B-64KB
 * - Small slabs of 64KB (objects <= 4KB) + large slabs of 2MB (objects > 4KB)
 * - Slab header with a back pointer for O(1) free lookup
 * - Partial/full/empty three-state linked-list management
 * ========================================================================= */

/* Each slab header is stored at the start of the slab for O(1) free lookup. */
#define SLAB_HEADER_MAGIC 0x534C4142  /* "SLAB" in ASCII. */
#define LARGE_SLAB_HEADER_MAGIC 0x4C534C42  /* "LSLB" in ASCII. */
typedef struct numa_slab_header {
    uint32_t magic;                  /* Magic number for validation. */
    uint32_t class_idx;              /* Size class index. */
    struct numa_slab *slab;          /* Back pointer to the slab structure. */
    void *raw_memory;                /* Raw unaligned memory, kept for numa_free. */
} numa_slab_header_t;

#define SLAB_HEADER_SIZE (sizeof(numa_slab_header_t))
#define SLAB_USABLE_SIZE (SLAB_SIZE - SLAB_HEADER_SIZE)
#define LARGE_SLAB_USABLE_SIZE (LARGE_SLAB_SIZE - SLAB_HEADER_SIZE)

/* Return 1 if the class uses large slabs (objects > 4KB). */
static inline int is_large_slab_class(int class_idx) {
    return class_idx >= 24;  /* Classes 24-32 map to 5KB-64KB. */
}

/* Return the slab size for a class. */
static inline size_t slab_size_for_class(int class_idx) {
    return is_large_slab_class(class_idx) ? LARGE_SLAB_SIZE : SLAB_SIZE;
}

/* Return the slab alignment for a class. */
static inline size_t slab_align_for_class(int class_idx) {
    return is_large_slab_class(class_idx) ? LARGE_SLAB_SIZE : SLAB_SIZE;
}

/* Slab structure - P2 fix: atomic counters for lock-free operation. */
typedef struct numa_slab {
    void *memory;                    /* Actual memory address (NUMA-allocated). */
    struct numa_slab *next;          /* Next slab in the list. */
    struct numa_slab *prev;          /* Previous slab (P2 fix: O(1) removal). */
    uint32_t bitmap[SLAB_BITMAP_SIZE]; /* 128-bit bitmap for object allocation (atomic access). */
    _Atomic uint16_t free_count;     /* Number of free objects (atomic). */
    uint16_t objects_per_slab;       /* Total number of objects per slab. */
    int node_id;                     /* NUMA node ID of this slab. */
    int class_idx;                   /* Size class index. */
    _Atomic int list_type;           /* 0=partial, 1=full, 2=empty (atomic). */
} numa_slab_t;

/* Slab list type constants. */
#define SLAB_LIST_PARTIAL 0
#define SLAB_LIST_FULL    1
#define SLAB_LIST_EMPTY   2

/* Size class (one per size class). */
typedef struct {
    size_t obj_size;                 /* Object size (includes PREFIX). */
    numa_slab_t *partial_slabs;      /* Partially used slabs. */
    numa_slab_t *full_slabs;         /* Fully allocated slabs. */
    numa_slab_t *empty_slabs;        /* Empty slabs (cached). */
    numa_slab_t *current_slab;       /* Current slab pointer for the fast path. */
    size_t empty_count;              /* Number of cached empty slabs. */
    pthread_mutex_t lock;            /* Thread safety. */
    size_t slabs_count;              /* Total number of allocated slabs. */
} numa_slab_class_t;

/* Per-node slab pool. */
typedef struct {
    int node_id;
    numa_slab_class_t classes[NUMA_POOL_SIZE_CLASSES];
} numa_slab_node_t;

/* Global slab context. */
static struct {
    int initialized;
    int num_nodes;
    numa_slab_node_t *slab_nodes;
} slab_ctx = {
    .initialized = 0,
    .num_nodes = 0,
    .slab_nodes = NULL
};

/* Bitmap operations - P2 fix: lock-free using atomic operations. */
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

/* Try to atomically set a bit; return 1 on success (bit was 0), 0 if already set. */
static inline int bitmap_try_set(uint32_t *bitmap, int bit) {
    uint32_t mask = 1U << (bit % 32);
    uint32_t old = __atomic_fetch_or(&bitmap[bit / 32], mask, __ATOMIC_ACQ_REL);
    return (old & mask) == 0;  /* 1 means the bit was claimed, 0 means it was already set. */
}

/* Find the first free bit using CPU instructions (O(1) per 32-bit word).
 * P2 fix: lock-free version using atomic reads. */
static int bitmap_find_first_free(uint32_t *bitmap, int max_bits) {
    int num_words = (max_bits + 31) / 32;
    for (int i = 0; i < num_words; i++) {
        /* Atomically read the current bitmap state. */
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

/* Lock-free find-and-set: locate a free bit and set it atomically.
 * Returns the bit index on success, -1 if no free bit exists. */
static int bitmap_find_and_set(uint32_t *bitmap, int max_bits) {
    int num_words = (max_bits + 31) / 32;
    for (int i = 0; i < num_words; i++) {
        uint32_t word = __atomic_load_n(&bitmap[i], __ATOMIC_ACQUIRE);
        while (~word != 0) {  /* While there is a free bit. */
            uint32_t inverted = ~word;
            int bit_pos = __builtin_ffs(inverted) - 1;
            int global_pos = i * 32 + bit_pos;
            if (global_pos >= max_bits) break;
            
            /* Try to atomically set the bit. */
            uint32_t mask = 1U << bit_pos;
            uint32_t expected = word;
            uint32_t desired = word | mask;
            if (__atomic_compare_exchange_n(&bitmap[i], &expected, desired,
                                           0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return global_pos;  /* Successfully claimed the bit. */
            }
            /* CAS failed, reload and retry. */
            word = expected;
        }
    }
    return -1;
}

/* P2 fix: helper - remove a slab from the doubly linked list in O(1). */
static inline void slab_list_remove(numa_slab_t **list_head, numa_slab_t *slab) {
    if (slab->prev) {
        slab->prev->next = slab->next;
    } else {
        /* The slab is the head node. */
        *list_head = slab->next;
    }
    if (slab->next) {
        slab->next->prev = slab->prev;
    }
    slab->prev = NULL;
    slab->next = NULL;
}

/* P2 fix: helper - add a slab to the head of the doubly linked list in O(1). */
static inline void slab_list_add_head(numa_slab_t **list_head, numa_slab_t *slab) {
    slab->prev = NULL;
    slab->next = *list_head;
    if (*list_head) {
        (*list_head)->prev = slab;
    }
    *list_head = slab;
}

/* Allocate a new memory-aligned slab. */
static numa_slab_t *alloc_new_slab(int node, size_t obj_size, int class_idx) {
    /* Allocate the slab structure. */
    numa_slab_t *slab = (numa_slab_t *)malloc(sizeof(numa_slab_t));
    if (!slab) return NULL;

    if (is_large_slab_class(class_idx)) {
        /* Large slab (objects > 4KB): mmap + mbind, modeled after memkind arena_extent_alloc.
         * Allocate 2x the size, align to LARGE_SLAB_SIZE, munmap head/tail to reduce RSS. */
        size_t alloc_size = LARGE_SLAB_SIZE * 2;
        void *raw_mem = mmap(NULL, alloc_size, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw_mem == MAP_FAILED) {
            free(slab);
            return NULL;
        }

        /* Align to the LARGE_SLAB_SIZE boundary. */
        uintptr_t raw_addr = (uintptr_t)raw_mem;
        uintptr_t aligned_addr = (raw_addr + LARGE_SLAB_SIZE - 1) & ~((uintptr_t)(LARGE_SLAB_SIZE - 1));

        /* munmap the unaligned head and tail to reduce RSS waste. */
        size_t head_len = aligned_addr - raw_addr;
        if (head_len > 0) munmap(raw_mem, head_len);

        uintptr_t tail = aligned_addr + LARGE_SLAB_SIZE;
        size_t tail_len = (raw_addr + alloc_size) - tail;
        if (tail_len > 0) munmap((void *)tail, tail_len);

        /* mbind to the target NUMA node. */
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

        /* Initialize the header. */
        numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
        header->magic = LARGE_SLAB_HEADER_MAGIC;
        header->class_idx = class_idx;
        header->slab = slab;
        header->raw_memory = NULL; /* Large slabs are munmapped directly, no raw pointer needed. */
    } else {
        /* Small slab (objects <= 4KB): use numa_alloc_onnode with 2x alignment. */
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
    
    /* Initialize the slab. */
    memset(slab->bitmap, 0, sizeof(slab->bitmap));
    /* Compute objects per slab from the usable size (after the header). */
    size_t usable_size = is_large_slab_class(class_idx) ? LARGE_SLAB_USABLE_SIZE : SLAB_USABLE_SIZE;
    slab->objects_per_slab = usable_size / obj_size;
    __atomic_store_n(&slab->free_count, slab->objects_per_slab, __ATOMIC_RELEASE);
    slab->next = NULL;
    slab->prev = NULL;  /* P2 fix: initialize the prev pointer. */
    slab->node_id = node;
    slab->class_idx = class_idx;
    __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
    
    return slab;
}

/* Release a slab. */
static void free_slab(numa_slab_t *slab) {
    if (slab->memory) {
        numa_slab_header_t *header = (numa_slab_header_t *)slab->memory;
        if (is_large_slab_class(slab->class_idx)) {
            /* Large slab: munmap the aligned 2MB region. */
            munmap(slab->memory, LARGE_SLAB_SIZE);
        } else {
            /* Small slab: numa_free the original 2x aligned region. */
            numa_free(header->raw_memory, SLAB_SIZE * 2);
        }
    }
    free(slab);
}

/* Initialize the slab allocator. */
int numa_slab_init(void) {
    if (slab_ctx.initialized) {
        return 0;
    }
    
    /* Check NUMA availability. */
    if (numa_available() < 0) {
        slab_ctx.num_nodes = 1;
    } else {
        slab_ctx.num_nodes = numa_max_node() + 1;
    }
    
    /* Allocate the per-node structures. */
    slab_ctx.slab_nodes = (numa_slab_node_t *)calloc(
        slab_ctx.num_nodes, sizeof(numa_slab_node_t));
    if (!slab_ctx.slab_nodes) {
        return -1;
    }
    
    /* Initialize the slab classes for each node. */
    for (int i = 0; i < slab_ctx.num_nodes; i++) {
        slab_ctx.slab_nodes[i].node_id = i;
        
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            numa_slab_class_t *class = &slab_ctx.slab_nodes[i].classes[j];

            /* Initialize all 33 size classes (8B-64KB all go through slabs). */
            size_t obj_size = numa_pool_size_classes[j];

            class->obj_size = obj_size + 16;  /* Includes the PREFIX. */
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

/* Clean up the slab allocator. */
void numa_slab_cleanup(void) {
    if (!slab_ctx.initialized) {
        return;
    }
    
    for (int i = 0; i < slab_ctx.num_nodes; i++) {
        for (int j = 0; j < NUMA_POOL_SIZE_CLASSES; j++) {
            numa_slab_class_t *class = &slab_ctx.slab_nodes[i].classes[j];
            
            if (class->obj_size == 0) continue;
            
            /* Release all slabs from all lists. */
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

/* Allocate from a slab - the fast path only uses current_slab, avoiding use-after-free on list traversal. */
void *numa_slab_alloc(size_t size, int node, size_t *total_size) {
    if (!slab_ctx.initialized) {
        return NULL;
    }

    /* Find the appropriate size class. */
    int class_idx = -1;
    for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {
        if (size <= numa_pool_size_classes[i]) {
            class_idx = i;
            break;
        }
    }

    if (class_idx < 0) {
        return NULL;  /* Size exceeds the class range. */
    }

    /* Validate the node. */
    if (node < 0 || node >= slab_ctx.num_nodes) {
        node = 0;
    }

    numa_slab_class_t *class = &slab_ctx.slab_nodes[node].classes[class_idx];
    size_t aligned_size = (size + 15) & ~15;  /* 16-byte align */
    *total_size = aligned_size + 16;  /* Include PREFIX */

    /* Fast path: lock-free attempt to allocate from current_slab. */
    numa_slab_t *slab = __atomic_load_n(&class->current_slab, __ATOMIC_ACQUIRE);
    if (slab) {
        int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
        if (free_bit >= 0) {
            /* Successfully claimed a slot. */
            uint16_t new_count = __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);

            /* Compute the object address (skip the header). */
            void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);

            /* If the slab is full, clear current_slab (the slow path will pick a new one). */
            if (new_count == 0) {
                __atomic_store_n(&class->current_slab, NULL, __ATOMIC_RELEASE);
            }
            return result;
        }
        /* current_slab is full, clear it. */
        __atomic_store_n(&class->current_slab, NULL, __ATOMIC_RELEASE);
    }

    /* Slow path: lock and acquire a new slab. */
    pthread_mutex_lock(&class->lock);

    /* Re-check current_slab (may have been updated by another thread). */
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

    /* Look for a usable slab in the partial_slabs list, moving full ones to full_slabs. */
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
        /* Slab is full, move it to full_slabs to avoid repeated traversal. */
        slab_list_remove(&class->partial_slabs, slab);
        slab_list_add_head(&class->full_slabs, slab);
        __atomic_store_n(&slab->list_type, SLAB_LIST_FULL, __ATOMIC_RELEASE);
        slab = next;
    }

    /* Take from the empty cache or allocate a new slab (batch allocation reduces syscalls). */
    if (class->empty_slabs) {
        slab = class->empty_slabs;
        slab_list_remove(&class->empty_slabs, slab);
        class->empty_count--;
    } else {
        /* Batch allocation: classes with fewer objects get extra preallocated slabs. */
        int batch_size = (slab_ctx.slab_nodes[node].classes[class_idx].obj_size > 512) ? 4 : 2;

        for (int i = 0; i < batch_size; i++) {
            numa_slab_t *new_slab = alloc_new_slab(node, class->obj_size, class_idx);
            if (!new_slab) {
                if (i == 0) {
                    /* None were allocated successfully. */
                    pthread_mutex_unlock(&class->lock);
                    return NULL;
                }
                /* Partial success, use what was allocated. */
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

    /* Add to the partial list and set as current_slab. */
    slab_list_add_head(&class->partial_slabs, slab);
    __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
    class->current_slab = slab;

    /* Allocate from the new slab. */
    int free_bit = bitmap_find_and_set(slab->bitmap, slab->objects_per_slab);
    __atomic_sub_fetch(&slab->free_count, 1, __ATOMIC_ACQ_REL);
    void *result = (char *)slab->memory + SLAB_HEADER_SIZE + (free_bit * class->obj_size);

    pthread_mutex_unlock(&class->lock);
    return result;
}

/* Free to a slab - P2 fix: lock-free fast path using atomic operations. */
void numa_slab_free(void *ptr, size_t total_size, int node) {
    if (!slab_ctx.initialized || !ptr) {
        return;
    }

    /* P2 fix: page alignment plus the slab header enable O(1) slab lookup.
     * Try small slab alignment (64KB) first, then large slab alignment (2MB). */
    uintptr_t ptr_addr = (uintptr_t)ptr;

    /* Try small slab alignment first. */
    uintptr_t slab_base = ptr_addr & ~((uintptr_t)(SLAB_SIZE - 1));
    numa_slab_header_t *header = (numa_slab_header_t *)slab_base;

    if (header->magic != SLAB_HEADER_MAGIC) {
        /* Small slab missed, try large slab alignment (2MB). */
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
    
    /* Compute the object index (accounting for the header). */
    size_t offset = (char *)ptr - (char *)slab->memory - SLAB_HEADER_SIZE;
    int obj_index = offset / class->obj_size;
    
    if (obj_index < 0 || obj_index >= (int)slab->objects_per_slab) {
        return;
    }
    
    /* Lock-free: atomically clear the bit and bump free_count. */
    bitmap_clear(slab->bitmap, obj_index);
    uint16_t old_count = __atomic_fetch_add(&slab->free_count, 1, __ATOMIC_ACQ_REL);
    uint16_t new_count = old_count + 1;
    
    /* Check whether the slab must move between lists (requires the lock). */
    int was_full = (old_count == 0);
    int is_empty = (new_count == slab->objects_per_slab);
    
    if (was_full || is_empty) {
        pthread_mutex_lock(&class->lock);

        int current_list = __atomic_load_n(&slab->list_type, __ATOMIC_ACQUIRE);
        uint16_t current_count = __atomic_load_n(&slab->free_count, __ATOMIC_ACQUIRE);

        if (was_full && current_list == SLAB_LIST_FULL) {
            /* Move from full to partial. */
            slab_list_remove(&class->full_slabs, slab);
            slab_list_add_head(&class->partial_slabs, slab);
            __atomic_store_n(&slab->list_type, SLAB_LIST_PARTIAL, __ATOMIC_RELEASE);
            if (!class->current_slab) {
                class->current_slab = slab;
            }
        } else if (current_count == slab->objects_per_slab && current_list == SLAB_LIST_PARTIAL) {
            /* Move from partial to empty/free. */
            slab_list_remove(&class->partial_slabs, slab);
            if (class->current_slab == slab) {
                class->current_slab = NULL;
            }

            /* Do not munmap/free_slab here: the lock-free fast path of
             * numa_slab_alloc may still hold this slab as current_slab, and if
             * another thread released the memory at this point, a CAS on the
             * unmapped bitmap would be a use-after-free. Empty slabs are kept
             * in the empty cache and reclaimed by numa_slab_cleanup() at
             * shutdown (bounded by the peak watermark). */
            slab_list_add_head(&class->empty_slabs, slab);
            __atomic_store_n(&slab->list_type, SLAB_LIST_EMPTY, __ATOMIC_RELEASE);
            class->empty_count++;
        }

        pthread_mutex_unlock(&class->lock);
    }
}

/* Return the number of NUMA nodes (compatibility with the old interface). */
int numa_pool_num_nodes(void)
{
    if (slab_ctx.initialized) {
        return slab_ctx.num_nodes;
    }
    /* Fallback: query libnuma directly. */
    if (numa_available() < 0) {
        return 1;
    }
    return numa_max_node() + 1;
}

/* Return the current NUMA node (compatibility with the old interface). */
int numa_pool_get_node(void)
{
    int cpu = sched_getcpu();
    if (cpu >= 0) {
        return numa_node_of_cpu(cpu);
    }
    return 0;
}

/* Check whether NUMA is available (compatibility with the old interface). */
int numa_pool_available(void)
{
    return slab_ctx.initialized ? 1 : (numa_available() >= 0 ? 1 : 0);
}
