/* =============================================================================
 * nf_bridge.h - bridge between the NUMAflow DAG engine and a key-value store.
 *
 * The engine is pure C11 and store-agnostic.  A host (Redis) implements two
 * callbacks:
 *   enumerate()  - yield cache items one by one (key, size, current node,
 *                  access count, hotness, recency);
 *   apply()      - physically migrate one key to a target node.
 *
 * nf_bridge_run() then: enumerates -> builds the item batch -> runs the DAG ->
 * collects the migration decisions -> calls apply() for each -> folds the
 * run statistics into the tracker feedback loop.  This is the exact contract
 * the Redis adapter (src/numa_flow.c) implements.
 * ========================================================================== */
#ifndef NF_BRIDGE_H
#define NF_BRIDGE_H

#include "nf_ops.h"
#include "nf_exec.h"
#include "nf_track.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Enumeration callback: fill *out with one item; return 0 to continue,
 * 1 to stop iteration (end of keys), negative on error. */
typedef int (*nf_enum_fn)(void *ud, nf_item_t *out);

/* Apply callback: migrate `key` to `target_node`. Return 0 on success. */
typedef int (*nf_apply_fn)(void *ud, const char *key, int target_node);

/* Result of a bridge run. */
typedef struct {
    int      enumerated;   /* number of items seen */
    int      migrations;   /* migration decisions emitted */
    int      applied;      /* migrations successfully applied */
    int      failed;       /* apply() failures */
    double   feedback;     /* post-run tracker feedback score */
} nf_bridge_result_t;

typedef struct {
    nf_enum_fn   enumerate;
    nf_apply_fn  apply;
    void        *ud;
    /* shared topology / stats / env (nf_bridge_run fills ctx from these) */
    nf_ctx_t     ctx;
    nf_tracker_t *tracker;   /* optional: enable the feedback loop */
} nf_bridge_t;

/* Run workflow `g` over the items enumerated by `br`.  Migrations decided by
 * the DAG (emit_migrate / select_dest_node) are handed to br->apply(). */
int nf_bridge_run(nf_bridge_t *br, const nf_graph_t *g, nf_bridge_result_t *out);

/* Convenience: run a built-in strategy by name. */
int nf_bridge_run_named(nf_bridge_t *br, const char *strategy, nf_bridge_result_t *out);

#ifdef __cplusplus
}
#endif

#endif /* NF_BRIDGE_H */
