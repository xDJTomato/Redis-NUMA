/* =============================================================================
 * nf_common.h - NUMAflow core common types, constants and small utilities.
 *
 * NUMAflow is the N8N-style workflow engine for Redis-NUMA memory scheduling.
 * It decomposes NUMA memory-scheduling strategies into composable "atomic
 * operations" that can be arranged into a directed acyclic graph (DAG) and
 * executed as a dataflow pipeline.
 *
 * This header is pure C11 and has NO Redis / libnuma / POSIX dependency so it
 * can be built and unit-tested on any platform (including native Windows with
 * MinGW) and linked into Redis on Linux.
 * ========================================================================== */
#ifndef NF_COMMON_H
#define NF_COMMON_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- sizing constants --------------------------------------------------- */
#define NF_KEY_MAX      128   /* max cache key length (bytes) */
#define NF_ID_MAX       64    /* node / op id length */
#define NF_OP_MAX       64    /* op name length */
#define NF_PARAM_MAX    64    /* param key length */
#define NF_VALUE_MAX    256   /* param value length */
#define NF_STR_MAX      512   /* generic string buffer */
#define NF_MAX_NODES    64    /* max emulated NUMA nodes */
#define NF_MAX_ITEMS    1048576 /* hard cap on flow items (safety) */

/* ---- return codes -------------------------------------------------------- */
#define NF_OK            0
#define NF_ERR          -1
#define NF_ENOENT       -2
#define NF_EINVAL       -3
#define NF_ENOMEM       -4
#define NF_EEXIST       -5
#define NF_ECYCLE       -6    /* graph has a cycle */
#define NF_EEMPTY       -7

/* ---- logging levels (mirror Redis verbosity loosely) --------------------- */
#define NF_LOG_DEBUG    0
#define NF_LOG_VERBOSE  1
#define NF_LOG_NOTICE   2
#define NF_LOG_WARNING  3

/* ===========================================================================
 * Deterministic PRNG (xorshift64*).  Used everywhere so evaluation runs are
 * bit-for-bit reproducible given the same seed -> fairness across strategies.
 * ========================================================================== */
typedef struct {
    uint64_t s;
} nf_rng_t;

static inline uint64_t nf_rng_next(nf_rng_t *r) {
    uint64_t x = r->s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    r->s = x;
    return x * UINT64_C(2685821657736338717);
}

static inline nf_rng_t nf_rng_seed(uint64_t seed) {
    nf_rng_t r;
    r.s = seed ? seed : UINT64_C(0x9e3779b97f4a7c15);
    return r;
}

/* Uniform double in [0,1). */
static inline double nf_rng_double(nf_rng_t *r) {
    return (double)(nf_rng_next(r) >> 11) * (1.0 / 9007199254740992.0); /* 2^53 */
}

/* Uniform integer in [0, n). */
static inline int nf_rng_int(nf_rng_t *r, int n) {
    if (n <= 1) return 0;
    return (int)(nf_rng_next(r) % (uint64_t)n);
}

/* ===========================================================================
 * Key/value parameter (used for op params and shared params).
 * ========================================================================== */
typedef struct {
    char key[NF_PARAM_MAX];
    char value[NF_VALUE_MAX];
} nf_kv_t;

typedef struct {
    nf_kv_t *items;
    int count;
    int cap;
} nf_params_t;

void     nf_params_init(nf_params_t *p);
void     nf_params_free(nf_params_t *p);
int      nf_params_set(nf_params_t *p, const char *key, const char *value);
const char *nf_params_get(const nf_params_t *p, const char *key);
int      nf_params_get_int(const nf_params_t *p, const char *key, int dflt);
double   nf_params_get_double(const nf_params_t *p, const char *key, double dflt);
/* Append a "key=value" text param (parses on the '=' sign). */
int      nf_params_parse_assign(nf_params_t *p, const char *assign);

/* ===========================================================================
 * NUMA node topology (emulated; see numa_shim.h).
 * ========================================================================== */
typedef struct {
    int      node_id;          /* 0-based node index */
    char     name[NF_ID_MAX];  /* human name, e.g. "dram0", "cxl1" */
    uint64_t total_bytes;      /* total capacity (bytes) */
    uint64_t used_bytes;       /* currently allocated bytes */
    double   latency_ns;       /* local access latency (ns) */
    double   bandwidth_mbps;   /* available bandwidth (MB/s) */
    double   pressure;         /* 0..1 current pressure */
    double   weight;           /* allocation weight (relative) */
    double   distance;         /* modeled NUMA distance to node 0 */
} nf_node_t;

/* ===========================================================================
 * A single cache item (key record) flowing through the pipeline.
 * ========================================================================== */
typedef struct {
    char     key[NF_KEY_MAX];  /* key name */
    void    *value;            /* opaque value pointer (NULL for synthetic eval) */
    size_t   value_size;       /* bytes */
    int      current_node;     /* node currently holding the value (-1 = none) */
    uint64_t access_count;     /* lifetime access count */
    uint64_t recency;          /* last-access tick (larger = more recent) */
    uint8_t  hotness;          /* 0..7 staircase hotness */
    uint32_t freq_est;         /* frequency estimate (Count-Min Sketch) */
    double   ewma;             /* EWMA blended score */
    double   cost_benefit;     /* net benefit of migrating this item */
    int      selected_node;    /* decision: target node */
    int      migrate;          /* decision: 1 = emit migration */
    int      keep;             /* filter: 1 = keep in flow, 0 = drop */
    int      touched;          /* scratch flag (doorkeeper / visited) */
} nf_item_t;

/* Dynamic array of items. */
typedef struct {
    nf_item_t *items;
    size_t count;
    size_t cap;
} nf_items_t;

void nf_items_init(nf_items_t *a);
void nf_items_free(nf_items_t *a);
int  nf_items_reserve(nf_items_t *a, size_t n);
int  nf_items_push(nf_items_t *a, const nf_item_t *it);
int  nf_items_copy(nf_items_t *dst, const nf_items_t *src);
int  nf_items_append(nf_items_t *dst, const nf_items_t *src);   /* concat */
void nf_items_clear(nf_items_t *a);
/* Keep only items whose ->keep flag is non-zero (in place). */
void nf_items_filter_keep(nf_items_t *a);
/* Stable sort by a comparator. */
typedef int (*nf_item_cmp_fn)(const nf_item_t *a, const nf_item_t *b);
void nf_items_sort(nf_items_t *a, nf_item_cmp_fn cmp);

/* ===========================================================================
 * Shared statistics (accumulated by ops during an execution run).
 * ========================================================================== */
typedef struct {
    uint64_t accesses;          /* total accesses modeled */
    uint64_t local_hits;        /* accesses served from the local node */
    uint64_t remote_hits;       /* accesses served from a remote node */
    uint64_t migrations_done;   /* emitted + applied migrations */
    uint64_t migrations_skipped;/* migrations dropped by budget/filter */
    uint64_t node_accesses[NF_MAX_NODES]; /* accesses per node */
    double   total_cost_ns;     /* accumulated modeled access cost (ns) */
} nf_stats_t;

/* ===========================================================================
 * Logging hook (assignable; defaults to fprintf(stderr)).
 * ========================================================================== */
typedef void (*nf_log_fn)(int level, const char *fmt, ...);
extern nf_log_fn nf_logger;
void nf_log(int level, const char *fmt, ...);

/* Minimal platform-independent monotonic tick (ms) for recency/scheduling. */
uint64_t nf_tick_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* NF_COMMON_H */
