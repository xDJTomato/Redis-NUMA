/* nf_adapt.c - self-adapting DAG (parameter hill-climb + structure selection). */
#include "nf_adapt.h"
#include "nf_strategy.h"

#include <stdio.h>
#include <string.h>

void nf_adapt_init(nf_adapt_t *a) {
    memset(a, 0, sizeof(*a));
    a->low_residency = 0.55;
    a->high_residency = 0.85;
    a->churn_threshold = 0.35;
    a->mode = NF_ADAPT_BALANCED;
}

int nf_adapt_add_param(nf_adapt_t *a, const char *op, const char *param,
                       double value, double min, double max, double step, int is_double) {
    if (!a || a->param_count >= NF_ADAPT_MAX_PARAMS) return NF_ERR;
    nf_adapt_param_t *p = &a->params[a->param_count++];
    memset(p, 0, sizeof(*p));
    strncpy(p->op, op, NF_OP_MAX - 1);
    strncpy(p->param, param, NF_PARAM_MAX - 1);
    p->value = value; p->min = min; p->max = max; p->step = step;
    p->is_double = is_double; p->last_dir = 1; p->ready = 0;
    return NF_OK;
}

nf_adapt_mode_t nf_adapt_tune(nf_adapt_t *a, double feedback, uint64_t migrations, int enumerated) {
    if (!a) return NF_ADAPT_BALANCED;
    double churn = enumerated > 0 ? (double)migrations / (double)enumerated : 0.0;

    /* ---- structure selection ---- */
    if (feedback < a->low_residency && churn < a->churn_threshold) {
        a->mode = NF_ADAPT_AGGRESSIVE;     /* under-utilizing DRAM: push harder */
    } else if (churn > a->churn_threshold && feedback < a->high_residency) {
        a->mode = NF_ADAPT_CONSERVATIVE;   /* thrashing: back off */
    } else {
        a->mode = NF_ADAPT_BALANCED;
    }

    /* ---- parameter hill-climb (one parameter per round, round-robin) ---- */
    if (a->param_count > 0) {
        nf_adapt_param_t *p = &a->params[a->param_index % a->param_count];
        if (!p->ready) {
            p->ready = 1;
            p->last_feedback = feedback;
        } else {
            double delta = feedback - p->last_feedback;
            if (delta > 1e-9) {
                /* improved: keep direction */
            } else if (delta < -1e-9) {
                p->last_dir = -p->last_dir;   /* worsened: reverse */
            } else {
                /* flat: keep */
            }
            p->value += p->last_dir * p->step;
            if (p->value < p->min) p->value = p->min;
            if (p->value > p->max) p->value = p->max;
            p->last_feedback = feedback;
        }
        a->param_index = (a->param_index + 1) % a->param_count;
    }

    a->last_feedback = feedback;
    a->last_migrations = migrations;
    a->last_enumerated = enumerated;
    return a->mode;
}

void nf_adapt_apply_params(const nf_adapt_t *a, nf_graph_t *g) {
    if (!a || !g) return;
    for (int i = 0; i < a->param_count; i++) {
        const nf_adapt_param_t *p = &a->params[i];
        if (!p->ready) continue;
        char val[NF_VALUE_MAX];
        if (p->is_double) snprintf(val, sizeof(val), "%.2f", p->value);
        else snprintf(val, sizeof(val), "%d", (int)(p->value + 0.5));
        for (int n = 0; n < g->node_count; n++) {
            if (strcmp(g->nodes[n].op, p->op) == 0)
                nf_graph_node_set_param(g, g->nodes[n].id, p->param, val);
        }
    }
}

