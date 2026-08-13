/* nf_alloc.c - independent NUMA-aware low-fragmentation allocator (pure C11). */
#include "nf_alloc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#define NF_SEGMENT_SIZE    (64 * 1024)
#define NF_LARGE_THRESHOLD 16384
#define NF_TCACHE_REFILL   32
#define NF_SEG_MAGIC       0x53454731u
#define NF_LARGE_MAGIC     0x4c415247u

/* 40 size classes: 8..128 (8-granular), then geometric up to 16KB. */
static const size_t g_class_size[] = {
    8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128,
    160, 192, 224, 256, 320, 384, 448, 512, 640, 768, 896, 1024,
    1280, 1536, 1792, 2048, 2560, 3072, 3584, 4096,
    5120, 6144, 7168, 8192, 10240, 12288, 14336, 16384
};
#define NF_NUM_CLASSES ((int)(sizeof(g_class_size) / sizeof(g_class_size[0])))

typedef struct nf_segment {
    uint32_t magic;
    uint32_t class_idx;
    uint32_t slot_size;
    uint32_t slot_count;
    uint16_t node;
    uint32_t free_count;
    void    *free_list;
} nf_segment_t;

typedef struct nf_large_hdr {
    uint32_t magic;
    uint32_t size;
    uint16_t node;
    uint16_t pad;
} nf_large_hdr_t;

typedef struct { void *head[NF_NUM_CLASSES]; uint32_t count[NF_NUM_CLASSES]; struct nf_alloc *owner; } nf_tcache_t;

struct nf_alloc {
    nf_alloc_backend_t backend;
    pthread_mutex_t     lock;
    nf_segment_t      **metamap;
    size_t              metamap_count, metamap_cap;
    void               *free_lists[NF_NUM_CLASSES];
    nf_alloc_stats_t    stats;
    int                 rr;              /* default-node round robin */
};

int nf_alloc_num_classes(void) { return NF_NUM_CLASSES; }
size_t nf_alloc_class_size(int idx) { return (idx >= 0 && idx < NF_NUM_CLASSES) ? g_class_size[idx] : 0; }

static size_t align_up(size_t x, size_t a) { return (x + a - 1) & ~(a - 1); }
static int size_to_class(size_t size) {
    for (int i = 0; i < NF_NUM_CLASSES; i++) if (size <= g_class_size[i]) return i;
    return -1; /* large */
}

/* ---- free-list helpers (must hold lock for shared lists) ---------------- */
static void *fl_pop(void **head) { void *p = *head; if (p) *head = *(void **)p; return p; }
static void fl_push(void **head, void *p) { *(void **)p = *head; *head = p; }

/* ---- metamap: address -> segment ---------------------------------------- */
static nf_segment_t *metamap_find(nf_alloc_t *a, const void *ptr) {
    size_t lo = 0, hi = a->metamap_count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        nf_segment_t *s = a->metamap[mid];
        if ((const char *)ptr < (const char *)s) hi = mid;
        else if ((const char *)ptr >= (const char *)s + NF_SEGMENT_SIZE) lo = mid + 1;
        else return s;
    }
    return NULL;
}
static void metamap_insert(nf_alloc_t *a, nf_segment_t *s) {
    if (a->metamap_count == a->metamap_cap) {
        size_t ncap = a->metamap_cap ? a->metamap_cap * 2 : 256;
        nf_segment_t **t = (nf_segment_t **)realloc(a->metamap, ncap * sizeof(nf_segment_t *));
        if (!t) return;
        a->metamap = t; a->metamap_cap = ncap;
    }
    size_t i = a->metamap_count;
    while (i > 0 && a->metamap[i - 1] > s) { a->metamap[i] = a->metamap[i - 1]; i--; }
    a->metamap[i] = s;
    a->metamap_count++;
}

/* ---- segment creation (locked) ------------------------------------------ */
static nf_segment_t *segment_new_locked(nf_alloc_t *a, int class_idx, int node) {
    size_t slot = g_class_size[class_idx];
    size_t data_start = align_up(sizeof(nf_segment_t), 16);
    size_t slot_count = (NF_SEGMENT_SIZE - data_start) / slot;
    void *base = a->backend.chunk_alloc(a->backend.ud, NF_SEGMENT_SIZE, node);
    if (!base) return NULL;
    nf_segment_t *s = (nf_segment_t *)base;
    s->magic = NF_SEG_MAGIC; s->class_idx = (uint32_t)class_idx;
    s->slot_size = (uint32_t)slot; s->slot_count = (uint32_t)slot_count;
    s->node = (uint16_t)node; s->free_count = (uint32_t)slot_count; s->free_list = NULL;
    char *slotp = (char *)base + data_start;
    for (size_t i = 0; i < slot_count; i++) { fl_push(&s->free_list, slotp); slotp += slot; }
    metamap_insert(a, s);
    a->stats.segment_count++;
    a->stats.backend_bytes += NF_SEGMENT_SIZE;
    if (a->stats.backend_bytes > a->stats.peak_backend_bytes) a->stats.peak_backend_bytes = a->stats.backend_bytes;
    return s;
}

