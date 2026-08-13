/* nf_graph.c - DAG graph model implementation (pure C11). */
#include "nf_graph.h"
#include "nf_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void nf_graph_init(nf_graph_t *g) {
    memset(g, 0, sizeof(*g));
}

void nf_graph_free(nf_graph_t *g) {
    for (int i = 0; i < g->node_count; i++) nf_params_free(&g->nodes[i].params);
    free(g->nodes);
    free(g->edges);
    nf_graph_init(g);
}

int nf_graph_add_node(nf_graph_t *g, const char *id, const char *op) {
    if (!g || !id || !op) return NF_EINVAL;
    if (nf_graph_find_node(g, id)) return NF_EEXIST;
    if (g->node_count == g->node_cap) {
        int ncap = g->node_cap ? g->node_cap * 2 : 8;
        nf_gnode_t *tmp = (nf_gnode_t *)realloc(g->nodes, (size_t)ncap * sizeof(nf_gnode_t));
        if (!tmp) return NF_ENOMEM;
        g->nodes = tmp;
        g->node_cap = ncap;
    }
    nf_gnode_t *n = &g->nodes[g->node_count];
    memset(n, 0, sizeof(*n));
    strncpy(n->id, id, NF_ID_MAX - 1);
    strncpy(n->op, op, NF_OP_MAX - 1);
    nf_params_init(&n->params);
    g->node_count++;
    return NF_OK;
}

int nf_graph_node_set_param(nf_graph_t *g, const char *id, const char *key, const char *value) {
    nf_gnode_t *n = nf_graph_find_node(g, id);
    if (!n) return NF_ENOENT;
    return nf_params_set(&n->params, key, value);
}

int nf_graph_add_edge(nf_graph_t *g, const char *from, const char *to) {
    if (!g || !from || !to) return NF_EINVAL;
    if (!nf_graph_find_node(g, from) || !nf_graph_find_node(g, to)) return NF_ENOENT;
    if (g->edge_count == g->edge_cap) {
        int ncap = g->edge_cap ? g->edge_cap * 2 : 8;
        nf_edge_t *tmp = (nf_edge_t *)realloc(g->edges, (size_t)ncap * sizeof(nf_edge_t));
        if (!tmp) return NF_ENOMEM;
        g->edges = tmp;
        g->edge_cap = ncap;
    }
    nf_edge_t *e = &g->edges[g->edge_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->from, from, NF_ID_MAX - 1);
    strncpy(e->to, to, NF_ID_MAX - 1);
    g->edge_count++;
    return NF_OK;
}

nf_gnode_t *nf_graph_find_node(const nf_graph_t *g, const char *id) {
    if (!g || !id) return NULL;
    for (int i = 0; i < g->node_count; i++) {
        if (strcmp(g->nodes[i].id, id) == 0) return &g->nodes[i];
    }
    return NULL;
}

int nf_graph_node_index(const nf_graph_t *g, const char *id) {
    if (!g || !id) return -1;
    for (int i = 0; i < g->node_count; i++) {
        if (strcmp(g->nodes[i].id, id) == 0) return i;
    }
    return -1;
}

char *nf_graph_to_json(const nf_graph_t *g) {
    nf_json_t *root = nf_json_new_obj();
    nf_json_obj_set(root, "name", nf_json_new_str(g->name[0] ? g->name : "workflow"));
    nf_json_obj_set(root, "description", nf_json_new_str(g->description[0] ? g->description : ""));
    nf_json_t *nodes = nf_json_new_arr();
    for (int i = 0; i < g->node_count; i++) {
        nf_gnode_t *n = &g->nodes[i];
        nf_json_t *jn = nf_json_new_obj();
        nf_json_obj_set(jn, "id", nf_json_new_str(n->id));
        nf_json_obj_set(jn, "op", nf_json_new_str(n->op));
        if (n->params.count > 0) {
            nf_json_t *jp = nf_json_new_obj();
            for (int k = 0; k < n->params.count; k++)
                nf_json_obj_set(jp, n->params.items[k].key, nf_json_new_str(n->params.items[k].value));
            nf_json_obj_set(jn, "params", jp);
        }
        nf_json_arr_push(nodes, jn);
    }
    nf_json_obj_set(root, "nodes", nodes);
    nf_json_t *edges = nf_json_new_arr();
    for (int i = 0; i < g->edge_count; i++) {
        nf_json_t *je = nf_json_new_obj();
        nf_json_obj_set(je, "from", nf_json_new_str(g->edges[i].from));
        nf_json_obj_set(je, "to", nf_json_new_str(g->edges[i].to));
        nf_json_arr_push(edges, je);
    }
    nf_json_obj_set(root, "edges", edges);
    char *out = nf_json_serialize(root);
    nf_json_free(root);
    return out;
}