/* ---- structure templates ---- */
typedef struct { const char *op; const char *k; const char *v; } step_t;
static int chain_from(nf_graph_t *g, const step_t *s, int n, const char *prefix, const char *connect_from) {
    char id[16], prev[16];
    snprintf(prev, sizeof(prev), "%s", connect_from ? connect_from : "");
    for (int i = 0; i < n; i++) {
        snprintf(id, sizeof(id), "%s%d", prefix, i + 1);
        nf_graph_add_node(g, id, s[i].op);
        if (s[i].k && s[i].v) nf_graph_node_set_param(g, id, s[i].k, s[i].v);
        if (prev[0]) nf_graph_add_edge(g, prev, id);
        snprintf(prev, sizeof(prev), "%s", id);
    }
    return NF_OK;
}
static int chain(nf_graph_t *g, const step_t *s, int n) {
    return chain_from(g, s, n, "n", NULL);
}

int nf_adapt_build_graph(const nf_adapt_t *a, nf_graph_t *g) {
    if (!a || !g) return NF_EINVAL;
    nf_graph_free(g); nf_graph_init(g);
    strncpy(g->name, "adaptive", NF_STR_MAX - 1);

    switch (a->mode) {
        case NF_ADAPT_CONSERVATIVE: {
            /* promote only the very highest-benefit keys; no demotion */
            step_t s[] = {
                { "cms_estimate", NULL, NULL },
                { "score_cost_benefit", NULL, NULL },
                { "filter_benefit", "threshold", "1.0" },
                { "rank_cost", NULL, NULL },
                { "budget_limit", "budget", "64" },
                { "select_dest_node", "require_benefit", "1" },
                { "emit_migrate", NULL, NULL },
            };
            strncpy(g->description, "adaptive: conservative (high-benefit promotion only)", NF_STR_MAX - 1);
            chain(g, s, (int)(sizeof(s) / sizeof(s[0])));
            break;
        }
        case NF_ADAPT_AGGRESSIVE: {
            /* full promote + demote + rebalance. Fork BEFORE either phase
             * mutates anything (same fix as nf_strategy.c's build_caat, see
             * its comment): DRAM residents only go through demote (decide,
             * then a terminal emit_migrate), off-DRAM residents only go
             * through promote (filters narrow first, mutate last) - each
             * item is mutated at most once and always reaches a sink. */
            step_t score[] = {
                { "cms_estimate", NULL, NULL },
                { "score_cost_benefit", NULL, NULL },
            };
            chain_from(g, score, (int)(sizeof(score) / sizeof(score[0])), "s", NULL);
            nf_graph_add_node(g, "on_dram", "filter_local");
            nf_graph_node_set_param(g, "on_dram", "node", "0");
            nf_graph_add_edge(g, "s2", "on_dram");
            nf_graph_add_node(g, "off_dram", "filter_remote");
            nf_graph_node_set_param(g, "off_dram", "node", "0");
            nf_graph_add_edge(g, "s2", "off_dram");
            step_t demote[] = {
                { "demote_cold", "threshold", "0" },
                { "emit_migrate", NULL, NULL },
            };
            chain_from(g, demote, (int)(sizeof(demote) / sizeof(demote[0])), "d", "on_dram");
            step_t promote[] = {
                { "filter_freq", "threshold", "1" },
                { "filter_benefit", "threshold", "0" },
                { "rank_cost", NULL, NULL },
                { "budget_limit", "budget", "512" },
                { "select_dest_node", "require_benefit", "1" },
                { "emit_migrate", NULL, NULL },
                { "balance_nodes", NULL, NULL },
            };
            chain_from(g, promote, (int)(sizeof(promote) / sizeof(promote[0])), "p", "off_dram");
            strncpy(g->description, "adaptive: aggressive (promote + demote + rebalance)", NF_STR_MAX - 1);
            break;
        }
        default: {
            /* balanced = CAAT (promote + demote) */
            nf_strategy_build(g, "caat");
            strncpy(g->name, "adaptive", NF_STR_MAX - 1);
            strncpy(g->description, "adaptive: balanced (CAAT)", NF_STR_MAX - 1);
            break;
        }
    }
    return NF_OK;
}

const char *nf_adapt_mode_name(nf_adapt_mode_t m) {
    switch (m) {
        case NF_ADAPT_CONSERVATIVE: return "conservative";
        case NF_ADAPT_AGGRESSIVE: return "aggressive";
        default: return "balanced";
    }
}