/* ---- default backend (2 logical nodes over malloc) ---------------------- */
static void *def_chunk_alloc(void *ud, size_t size, int node) { (void)ud; (void)node; return malloc(size); }
static void  def_chunk_free(void *ud, void *ptr, size_t size) { (void)ud; (void)size; free(ptr); }

nf_alloc_t *nf_alloc_create(const nf_alloc_backend_t *backend) {
    nf_alloc_t *a = (nf_alloc_t *)calloc(1, sizeof(nf_alloc_t));
    if (!a) return NULL;
    if (backend && backend->num_nodes > 0) a->backend = *backend;
    else { a->backend.chunk_alloc = def_chunk_alloc; a->backend.chunk_free = def_chunk_free; a->backend.num_nodes = 2; a->backend.ud = NULL; }
    pthread_mutex_init(&a->lock, NULL);
    a->stats.num_nodes = a->backend.num_nodes;
    return a;
}

void nf_alloc_destroy(nf_alloc_t *a) {
    if (!a) return;
    pthread_mutex_lock(&a->lock);
    for (size_t i = 0; i < a->metamap_count; i++)
        a->backend.chunk_free(a->backend.ud, a->metamap[i], NF_SEGMENT_SIZE);
    free(a->metamap);
    pthread_mutex_unlock(&a->lock);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

/* ---- thread-local cache -------------------------------------------------- */
static _Thread_local nf_tcache_t t_tcache;

/* Switching allocators: drop the thread cache.  The cached slots still belong
 * to the old allocator's segments, which that allocator frees on destroy, so
 * nothing leaks. */
static void tcache_discard(void) {
    for (int i = 0; i < NF_NUM_CLASSES; i++) { t_tcache.head[i] = NULL; t_tcache.count[i] = 0; }
}

void *nf_alloc_malloc_onnode(nf_alloc_t *a, size_t size, int node) {
    if (!a || size == 0) size = 8;
    if (node < 0 || node >= a->backend.num_nodes) node = 0;

    int cls = size_to_class(size);
    if (cls < 0) {
        /* large: header + page-aligned payload */
        size_t payload = align_up(size, 16);
        void *base = a->backend.chunk_alloc(a->backend.ud, sizeof(nf_large_hdr_t) + payload, node);
        if (!base) return NULL;
        nf_large_hdr_t *h = (nf_large_hdr_t *)base;
        h->magic = NF_LARGE_MAGIC; h->size = (uint32_t)payload; h->node = (uint16_t)node; h->pad = 0;
        void *p = (char *)base + sizeof(nf_large_hdr_t);
        pthread_mutex_lock(&a->lock);
        a->stats.alloc_count++; a->stats.requested_bytes += size; a->stats.usable_bytes += payload;
        a->stats.cur_usable += payload;
        a->stats.backend_bytes += sizeof(nf_large_hdr_t) + payload;
        if (a->stats.backend_bytes > a->stats.peak_backend_bytes) a->stats.peak_backend_bytes = a->stats.backend_bytes;
        a->stats.node_alloc_count[node]++;
        pthread_mutex_unlock(&a->lock);
        return p;
    }

    /* tcache owner check (single-allocator fast path) */
    if (t_tcache.owner && t_tcache.owner != a) tcache_discard();
    t_tcache.owner = a;

    /* fast path: pop from thread cache (no lock; stats via atomic builtins) */
    if (t_tcache.head[cls]) {
        void *p = fl_pop(&t_tcache.head[cls]);
        t_tcache.count[cls]--;
        __atomic_add_fetch(&a->stats.alloc_count, 1, __ATOMIC_RELAXED);
        __atomic_add_fetch(&a->stats.requested_bytes, size, __ATOMIC_RELAXED);
        __atomic_add_fetch(&a->stats.usable_bytes, g_class_size[cls], __ATOMIC_RELAXED);
        __atomic_add_fetch(&a->stats.cur_usable, g_class_size[cls], __ATOMIC_RELAXED);
        __atomic_add_fetch(&a->stats.node_alloc_count[node], 1, __ATOMIC_RELAXED);
        return p;
    }

    /* refill: batch from the shared list (or a new segment) */
    pthread_mutex_lock(&a->lock);
    if (!a->free_lists[cls]) {
        nf_segment_t *s = segment_new_locked(a, cls, node);
        if (!s) { pthread_mutex_unlock(&a->lock); return NULL; }
        /* move the segment's slots into the shared list */
        while (s->free_list) { void *p = fl_pop(&s->free_list); fl_push(&a->free_lists[cls], p); }
        s->free_count = 0;
    }
    void *result = NULL;
    for (int i = 0; i < NF_TCACHE_REFILL && a->free_lists[cls]; i++) {
        void *p = fl_pop(&a->free_lists[cls]);
        fl_push(&t_tcache.head[cls], p); t_tcache.count[cls]++;
    }
    result = fl_pop(&t_tcache.head[cls]); t_tcache.count[cls]--;
    a->stats.alloc_count++; a->stats.requested_bytes += size; a->stats.usable_bytes += g_class_size[cls];
    a->stats.cur_usable += g_class_size[cls];
    a->stats.node_alloc_count[node]++;
    pthread_mutex_unlock(&a->lock);
    return result;
}

void *nf_alloc_malloc(nf_alloc_t *a, size_t size) {
    if (!a) return NULL;
    int node = a->rr % a->backend.num_nodes; a->rr++;
    return nf_alloc_malloc_onnode(a, size, node);
}

void *nf_alloc_calloc(nf_alloc_t *a, size_t n, size_t size) {
    size_t total = n * size;
    void *p = nf_alloc_malloc(a, total);
    if (p) memset(p, 0, total);
    return p;
}

void nf_alloc_free(nf_alloc_t *a, void *ptr) {
    if (!a || !ptr) return;
    pthread_mutex_lock(&a->lock);
    nf_segment_t *s = metamap_find(a, ptr);
    if (s) {
        int cls = (int)s->class_idx;
        fl_push(&a->free_lists[cls], ptr);
        s->free_count++;
        a->stats.free_count++;
        a->stats.cur_usable -= s->slot_size;
    } else {
        /* large allocation (header just before ptr) */
        nf_large_hdr_t *h = (nf_large_hdr_t *)((char *)ptr - sizeof(nf_large_hdr_t));
        if (h->magic == NF_LARGE_MAGIC) {
            a->stats.free_count++;
            a->stats.cur_usable -= h->size;
            a->stats.backend_bytes -= sizeof(nf_large_hdr_t) + h->size;
            a->backend.chunk_free(a->backend.ud, h, sizeof(nf_large_hdr_t) + h->size);
        }
    }
    pthread_mutex_unlock(&a->lock);
}

size_t nf_alloc_usable_size(nf_alloc_t *a, const void *ptr) {
    if (!a || !ptr) return 0;
    pthread_mutex_lock(&a->lock);
    size_t out = 0;
    nf_segment_t *s = metamap_find(a, ptr);
    if (s) out = s->slot_size;
    else {
        nf_large_hdr_t *h = (nf_large_hdr_t *)((const char *)ptr - sizeof(nf_large_hdr_t));
        if (h->magic == NF_LARGE_MAGIC) out = h->size;
    }
    pthread_mutex_unlock(&a->lock);
    return out;
}

void *nf_alloc_realloc(nf_alloc_t *a, void *ptr, size_t size) {
    if (!ptr) return nf_alloc_malloc(a, size);
    if (size == 0) { nf_alloc_free(a, ptr); return NULL; }
    size_t old = nf_alloc_usable_size(a, ptr);
    if (size <= old && size_to_class(size) == size_to_class(old)) return ptr;
    void *np = nf_alloc_malloc(a, size);
    if (!np) return NULL;
    size_t copy = size < old ? size : old;
    if (copy) memcpy(np, ptr, copy);
    nf_alloc_free(a, ptr);
    return np;
}

void nf_alloc_get_stats(nf_alloc_t *a, nf_alloc_stats_t *out) {
    if (!a || !out) return;
    pthread_mutex_lock(&a->lock);
    *out = a->stats;
    pthread_mutex_unlock(&a->lock);
}
double nf_alloc_internal_frag(nf_alloc_t *a) {
    if (!a) return 0.0;
    pthread_mutex_lock(&a->lock);
    double r = a->stats.requested_bytes ? (double)(a->stats.usable_bytes - a->stats.requested_bytes) / (double)a->stats.requested_bytes : 0.0;
    pthread_mutex_unlock(&a->lock);
    return r;
}
double nf_alloc_external_overhead(nf_alloc_t *a) {
    if (!a) return 0.0;
    pthread_mutex_lock(&a->lock);
    double r = a->stats.cur_usable ? (double)a->stats.backend_bytes / (double)a->stats.cur_usable - 1.0 : 0.0;
    pthread_mutex_unlock(&a->lock);
    return r;
}