int nf_graph_from_json(nf_graph_t *g, const char *json, char *errbuf, size_t errlen) {
    const char *err = NULL;
    nf_json_t *root = nf_json_parse(json, &err);
    if (!root) {
        if (errbuf) snprintf(errbuf, errlen, "json: %s", err ? err : "parse error");
        return NF_EINVAL;
    }
    const char *name = nf_json_obj_get_str(root, "name");
    if (name) strncpy(g->name, name, NF_STR_MAX - 1);
    const char *desc = nf_json_obj_get_str(root, "description");
    if (desc) strncpy(g->description, desc, NF_STR_MAX - 1);

    nf_json_t *nodes = nf_json_obj_get(root, "nodes");
    nf_json_t *edges = nf_json_obj_get(root, "edges");
    if (!nodes || nodes->type != NF_JSON_ARR) {
        nf_json_free(root);
        if (errbuf) snprintf(errbuf, errlen, "missing nodes array");
        return NF_EINVAL;
    }
    for (size_t i = 0; i < nf_json_arr_len(nodes); i++) {
        nf_json_t *jn = nf_json_arr_get(nodes, i);
        const char *id = nf_json_obj_get_str(jn, "id");
        const char *op = nf_json_obj_get_str(jn, "op");
        if (!id || !op) continue;
        if (nf_graph_add_node(g, id, op) != NF_OK) {
            nf_json_free(root);
            if (errbuf) snprintf(errbuf, errlen, "duplicate node id: %s", id);
            return NF_EEXIST;
        }
        nf_json_t *params = nf_json_obj_get(jn, "params");
        if (params && params->type == NF_JSON_OBJ) {
            for (size_t k = 0; k < params->child_count; k++) {
                const char *val = nf_json_str(params->children[k]);
                if (val) nf_graph_node_set_param(g, id, params->keys[k], val);
            }
        }
    }
    if (edges && edges->type == NF_JSON_ARR) {
        for (size_t i = 0; i < nf_json_arr_len(edges); i++) {
            nf_json_t *je = nf_json_arr_get(edges, i);
            const char *from = nf_json_obj_get_str(je, "from");
            const char *to = nf_json_obj_get_str(je, "to");
            if (from && to) nf_graph_add_edge(g, from, to);
        }
    }
    nf_json_free(root);
    return NF_OK;
}

int nf_graph_load_file(nf_graph_t *g, const char *path, char *errbuf, size_t errlen) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (errbuf) snprintf(errbuf, errlen, "cannot open %s", path);
        return NF_ENOENT;
    }
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NF_ERR; }
    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NF_ENOMEM; }
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    buf[rd] = '\0';
    fclose(fp);
    int rc = nf_graph_from_json(g, buf, errbuf, errlen);
    free(buf);
    return rc;
}

int nf_graph_topo_sort(const nf_graph_t *g, int *out_order) {
    if (!g || !out_order) return NF_EINVAL;
    int n = g->node_count;
    int *indeg = (int *)calloc((size_t)n, sizeof(int));
    if (n && !indeg) return NF_ENOMEM;
    for (int i = 0; i < g->edge_count; i++) {
        int t = nf_graph_node_index(g, g->edges[i].to);
        if (t >= 0) indeg[t]++;
    }
    int *queue = (int *)malloc((size_t)n * sizeof(int));
    if (n && !queue) { free(indeg); return NF_ENOMEM; }
    int head = 0, tail = 0;
    for (int i = 0; i < n; i++) if (indeg[i] == 0) queue[tail++] = i;
    int out = 0;
    while (head < tail) {
        int u = queue[head++];
        out_order[out++] = u;
        for (int i = 0; i < g->edge_count; i++) {
            int f = nf_graph_node_index(g, g->edges[i].from);
            if (f != u) continue;
            int t = nf_graph_node_index(g, g->edges[i].to);
            if (t >= 0 && --indeg[t] == 0) queue[tail++] = t;
        }
    }
    free(queue);
    free(indeg);
    return (out == n) ? NF_OK : NF_ECYCLE;
}
