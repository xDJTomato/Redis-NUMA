/* nf_template.c - beginner-friendly workflow template library. */
#include "nf_template.h"
#include "nf_strategy.h"
#include "nf_adapt.h"

#include <stdio.h>
#include <string.h>

typedef struct { const char *op; const char *k; const char *v; } step_t;
static int chain(nf_graph_t *g, const step_t *s, int n) {
    char id[8], prev[8] = "";
    for (int i = 0; i < n; i++) {
        snprintf(id, sizeof(id), "n%d", i + 1);
        if (nf_graph_add_node(g, id, s[i].op) != NF_OK) return NF_ERR;
        if (s[i].k && s[i].v) nf_graph_node_set_param(g, id, s[i].k, s[i].v);
        if (i > 0) nf_graph_add_edge(g, prev, id);
        snprintf(prev, sizeof(prev), "%s", id);
    }
    return NF_OK;
}

/* ---- allocation templates (reuse the 9 alloc strategies) --------------- */
static int build_alloc(nf_graph_t *g, const char *name) { return nf_strategy_build(g, name); }
#define ALLOC_BUILDER(nm) static int build_##nm(nf_graph_t *g) { return build_alloc(g, #nm); }
ALLOC_BUILDER(alloc_local_first)
ALLOC_BUILDER(alloc_interleave)
ALLOC_BUILDER(alloc_round_robin)
ALLOC_BUILDER(alloc_weighted)
ALLOC_BUILDER(alloc_pressure_aware)
ALLOC_BUILDER(alloc_cxl_optimized)
ALLOC_BUILDER(alloc_weighted_interleave)
ALLOC_BUILDER(alloc_adaptive)
ALLOC_BUILDER(alloc_latency_aware)

/* ---- tiering templates --------------------------------------------------- */
static int build_tier_caat(nf_graph_t *g) { return nf_strategy_build(g, "caat"); }

static int build_tier_promote_hot(nf_graph_t *g) {
    step_t s[] = {
        { "score_hotness", NULL, NULL },
        { "filter_hot", "threshold", "5" },
        { "rank_hotness", NULL, NULL },
        { "budget_limit", "budget", "256" },
        { "select_dest_node", NULL, NULL },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

static int build_tier_promote_freq(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "filter_freq", "threshold", "2" },
        { "rank_frequency", NULL, NULL },
        { "budget_limit", "budget", "256" },
        { "select_dest_node", NULL, NULL },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

static int build_tier_demote_cold(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "demote_cold", "threshold", "1" },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

static int build_tier_rebalance(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "score_cost_benefit", NULL, NULL },
        { "demote_cold", "threshold", "1" },
        { "emit_migrate", NULL, NULL },
        { "filter_freq", "threshold", "1" },
        { "filter_benefit", "threshold", "0" },
        { "rank_cost", NULL, NULL },
        { "budget_limit", "budget", "512" },
        { "select_dest_node", "require_benefit", "1" },
        { "emit_migrate", NULL, NULL },
        { "balance_nodes", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

static int build_tier_cost_benefit(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "score_cost_benefit", NULL, NULL },
        { "filter_benefit", "threshold", "1.0" },
        { "rank_cost", NULL, NULL },
        { "budget_limit", "budget", "64" },
        { "select_dest_node", "require_benefit", "1" },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

/* ---- cost-objective templates ------------------------------------------- */
static int build_cost_min_migration(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "filter_freq", "threshold", "5" },
        { "rank_frequency", NULL, NULL },
        { "budget_limit", "budget", "32" },
        { "select_dest_node", NULL, NULL },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}
static int build_cost_min_latency(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "filter_freq", "threshold", "1" },
        { "rank_frequency", NULL, NULL },
        { "budget_limit", "budget", "512" },
        { "select_dest_node", NULL, NULL },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}
static int build_cost_balanced(nf_graph_t *g) { return nf_strategy_build(g, "caat"); }

/* ---- adaptive templates -------------------------------------------------- */
static int build_adaptive_mode(nf_graph_t *g, nf_adapt_mode_t m) {
    nf_adapt_t a; nf_adapt_init(&a); a.mode = m;
    return nf_adapt_build_graph(&a, g);
}
static int build_adapt_conservative(nf_graph_t *g) { return build_adaptive_mode(g, NF_ADAPT_CONSERVATIVE); }
static int build_adapt_balanced(nf_graph_t *g) { return build_adaptive_mode(g, NF_ADAPT_BALANCED); }
static int build_adapt_aggressive(nf_graph_t *g) { return build_adaptive_mode(g, NF_ADAPT_AGGRESSIVE); }

/* ---- special templates --------------------------------------------------- */
static int build_cache_warming(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "filter_freq", "threshold", "1" },
        { "rank_frequency", NULL, NULL },
        { "budget_limit", "budget", "1024" },
        { "select_dest_node", NULL, NULL },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}
static int build_hot_key_pinning(nf_graph_t *g) {
    step_t s[] = {
        { "cms_estimate", NULL, NULL },
        { "filter_freq", "threshold", "3" },
        { "rank_frequency", NULL, NULL },
        { "budget_limit", "budget", "128" },
        { "select_dest_node", NULL, NULL },
        { "emit_migrate", NULL, NULL },
    };
    return chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
}

