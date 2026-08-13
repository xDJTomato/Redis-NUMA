/* =============================================================================
 * nf_exec.h - DAG dataflow executor (pure C11).
 *
 * Executes a workflow graph topologically with broadcast + fan-in dataflow
 * semantics: a source node receives the initial items, each node runs its op
 * on the union of its incoming edges' payloads, and its output is copied to
 * every outgoing edge.  The final result is the union of all sink outputs.
 * ========================================================================== */
#ifndef NF_EXEC_H
#define NF_EXEC_H

#include "nf_graph.h"
#include "nf_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    nf_graph_t  graph;
    nf_items_t *node_outputs;
    nf_items_t *edge_payloads;
    int        *order;
    int         order_count;
    nf_items_t  result;
    char        error[NF_STR_MAX];
} nf_exec_t;

void nf_exec_init(nf_exec_t *ex);
void nf_exec_free(nf_exec_t *ex);

/* Run graph `g` over `initial` items with context `ctx` (which must carry
 * topology/env/tracker).  On success returns NF_OK and fills ex->result;
 * otherwise returns a negative error and sets ex->error. */
int  nf_exec_run(nf_exec_t *ex, const nf_graph_t *g, const nf_items_t *initial, nf_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* NF_EXEC_H */
