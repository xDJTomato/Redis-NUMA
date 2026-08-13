/* =============================================================================
 * nf_strategy.h - named strategy catalog: existing strategies decomposed into
 * DAG workflows, plus the new default cost-aware adaptive tiering strategy.
 * ========================================================================== */
#ifndef NF_STRATEGY_H
#define NF_STRATEGY_H

#include "nf_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Build a named built-in strategy into `g` (overwrites). Returns NF_OK or a
 * negative error.  Names:
 *   migration : caat (default), composite_lru, tinylfu, noop
 *   allocation: alloc_local_first, alloc_interleave, alloc_round_robin,
 *               alloc_weighted, alloc_pressure_aware, alloc_cxl_optimized,
 *               alloc_weighted_interleave, alloc_adaptive, alloc_latency_aware */
int  nf_strategy_build(nf_graph_t *g, const char *name);

/* Number of built-in strategy names; names returned by nf_strategy_name(i). */
int  nf_strategy_count(void);
const char *nf_strategy_name(int i);
/* Short description for a strategy name (or NULL). */
const char *nf_strategy_desc(const char *name);
int  nf_strategy_is_allocation(const char *name);

/* The default migration strategy name. */
const char *nf_strategy_default(void);

#ifdef __cplusplus
}
#endif

#endif /* NF_STRATEGY_H */
