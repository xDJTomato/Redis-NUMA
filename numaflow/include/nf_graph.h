/* =============================================================================
 * nf_graph.h - DAG graph model for NUMAflow workflows (pure C11).
 *
 * A workflow is a directed acyclic graph of nodes; every node references a
 * registered atomic operation by name and carries string key/value params.
 * Edges are anonymous (from -> to); the executor uses broadcast + fan-in
 * dataflow semantics (see nf_exec.h).
 * ========================================================================== */
#ifndef NF_GRAPH_H
#define NF_GRAPH_H

#include "nf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char        id[NF_ID_MAX];
    char        op[NF_OP_MAX];
    nf_params_t params;
} nf_gnode_t;

typedef struct {
    char from[NF_ID_MAX];
    char to[NF_ID_MAX];
} nf_edge_t;

typedef struct {
    char      name[NF_STR_MAX];
    char      description[NF_STR_MAX];
    nf_gnode_t *nodes;
    int        node_count, node_cap;
    nf_edge_t *edges;
    int        edge_count, edge_cap;
} nf_graph_t;

void nf_graph_init(nf_graph_t *g);
void nf_graph_free(nf_graph_t *g);

int  nf_graph_add_node(nf_graph_t *g, const char *id, const char *op);
int  nf_graph_node_set_param(nf_graph_t *g, const char *id, const char *key, const char *value);
int  nf_graph_add_edge(nf_graph_t *g, const char *from, const char *to);
nf_gnode_t *nf_graph_find_node(const nf_graph_t *g, const char *id);
int  nf_graph_node_index(const nf_graph_t *g, const char *id);

/* Serialize to a malloc'd JSON string (caller frees). */
char *nf_graph_to_json(const nf_graph_t *g);
/* Load from JSON text; returns NF_OK or a negative error. */
int   nf_graph_from_json(nf_graph_t *g, const char *json, char *errbuf, size_t errlen);
/* Load from a file. */
int   nf_graph_load_file(nf_graph_t *g, const char *path, char *errbuf, size_t errlen);

/* Topological sort (Kahn).  out_order must hold node_count entries.
 * Returns NF_OK, NF_ECYCLE, or another error. */
int   nf_graph_topo_sort(const nf_graph_t *g, int *out_order);

#ifdef __cplusplus
}
#endif

#endif /* NF_GRAPH_H */
