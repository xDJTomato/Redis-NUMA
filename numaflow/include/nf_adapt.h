/* =============================================================================
 * nf_adapt.h - self-adapting DAG: auto-tunes parameters AND structure from
 * cache-health feedback (pure C11).
 *
 * Two mechanisms:
 *   1. Parameter hill-climbing: each registered parameter is nudged one at a
 *      time (round-robin); if the feedback improved, keep the direction, else
 *      reverse it.  Bound by min/max/step.
 *   2. Structure selection: three templates (conservative / balanced /
 *      aggressive) chosen from DRAM-residency and migration churn.
 * ========================================================================== */
#ifndef NF_ADAPT_H
#define NF_ADAPT_H

#include "nf_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NF_ADAPT_MAX_PARAMS 8

typedef enum {
    NF_ADAPT_CONSERVATIVE = 0,
    NF_ADAPT_BALANCED,
    NF_ADAPT_AGGRESSIVE
} nf_adapt_mode_t;

typedef struct {
    char    op[NF_OP_MAX];     /* op to configure (e.g. "filter_benefit") */
    char    param[NF_PARAM_MAX];/* param key (e.g. "threshold") */
    double  value;
    double  min, max, step;
    int     is_double;
    double  last_feedback;
    int     last_dir;          /* +1 / -1 */
    int     ready;             /* has a feedback baseline yet */
} nf_adapt_param_t;

typedef struct {
    nf_adapt_param_t params[NF_ADAPT_MAX_PARAMS];
    int     param_count;
    int     param_index;       /* round-robin cursor */
    nf_adapt_mode_t mode;
    double  low_residency;     /* below -> more aggressive */
    double  high_residency;    /* above -> more conservative */
    double  churn_threshold;   /* migrations/enumerated above -> conservative */
    double  last_feedback;
    int     last_enumerated;
    uint64_t last_migrations;
} nf_adapt_t;

void nf_adapt_init(nf_adapt_t *a);

/* Register a tunable parameter (op + param).  Returns 0 on success. */
int nf_adapt_add_param(nf_adapt_t *a, const char *op, const char *param,
                       double value, double min, double max, double step, int is_double);

/* Observe one evaluation round; returns the recommended structure mode and
 * updates the internal parameter values. */
nf_adapt_mode_t nf_adapt_tune(nf_adapt_t *a, double feedback, uint64_t migrations, int enumerated);

/* Write current parameter values into graph `g` (by op name). */
void nf_adapt_apply_params(const nf_adapt_t *a, nf_graph_t *g);

/* (Re)build the workflow template for the current mode into `g`. */
int nf_adapt_build_graph(const nf_adapt_t *a, nf_graph_t *g);

const char *nf_adapt_mode_name(nf_adapt_mode_t m);

#ifdef __cplusplus
}
#endif

#endif /* NF_ADAPT_H */
