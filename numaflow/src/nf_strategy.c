/* nf_strategy.c - named strategy catalog (existing strategies decomposed). */
#include "nf_strategy.h"

#include <stdio.h>
#include <string.h>

typedef int (*builder_fn)(nf_graph_t *g);

typedef struct {
    const char *name;
    const char *desc;
    int         is_alloc;
    builder_fn  build;
} strategy_def_t;

/* Build a linear chain: {op,paramkey,paramval} x n.  Ids are "<prefix>1".."<prefix>N".
 * If connect_from is non-NULL, an edge is added from that existing node id to
 * the chain's first node (used to fan a chain off of another chain's output
 * without id collisions - see build_caat). */
typedef struct { const char *op; const char *key; const char *val; } step_t;
static int build_chain_from(nf_graph_t *g, const step_t *steps, int n, const char *prefix, const char *connect_from) {
    char id[16], prev[16];
    snprintf(prev, sizeof(prev), "%s", connect_from ? connect_from : "");
    for (int i = 0; i < n; i++) {
        snprintf(id, sizeof(id), "%s%d", prefix, i + 1);
        if (nf_graph_add_node(g, id, steps[i].op) != NF_OK) return NF_ERR;
        if (steps[i].key && steps[i].val) nf_graph_node_set_param(g, id, steps[i].key, steps[i].val);
        if (prev[0]) nf_graph_add_edge(g, prev, id);
        snprintf(prev, sizeof(prev), "%s", id);
    }
    return NF_OK;
}
static int build_chain(nf_graph_t *g, const step_t *steps, int n) {
    return build_chain_from(g, steps, n, "n", NULL);
}

/* ---- migration strategy builders ---------------------------------------- */
static int build_noop(nf_graph_t *g) { (void)g; return NF_OK; }

