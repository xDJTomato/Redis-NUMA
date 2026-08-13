/* =============================================================================
 * nf_ops.h - atomic operation registry and execution context (pure C11).
 *
 * An atomic operation is a single composable step over a batch of cache items.
 * Strategies are decomposed into these operations and re-assembled as DAGs.
 * ========================================================================== */
#ifndef NF_OPS_H
#define NF_OPS_H

#include "nf_common.h"
#include "numa_shim.h"

#ifdef __cplusplus
extern "C" {
#endif

struct nf_tracker; /* defined in nf_track.h */

/* ---- execution context shared by every op in a run ---------------------- */
typedef struct {
    nf_node_t        *topo;      /* NUMA topology (owned by caller) */
    int               topo_count;
    const nf_params_t *params;    /* current node params (set per-node) */
    nf_stats_t        stats;     /* accumulated statistics */
    numaflow_env_t   *env;       /* NUMA emulation env (cost model) */
    struct nf_tracker *tracker;  /* optional tracking context */
    uint64_t          tick;      /* current logical tick (recency) */
    nf_rng_t          rng;       /* deterministic rng */
    int               budget;    /* migration budget for this run */
} nf_ctx_t;

/* ---- parameter spec (drives the GUI/TUI input forms) -------------------- */
typedef enum {
    NF_PARAM_INT = 0,
    NF_PARAM_DOUBLE,
    NF_PARAM_STRING,
    NF_PARAM_BOOL
} nf_param_type_t;

typedef struct {
    const char       *name;
    nf_param_type_t   type;
    const char       *default_value;
    const char       *description;
} nf_param_spec_t;

/* ---- the atomic operation ----------------------------------------------- */
typedef struct nf_op nf_op_t;
typedef int (*nf_op_run_fn)(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out);

struct nf_op {
    const char        *name;
    const char        *title;
    const char        *description;
    const char        *category;    /* alloc|score|filter|rank|decide|emit|track */
    nf_op_run_fn       run;
    const nf_param_spec_t *params;
    int                param_count;
};

/* ---- registry ----------------------------------------------------------- */
int  nf_ops_register(const nf_op_t *op);
const nf_op_t *nf_ops_find(const char *name);
int  nf_ops_count(void);
const nf_op_t *nf_ops_get(int index);   /* index [0, count) */
void nf_ops_register_all(void);         /* register every built-in op */
void nf_ops_clear(void);                /* clear registry (testing) */

/* Helper: clamp node id into [0, topo_count-1] (or 0). */
int  nf_ctx_node_index(const nf_ctx_t *ctx, int node);

#ifdef __cplusplus
}
#endif

#endif /* NF_OPS_H */
