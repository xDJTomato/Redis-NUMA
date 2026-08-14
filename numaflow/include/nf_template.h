/* =============================================================================
 * nf_template.h - beginner-friendly workflow template library.
 *
 * Templates are higher-level than the raw atomic ops and the 13 strategies:
 * each template bundles a ready-to-run DAG with a use case, a category and
 * sensible default parameters, so a newcomer can pick one, load it into the
 * GUI/TUI/CLI and only tune the knobs that matter for their workload.
 * ========================================================================== */
#ifndef NF_TEMPLATE_H
#define NF_TEMPLATE_H

#include "nf_graph.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Template categories (grouping shown in the GUI/TUI). */
#define NF_TMPL_CAT_TIERING    "tiering"
#define NF_TMPL_CAT_ALLOCATION "allocation"
#define NF_TMPL_CAT_COST       "cost"
#define NF_TMPL_CAT_ADAPTIVE   "adaptive"
#define NF_TMPL_CAT_SPECIAL    "special"

typedef struct nf_template {
    const char *name;
    const char *category;
    const char *description;   /* one-line summary */
    const char *use_case;      /* when to pick this template */
    int (*build)(nf_graph_t *g);
} nf_template_t;

int  nf_template_count(void);
const nf_template_t *nf_template_get(int i);
const nf_template_t *nf_template_find(const char *name);

/* Build a template's DAG into `g` (overwrites). Returns NF_OK or error. */
int  nf_template_build(nf_graph_t *g, const char *name);

/* Return the recommended default template for a fresh start. */
const char *nf_template_default(void);

#ifdef __cplusplus
}
#endif

#endif /* NF_TEMPLATE_H */
