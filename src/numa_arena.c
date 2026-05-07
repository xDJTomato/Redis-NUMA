/* numa_arena.c - jemalloc风格NUMA感知arena分配器实现
 *
 * 三层架构：
 *   tcache  → __thread 无锁栈，90%+ 分配/释放在此完成
 *   bin     → per-size-class 中央池，bitmap slab 管理
 *   extent  → numa_alloc_onnode 大块，切分为 slab
 *
 * 位图操作：复刻 jemalloc 的 bitmap_sfu（Set First Unset）O(1)分配，
 * 使用 GCC __builtin_ctz 实现。
 */

#define _GNU_SOURCE
#include "numa_arena.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <numa.h>

/* 日志：redis-server 优先使用 server.o 的强定义，cli/benchmark 用弱回退 */
__attribute__((weak)) void _serverLog(int level, const char *fmt, ...) {
    (void)level; (void)fmt;  /* no-op for redis-cli/benchmark */
}
#define ARENA_LOG(fmt, ...) _serverLog(2 /*LL_NOTICE*/, "[Arena] " fmt, ##__VA_ARGS__)
#include <sched.h>
#include <unistd.h>

/* ── Size Class 表 ─────────────────────────────────────────────────

   20 级，从 16B 到 4096B。分组增长覆盖 Redis 典型对象：
   key SDS(~20B), robj(~24B), dictEntry(~24B), 小 value(~64-512B),
   中 value(~1-4KB)。>4KB 直接走 extent。 */

const size_t numa_arena_size_classes[NUMA_ARENA_SIZE_CLASSES] = {
    16,   32,   48,   64,     /* tiny:  Redis 小元数据 */
    80,   96,   128,  160,    /* small: SDS 短字符串 */
    192,  256,  320,  384,    /* medium-small */
    512,  768,  1024, 1536,   /* medium */
    2048, 2560, 3072, 4096    /* medium-large */
};

/* ── Bin 配置表（init 时填充）─────────────────────────────────── */

typedef struct {
    size_t slab_size;         /* slab 总大小 */
    int    nregs_per_slab;    /* 每个 slab 的 region 数 */
} bin_config_t;

static bin_config_t g_bin_configs[NUMA_ARENA_SIZE_CLASSES];

/* ── 位图操作 ────────────────────────────────────────────────────

   每 region 1 bit。bitmap_sfu() 找第一个 0 位并置 1（O(1)）。
   bitmap_size_bytes = (nregs + 7) / 8 */

/* 计算位图字节数 */
static inline int bitmap_bytes(int nregs) {
    return (nregs + 7) / 8;
}

/* bitmap_sfu: set first unset — 找第一个 0 位并置 1，返回位索引。
   等价于 jemalloc 的 bitmap_sfu()。
   按 uint32_t 遍历，用 __builtin_ctz 找字节内第一个 0 位。 */
static int bitmap_sfu(uint8_t *bitmap, int nregs) {
    int nwords = (nregs + 31) / 32;
    uint32_t *words = (uint32_t *)bitmap;
    for (int w = 0; w < nwords; w++) {
        uint32_t val = words[w];
        if (val != 0xFFFFFFFF) {
            /* 找到第一个 0 位：取反后 ctz */
            int bit = __builtin_ctz(~val);
            int idx = w * 32 + bit;
            if (idx < nregs) {
                words[w] |= (1U << bit);
                return idx;
            }
        }
    }
    return -1; /* slab 满 */
}

/* bitmap_unset: 清除指定位 */
static void bitmap_unset(uint8_t *bitmap, int idx) {
    uint32_t *words = (uint32_t *)bitmap;
    words[idx / 32] &= ~(1U << (idx % 32));
}

/* bitmap_test: 测试指定位是否已设置 */
static int bitmap_test(uint8_t *bitmap, int idx) {
    uint32_t *words = (uint32_t *)bitmap;
    return (words[idx / 32] >> (idx % 32)) & 1;
}

/* ── 全局状态 ──────────────────────────────────────────────────── */

static struct {
    int initialized;
    int num_nodes;
    numa_arena_t *arenas;       /* [num_nodes] */
} g_arena_ctx = {0};

static __thread tcache_t *g_tcache = NULL;

/* ── tcache 辅助 ──────────────────────────────────────────────── */

static tcache_t *tcache_get(void) {
    if (__builtin_expect(!g_tcache, 0)) {
        g_tcache = calloc(1, sizeof(tcache_t));
    }
    return g_tcache;
}