/* ---- catalog ------------------------------------------------------------- */
static const nf_template_t g_templates[] = {
    /* tiering */
    { "tier_caat",          NF_TMPL_CAT_TIERING, "Promote hot + demote cold (CAAT)", "General-purpose tiering; the default choice", build_tier_caat },
    { "tier_promote_hot",   NF_TMPL_CAT_TIERING, "Promote hot keys only", "DRAM has headroom; no need to reclaim", build_tier_promote_hot },
    { "tier_promote_freq",  NF_TMPL_CAT_TIERING, "Promote frequent keys only", "Frequency-skewed workloads (zipf/hotspot)", build_tier_promote_freq },
    { "tier_demote_cold",   NF_TMPL_CAT_TIERING, "Demote cold keys only", "Reclaim DRAM under memory pressure", build_tier_demote_cold },
    { "tier_rebalance",     NF_TMPL_CAT_TIERING, "Promote + demote + rebalance", "Uneven node pressure; aggressive tuning", build_tier_rebalance },
    { "tier_cost_benefit",  NF_TMPL_CAT_TIERING, "Migrate only net-positive items", "Migration budget is expensive; conservative", build_tier_cost_benefit },
    /* allocation */
    { "alloc_local_first",         NF_TMPL_CAT_ALLOCATION, "Allocate on local DRAM", "Single-socket / latency-critical", build_alloc_local_first },
    { "alloc_interleave",          NF_TMPL_CAT_ALLOCATION, "Random interleave across nodes", "Uniform access; balance bandwidth", build_alloc_interleave },
    { "alloc_round_robin",         NF_TMPL_CAT_ALLOCATION, "Round-robin across nodes", "Deterministic balancing", build_alloc_round_robin },
    { "alloc_weighted",            NF_TMPL_CAT_ALLOCATION, "Weighted-random placement", "Known per-node capacity weights", build_alloc_weighted },
    { "alloc_pressure_aware",      NF_TMPL_CAT_ALLOCATION, "Place on least-pressured node", "Avoid overloading a node", build_alloc_pressure_aware },
    { "alloc_cxl_optimized",       NF_TMPL_CAT_ALLOCATION, "Small to DRAM, large to CXL", "Mixed object sizes", build_alloc_cxl_optimized },
    { "alloc_weighted_interleave", NF_TMPL_CAT_ALLOCATION, "Pressure-adjusted weighted interleave", "Adaptive static placement", build_alloc_weighted_interleave },
    { "alloc_adaptive",            NF_TMPL_CAT_ALLOCATION, "DRAM-first with pressure spill", "Prefer DRAM, spill under pressure", build_alloc_adaptive },
    { "alloc_latency_aware",       NF_TMPL_CAT_ALLOCATION, "Lowest modeled access cost", "Latency-sensitive placement", build_alloc_latency_aware },
    /* cost */
    { "cost_min_latency",    NF_TMPL_CAT_COST, "Minimize access latency", "Latency-bound; promote liberally", build_cost_min_latency },
    { "cost_min_migration",  NF_TMPL_CAT_COST, "Minimize migration count", "Migration bandwidth is scarce", build_cost_min_migration },
    { "cost_balanced",       NF_TMPL_CAT_COST, "Balance latency vs migration cost", "Default cost trade-off", build_cost_balanced },
    /* adaptive */
    { "adapt_conservative", NF_TMPL_CAT_ADAPTIVE, "Adaptive, conservative profile", "Thrashing-prone workloads", build_adapt_conservative },
    { "adapt_balanced",     NF_TMPL_CAT_ADAPTIVE, "Adaptive, balanced profile", "Unknown / changing workload", build_adapt_balanced },
    { "adapt_aggressive",   NF_TMPL_CAT_ADAPTIVE, "Adaptive, aggressive profile", "Under-utilized DRAM", build_adapt_aggressive },
    /* special */
    { "cache_warming",    NF_TMPL_CAT_SPECIAL, "Aggressive cold-start warmup", "Right after restart / failover", build_cache_warming },
    { "hot_key_pinning",  NF_TMPL_CAT_SPECIAL, "Pin hot keys to DRAM", "Stable small hot set", build_hot_key_pinning },
};

int nf_template_count(void) { return (int)(sizeof(g_templates) / sizeof(g_templates[0])); }
const nf_template_t *nf_template_get(int i) { return (i >= 0 && i < nf_template_count()) ? &g_templates[i] : NULL; }
const nf_template_t *nf_template_find(const char *name) {
    for (int i = 0; i < nf_template_count(); i++) if (strcmp(g_templates[i].name, name) == 0) return &g_templates[i];
    return NULL;
}
int nf_template_build(nf_graph_t *g, const char *name) {
    if (!g) return NF_EINVAL;
    const nf_template_t *t = nf_template_find(name);
    if (!t) return NF_ENOENT;
    nf_graph_free(g); nf_graph_init(g);
    strncpy(g->name, t->name, NF_STR_MAX - 1);
    strncpy(g->description, t->description, NF_STR_MAX - 1);
    return t->build(g);
}
const char *nf_template_default(void) { return "tier_caat"; }
