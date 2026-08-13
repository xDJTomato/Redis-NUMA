/* nf_exec.c - DAG dataflow executor (pure C11). */
#include "nf_exec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nf_exec_init(nf_exec_t *ex) {
    memset(ex, 0, sizeof(*ex));
    nf_graph_init(&ex->graph);
    nf_items_init(&ex->result);
}

void nf_exec_free(nf_exec_t *ex) {
    if (ex->node_outputs) {
        for (int i = 0; i < ex->graph.node_count; i++) nf_items_free(&ex->node_outputs[i]);
        free(ex->node_outputs); ex->node_outputs = NULL;
    }
    if (ex->edge_payloads) {
        for (int i = 0; i < ex->graph.edge_count; i++) nf_items_free(&ex->edge_payloads[i]);
        free(ex->edge_payloads); ex->edge_payloads = NULL;
    }
    free(ex->order); ex->order = NULL; ex->order_count = 0;
    nf_graph_free(&ex->graph);
    nf_items_free(&ex->result);
}

int nf_exec_run(nf_exec_t *ex, const nf_graph_t *g, const nf_items_t *initial, nf_ctx_t *ctx) {
    if (!ex || !g || !ctx) return NF_EINVAL;
    ex->error[0] = '\0';

    int n = g->node_count;
    if (n == 0) {
        return nf_items_copy(&ex->result, initial);
    }

    ex->order = (int *)malloc((size_t)n * sizeof(int));
    if (!ex->order) return NF_ENOMEM;
    int rc = nf_graph_topo_sort(g, ex->order);
    if (rc != NF_OK) {
        snprintf(ex->error, sizeof(ex->error), rc == NF_ECYCLE ? "workflow graph contains a cycle" : "topological sort failed");
        return rc;
    }
    ex->order_count = n;

    ex->node_outputs = (nf_items_t *)calloc((size_t)n, sizeof(nf_items_t));
    ex->edge_payloads = (nf_items_t *)calloc((size_t)g->edge_count, sizeof(nf_items_t));
    if ((n && !ex->node_outputs) || (g->edge_count && !ex->edge_payloads)) return NF_ENOMEM;
    for (int i = 0; i < n; i++) nf_items_init(&ex->node_outputs[i]);
    for (int i = 0; i < g->edge_count; i++) nf_items_init(&ex->edge_payloads[i]);

    nf_items_t inbuf, outbuf;
    nf_items_init(&inbuf); nf_items_init(&outbuf);

    for (int oi = 0; oi < n; oi++) {
        int idx = ex->order[oi];
        const nf_gnode_t *node = &g->nodes[idx];
        nf_items_clear(&inbuf);
        nf_items_clear(&outbuf);

        int has_incoming = 0;
        for (int e = 0; e < g->edge_count; e++) {
            if (strcmp(g->edges[e].to, node->id) == 0) {
                has_incoming = 1;
                nf_items_append(&inbuf, &ex->edge_payloads[e]);
            }
        }
        if (!has_incoming) nf_items_copy(&inbuf, initial);

        const nf_op_t *op = nf_ops_find(node->op);
        if (!op) {
            snprintf(ex->error, sizeof(ex->error), "unknown op '%s' on node '%s'", node->op, node->id);
            nf_items_free(&inbuf); nf_items_free(&outbuf);
            return NF_ENOENT;
        }

        const nf_params_t *saved = ctx->params;
        ctx->params = &node->params;
        int op_rc = op->run(op, ctx, &inbuf, &outbuf);
        ctx->params = saved;
        if (op_rc != NF_OK) {
            snprintf(ex->error, sizeof(ex->error), "op '%s' failed (%d)", node->op, op_rc);
            nf_items_free(&inbuf); nf_items_free(&outbuf);
            return op_rc;
        }

        nf_items_copy(&ex->node_outputs[idx], &outbuf);
        for (int e = 0; e < g->edge_count; e++) {
            if (strcmp(g->edges[e].from, node->id) == 0)
                nf_items_copy(&ex->edge_payloads[e], &outbuf);
        }
    }

    nf_items_free(&inbuf); nf_items_free(&outbuf);

    /* union of sink-node outputs */
    nf_items_clear(&ex->result);
    for (int i = 0; i < n; i++) {
        int has_outgoing = 0;
        for (int e = 0; e < g->edge_count; e++)
            if (strcmp(g->edges[e].from, g->nodes[i].id) == 0) { has_outgoing = 1; break; }
        if (!has_outgoing) nf_items_append(&ex->result, &ex->node_outputs[i]);
    }
    return NF_OK;
}