static int build_composite_lru(nf_graph_t *g) {
    step_t s[] = {
        { "score_hotness",      NULL, NULL },
        { "filter_hot",         "threshold", "5" },
        { "rank_hotness",       NULL, NULL },
        { "budget_limit",       "budget", "512" },
        { "select_dest_node",   NULL, NULL },
        { "emit_migrate",       NULL, NULL },
    };
    return build_chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

static int build_tinylfu(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate",       NULL, NULL },
        { "filter_freq",        "threshold", "2" },
        { "rank_frequency",     NULL, NULL },
        { "budget_limit",       "budget", "512" },
        { "select_dest_node",   NULL, NULL },
        { "emit_migrate",       NULL, NULL },
    };
    return build_chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

/* New default: Cost-Aware Adaptive Tiering (CAAT).  A full promote+demote
 * pipeline: demote cold DRAM residents to CXL to free capacity, and promote
 * the highest-benefit CXL residents into DRAM within the remaining capacity
 * and the migration budget.
 *
 * This used to be one linear chain (demote's emit_migrate feeding straight
 * into the promote filters). That silently broke bridge integrations
 * (src/numa_flow.c): nf_exec_run()'s result is the union of *sink* node
 * outputs only, and any item mutated by one op then dropped by a later
 * filter *in the same chain* never reaches a sink - so a demoted item that
 * didn't also qualify for promotion (freq/benefit filters) vanished from
 * the graph's result entirely. Its demotion had already happened
 * (current_node mutated, ctx.stats.migrations_done incremented), but the
 * bridge's enumerate-vs-result diff never saw it, so apply() (the thing
 * that performs a *real* migration) was never called.
 *
 * Fix: fork BEFORE either phase mutates anything, splitting on each item's
 * *original* residency (never on the same item twice): DRAM residents only
 * ever go through the demote phase (decide, then a terminal emit_migrate -
 * no filter runs after that mutation); off-DRAM residents only ever go
 * through the promote phase (filters narrow the candidates *first*, then a
 * terminal emit_migrate is the only mutation - nothing runs after it
 * either). Every item ends up mutated at most once and always reaches
 * exactly one sink. */
static int build_caat(nf_graph_t *g) {
    step_t score[] = {
        { "cms_estimate",       NULL, NULL },
        { "score_cost_benefit", NULL, NULL },
    };
    if (build_chain_from(g, score, 2, "s", NULL) != NF_OK) return NF_ERR;

    if (nf_graph_add_node(g, "on_dram", "filter_local") != NF_OK) return NF_ERR;
    nf_graph_node_set_param(g, "on_dram", "node", "0");
    if (nf_graph_add_edge(g, "s2", "on_dram") != NF_OK) return NF_ERR;

    if (nf_graph_add_node(g, "off_dram", "filter_remote") != NF_OK) return NF_ERR;
    nf_graph_node_set_param(g, "off_dram", "node", "0");
    if (nf_graph_add_edge(g, "s2", "off_dram") != NF_OK) return NF_ERR;

    step_t demote[] = {
        { "demote_cold",  "threshold", "1" },
        { "emit_migrate", NULL, NULL },
    };
    if (build_chain_from(g, demote, 2, "d", "on_dram") != NF_OK) return NF_ERR;

    step_t promote[] = {
        { "filter_freq",        "threshold", "1" },
        { "filter_benefit",     "threshold", "0" },
        { "rank_cost",          NULL, NULL },
        { "budget_limit",       "budget", "512" },
        { "select_dest_node",   "require_benefit", "1" },
        { "emit_migrate",       NULL, NULL },
    };
    return build_chain_from(g, promote, (int)(sizeof(promote) / sizeof(promote[0])), "p", "off_dram");
}

/* ---- allocation strategy builders (single-op workflows) ----------------- */
static int build_alloc(const char *op, nf_graph_t *g) {
    if (nf_graph_add_node(g, "alloc", op) != NF_OK) return NF_ERR;
    return NF_OK;
}

static int build_alloc_local_first(nf_graph_t *g) { return build_alloc("alloc_local_first", g); }
static int build_alloc_interleave(nf_graph_t *g) { return build_alloc("alloc_interleave", g); }
static int build_alloc_round_robin(nf_graph_t *g) { return build_alloc("alloc_round_robin", g); }
static int build_alloc_weighted(nf_graph_t *g) { return build_alloc("alloc_weighted", g); }
static int build_alloc_pressure_aware(nf_graph_t *g) { return build_alloc("alloc_pressure_aware", g); }
static int build_alloc_cxl_optimized(nf_graph_t *g) {
    if (nf_graph_add_node(g, "alloc", "alloc_cxl_optimized") != NF_OK) return NF_ERR;
    nf_graph_node_set_param(g, "alloc", "min_size", "1024");
    return NF_OK;
}
static int build_alloc_weighted_interleave(nf_graph_t *g) { return build_alloc("alloc_weighted_interleave", g); }
static int build_alloc_adaptive(nf_graph_t *g) { return build_alloc("alloc_adaptive", g); }
static int build_alloc_latency_aware(nf_graph_t *g) { return build_alloc("alloc_latency_aware", g); }

static const strategy_def_t g_defs[] = {
    { "caat",                    "Cost-aware adaptive tiering (default): migrate only net-positive items ranked by benefit.", 0, build_caat },
    { "composite_lru",           "Composite LRU: staircase-hotness hot items migrate to DRAM.", 0, build_composite_lru },
    { "tinylfu",                 "TinyLFU: Count-Min Sketch + Doorkeeper frequency discovery.", 0, build_tinylfu },
    { "noop",                    "Baseline: no migration.", 0, build_noop },
    { "alloc_local_first",       "Allocation: always place on local DRAM.", 1, build_alloc_local_first },
    { "alloc_interleave",        "Allocation: uniform random node.", 1, build_alloc_interleave },
    { "alloc_round_robin",       "Allocation: round-robin across nodes.", 1, build_alloc_round_robin },
    { "alloc_weighted",          "Allocation: weighted random by static weight.", 1, build_alloc_weighted },
    { "alloc_pressure_aware",    "Allocation: least-pressured node.", 1, build_alloc_pressure_aware },
    { "alloc_cxl_optimized",     "Allocation: small to DRAM, large to CXL.", 1, build_alloc_cxl_optimized },
    { "alloc_weighted_interleave","Allocation: weighted random by pressure-adjusted weight.", 1, build_alloc_weighted_interleave },
    { "alloc_adaptive",          "Allocation: DRAM-first with pressure spill.", 1, build_alloc_adaptive },
    { "alloc_latency_aware",     "Allocation: lowest modeled access cost.", 1, build_alloc_latency_aware },
};

int nf_strategy_build(nf_graph_t *g, const char *name) {
    if (!g || !name) return NF_EINVAL;
    nf_graph_free(g); nf_graph_init(g);
    strncpy(g->name, name, NF_STR_MAX - 1);
    for (size_t i = 0; i < sizeof(g_defs) / sizeof(g_defs[0]); i++) {
        if (strcmp(g_defs[i].name, name) == 0) {
            strncpy(g->description, g_defs[i].desc, NF_STR_MAX - 1);
            return g_defs[i].build(g);
        }
    }
    return NF_ENOENT;
}

int nf_strategy_count(void) { return (int)(sizeof(g_defs) / sizeof(g_defs[0])); }
const char *nf_strategy_name(int i) { return (i >= 0 && i < (int)(sizeof(g_defs)/sizeof(g_defs[0]))) ? g_defs[i].name : NULL; }
const char *nf_strategy_desc(const char *name) {
    for (size_t i = 0; i < sizeof(g_defs)/sizeof(g_defs[0]); i++) if (strcmp(g_defs[i].name, name) == 0) return g_defs[i].desc;
    return NULL;
}
int nf_strategy_is_allocation(const char *name) {
    for (size_t i = 0; i < sizeof(g_defs)/sizeof(g_defs[0]); i++) if (strcmp(g_defs[i].name, name) == 0) return g_defs[i].is_alloc;
    return 0;
}
const char *nf_strategy_default(void) { return "caat"; }