/* ── Size Class 查找 ────────────────────────────────────────────

   二分查找：返回第一个 >= size 的 class_idx。
   若 size > 4096 返回 -1（走 direct extent）。 */

static int size_to_class(size_t size) {
    if (size > NUMA_ARENA_MAX_ALLOC) return -1;
    int lo = 0, hi = NUMA_ARENA_SIZE_CLASSES - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (numa_arena_size_classes[mid] < size)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

/* ── 初始化 ────────────────────────────────────────────────────── */

int numa_arena_init(void) {
    if (g_arena_ctx.initialized) return 0;

    if (numa_available() == -1) return -1;

    g_arena_ctx.num_nodes = numa_max_node() + 1;
    if (g_arena_ctx.num_nodes > NUMA_ARENA_MAX_NODES)
        g_arena_ctx.num_nodes = NUMA_ARENA_MAX_NODES;

    g_arena_ctx.arenas = calloc(g_arena_ctx.num_nodes, sizeof(numa_arena_t));
    if (!g_arena_ctx.arenas) return -1;

    /* 计算每个 size-class 的 bin 配置 */
    for (int c = 0; c < NUMA_ARENA_SIZE_CLASSES; c++) {
        size_t reg_size = numa_arena_size_classes[c];
        /* slab_size: 必须为2的幂，ptr_to_extent依赖位掩码定位extent基址 */
        size_t slab_size = (reg_size <= 256)  ? (256 * 1024)  :
                           (reg_size <= 1024) ? (512 * 1024)  :
                                                (1024 * 1024);
        size_t usable = slab_size - sizeof(numa_extent_t);
        int nregs = (int)(usable / reg_size);
        if (nregs < 1) nregs = 1;
        if (nregs > 32768) nregs = 32768; /* 位图不超过4KB */

        /* 位图紧接extent头部 */
        size_t bitmap_sz = bitmap_bytes(nregs);
        usable = slab_size - sizeof(numa_extent_t) - bitmap_sz;
        nregs  = (int)(usable / reg_size);
        g_bin_configs[c].slab_size      = slab_size;  /* 2的幂，用于ptr_to_extent */
        g_bin_configs[c].nregs_per_slab = nregs;

        /* 初始化每个节点的 bin */
        for (int n = 0; n < g_arena_ctx.num_nodes; n++) {
            numa_bin_t *bin = &g_arena_ctx.arenas[n].bins[c];
            bin->reg_size       = reg_size;
            bin->slab_size      = slab_size;
            bin->nregs_per_slab = nregs;
            bin->slabcur        = NULL;
            bin->slabs_head     = NULL;
            pthread_mutex_init(&bin->lock, NULL);
        }
    }

    g_arena_ctx.initialized = 1;
    ARENA_LOG("init OK: %d nodes, %d classes. class0 slab=%zu nregs=%d",
              g_arena_ctx.num_nodes, NUMA_ARENA_SIZE_CLASSES,
              g_bin_configs[0].slab_size, g_bin_configs[0].nregs_per_slab);
    return 0;
}

void numa_arena_cleanup(void) {
    if (!g_arena_ctx.initialized) return;

    for (int n = 0; n < g_arena_ctx.num_nodes; n++) {
        for (int c = 0; c < NUMA_ARENA_SIZE_CLASSES; c++) {
            numa_bin_t *bin = &g_arena_ctx.arenas[n].bins[c];
            numa_extent_t *ext = bin->slabs_head;
            while (ext) {
                numa_extent_t *next = ext->next;
                numa_free(ext->memory, ext->size);
                ext = next;
            }
            bin->slabs_head = NULL;
            bin->slabcur    = NULL;
            pthread_mutex_destroy(&bin->lock);
        }
    }
    free(g_arena_ctx.arenas);
    g_arena_ctx.arenas = NULL;
    g_arena_ctx.initialized = 0;
}

int numa_arena_num_nodes(void) {
    return g_arena_ctx.num_nodes;
}

/* ── extent 操作 ───────────────────────────────────────────────── */

/* 分配新 extent → 直接 numa_alloc_onnode */
static numa_extent_t *extent_alloc(int node, int class_idx) {
    bin_config_t *cfg = &g_bin_configs[class_idx];
    size_t total = cfg->slab_size;

    void *mem = numa_alloc_onnode(total, node);
    if (!mem) return NULL;

    numa_extent_t *ext = (numa_extent_t *)mem;
    ext->memory = mem;
    ext->size   = total;
    ext->node_id = node;
    ext->nregs  = cfg->nregs_per_slab;
    ext->nfree  = cfg->nregs_per_slab;
    ext->next   = NULL;

    /* 清零位图 */
    int bm_bytes = bitmap_bytes(cfg->nregs_per_slab);
    memset(ext->bitmap, 0, bm_bytes);

    return ext;
}

static void extent_free(numa_extent_t *ext) {
    if (ext) numa_free(ext->memory, ext->size);
}

/* ptr_to_extent: 扫描bin的slab链表定位ptr所属extent。
   不使用位掩码（不依赖numa_alloc_onnode对齐到slab_size）。 */
static numa_extent_t *ptr_to_extent(numa_bin_t *bin, void *ptr) {
    numa_extent_t *ext = bin->slabs_head;
    while (ext) {
        if ((uintptr_t)ptr >= (uintptr_t)ext->memory &&
            (uintptr_t)ptr < (uintptr_t)ext->memory + ext->size) {
            return ext;
        }
        ext = ext->next;
    }
    return NULL;
}

/* 计算 region 在 extent 中的索引 */
static int ptr_region_idx(numa_extent_t *ext, void *ptr, int class_idx) {
    size_t reg_size    = numa_arena_size_classes[class_idx];
    int    bm_bytes    = bitmap_bytes(ext->nregs);
    uintptr_t data_start = (uintptr_t)ext->memory
                           + sizeof(numa_extent_t) + bm_bytes;
    return (int)(((uintptr_t)ptr - data_start) / reg_size);
}

/* extent 中 region 的地址 */
static void *region_ptr(numa_extent_t *ext, int idx, int class_idx) {
    (void)class_idx;
    int bm_bytes = bitmap_bytes(ext->nregs);
    uintptr_t data_start = (uintptr_t)ext->memory
                           + sizeof(numa_extent_t) + bm_bytes;
    return (void *)(data_start + idx * numa_arena_size_classes[class_idx]);
}

/* ── bin 操作（中央池，持锁）───────────────────────────────────── */

/* 从 bin 分配一个 region（已持锁时调用） */
static void *bin_alloc_locked(numa_bin_t *bin, int node, int class_idx) {
    /* 1. 尝试 slabcur */
    if (bin->slabcur) {
        int idx = bitmap_sfu(bin->slabcur->bitmap, bin->slabcur->nregs);
        if (idx >= 0) {
            bin->slabcur->nfree--;
            return region_ptr(bin->slabcur, idx, class_idx);
        }
    }

    /* 2. 扫描 slabs_head 找非满 slab */
    numa_extent_t *ext = bin->slabs_head;
    while (ext) {
        if (ext->nfree > 0) {
            int idx = bitmap_sfu(ext->bitmap, ext->nregs);
            if (idx >= 0) {
                ext->nfree--;
                bin->slabcur = ext;
                return region_ptr(ext, idx, class_idx);
            }
        }
        ext = ext->next;
    }

    /* 3. 分配新 extent */
    numa_extent_t *new_ext = extent_alloc(node, class_idx);
    if (!new_ext) return NULL;

    new_ext->next = bin->slabs_head;
    bin->slabs_head = new_ext;
    bin->slabcur    = new_ext;

    int idx = bitmap_sfu(new_ext->bitmap, new_ext->nregs);
    new_ext->nfree--;
    return region_ptr(new_ext, idx, class_idx);
}

/* bin batch refill: 持锁填满 tcache */
static int bin_refill(numa_bin_t *bin, int node, int class_idx,
                      tcache_bin_t *tcache_bin, int n) {
    pthread_mutex_lock(&bin->lock);
    int filled = 0;
    while (filled < n) {
        void *ptr = bin_alloc_locked(bin, node, class_idx);
        if (!ptr) break;
        tcache_bin->stack[filled++] = ptr;
    }
    pthread_mutex_unlock(&bin->lock);

    /* extent 分配在锁外完成（含 numa_alloc_onnode 系统调用） */
    while (filled < n) {
        numa_extent_t *new_ext = extent_alloc(node, class_idx);
        if (!new_ext) break;
        pthread_mutex_lock(&bin->lock);
        new_ext->next = bin->slabs_head;
        bin->slabs_head = new_ext;
        bin->slabcur    = new_ext;
        while (filled < n) {
            int idx = bitmap_sfu(new_ext->bitmap, new_ext->nregs);
            if (idx < 0) break;
            new_ext->nfree--;
            tcache_bin->stack[filled++] = region_ptr(new_ext, idx, class_idx);
        }
        pthread_mutex_unlock(&bin->lock);
    }
    tcache_bin->count = filled;
    return filled; /* 实际填充数 */
}

/* bin batch flush: 持锁归还一批 region */
static void bin_flush(numa_bin_t *bin, int class_idx,
                      void **ptrs, int n) {
    pthread_mutex_lock(&bin->lock);
    for (int i = 0; i < n; i++) {
        numa_extent_t *ext = ptr_to_extent(bin, ptrs[i]);
        int idx = ptr_region_idx(ext, ptrs[i], class_idx);
        if (idx >= 0 && idx < ext->nregs) {
            if (bitmap_test(ext->bitmap, idx)) {
                bitmap_unset(ext->bitmap, idx);
                ext->nfree++;
            }
        }

        /* 全空 extent：从链表移除并释放 */
        if (ext->nfree == ext->nregs) {
            if (bin->slabcur == ext) bin->slabcur = NULL;
            /* 从 slabs_head 移除 */
            numa_extent_t **pp = &bin->slabs_head;
            while (*pp) {
                if (*pp == ext) {
                    *pp = ext->next;
                    break;
                }
                pp = &(*pp)->next;
            }
            extent_free(ext);
        }
    }
    pthread_mutex_unlock(&bin->lock);
}

/* ── 公共分配接口 ──────────────────────────────────────────────── */

void *numa_arena_alloc(size_t size, int node, size_t *total_size,
                       int *from_pool_out) {
    if (!g_arena_ctx.initialized) return NULL;
    if (g_arena_ctx.num_nodes == 0) return NULL;
    if (node < 0 || node >= g_arena_ctx.num_nodes)
        node = 0;

    static int alloc_count = 0;
    if (++alloc_count <= 5) {
        ARENA_LOG("alloc #%d: size=%zu node=%d", alloc_count, size, node);
    }

    size_t alloc_size = size;
    int class_idx = size_to_class(size);

    if (total_size) *total_size = alloc_size;
    if (from_pool_out) *from_pool_out = 0;

    /* >4KB: 直接 extent */
    if (class_idx < 0) {
        void *ptr = numa_alloc_onnode(alloc_size, 0); /* no PREFIX for direct */
        return ptr;
    }

    /* tcache 快速路径 */
    tcache_t *tc = tcache_get();
    if (!tc) return NULL;
    tcache_bin_t *b = &tc->bins[node][class_idx];

    if (b->count > 0) {
        if (from_pool_out) *from_pool_out = 1;
        return b->stack[--b->count];
    }

    /* tcache 空 → 批量 refill */
    numa_bin_t *bin = &g_arena_ctx.arenas[node].bins[class_idx];
    int n = bin_refill(bin, node, class_idx, b, TCACHE_MAX_OBJECTS);
    if (n > 0) {
        if (from_pool_out) *from_pool_out = 1;
        return b->stack[--b->count];
    }

    return NULL;
}

/* ── 公共释放接口 ──────────────────────────────────────────────── */

void numa_arena_free(void *ptr, size_t total_size, int from_pool,
                     int node_id) {
    if (!ptr) return;

    /* from_pool=0: direct 分配，直接 numa_free */
    if (!from_pool) {
        numa_free(ptr, total_size);
        return;
    }

    if (!g_arena_ctx.initialized) return;
    if (node_id < 0 || node_id >= g_arena_ctx.num_nodes)
        node_id = 0;

    int class_idx = size_to_class(total_size);
    if (class_idx < 0) {
        numa_free(ptr, total_size);
        return;
    }

    /* tcache 快速路径 */
    tcache_t *tc = g_tcache;
    if (!tc) {
        /* 无 tcache：直接归还 bin */
        numa_bin_t *bin = &g_arena_ctx.arenas[node_id].bins[class_idx];
        bin_flush(bin, class_idx, &ptr, 1);
        return;
    }

    tcache_bin_t *b = &tc->bins[node_id][class_idx];
    if (b->count < TCACHE_MAX_OBJECTS) {
        b->stack[b->count++] = ptr;
        return;
    }

    /* tcache 满 → 批量 flush */
    numa_bin_t *bin = &g_arena_ctx.arenas[node_id].bins[class_idx];
    /* 取出前半段归还中央池 */
    void *flush_ptrs[TCACHE_FLUSH_BATCH];
    int flush_n = TCACHE_FLUSH_BATCH;
    memcpy(flush_ptrs, b->stack, flush_n * sizeof(void *));
    memmove(b->stack, b->stack + flush_n,
            (b->count - flush_n) * sizeof(void *));
    b->count -= flush_n;

    bin_flush(bin, class_idx, flush_ptrs, flush_n);

    /* 当前对象入栈 */
    b->stack[b->count++] = ptr;
}
