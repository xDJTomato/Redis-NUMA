/* nf_ops.c - atomic operation registry + all decomposed strategy operations.
 *
 * Every existing Redis-NUMA strategy (Composite LRU, TinyLFU and the 9
 * allocation policies) is decomposed here into small composable ops that can
 * be re-arranged as a N8N-style DAG.  The new default strategy (cost-aware
 * adaptive tiering) is also expressed entirely with these ops.
 *
 * Pure C11, no Redis/libnuma dependency.
 */
#include "nf_ops.h"
#include "nf_track.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ===========================================================================
 * registry
 * ======================================================================== */
static nf_op_t g_ops[256];
static int g_op_count = 0;

int nf_ops_register(const nf_op_t *op) {
    if (!op || !op->name || g_op_count >= 256) return NF_ENOMEM;
    for (int i = 0; i < g_op_count; i++)
        if (strcmp(g_ops[i].name, op->name) == 0) return NF_EEXIST;
    g_ops[g_op_count++] = *op;
    return NF_OK;
}

const nf_op_t *nf_ops_find(const char *name) {
    if (!name) return NULL;
    for (int i = 0; i < g_op_count; i++)
        if (strcmp(g_ops[i].name, name) == 0) return &g_ops[i];
    return NULL;
}

int nf_ops_count(void) { return g_op_count; }
const nf_op_t *nf_ops_get(int index) { return (index >= 0 && index < g_op_count) ? &g_ops[index] : NULL; }
void nf_ops_clear(void) { g_op_count = 0; }

int nf_ctx_node_index(const nf_ctx_t *ctx, int node) {
    if (!ctx || ctx->topo_count <= 0) return 0;
    if (node < 0) return 0;
    if (node >= ctx->topo_count) return ctx->topo_count - 1;
    return node;
}

/* ---- param helpers ------------------------------------------------------ */
static int    p_int(const nf_ctx_t *ctx, const char *k, int d) { return ctx && ctx->params ? nf_params_get_int(ctx->params, k, d) : d; }
static double p_dbl(const nf_ctx_t *ctx, const char *k, double d) { return ctx && ctx->params ? nf_params_get_double(ctx->params, k, d) : d; }
static int    p_bool(const nf_ctx_t *ctx, const char *k, int d) { const char *v = ctx && ctx->params ? nf_params_get(ctx->params, k) : NULL; return v ? (atoi(v) || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0) : d; }

/* ---- shared helpers ----------------------------------------------------- */
static int nf_local_node(const nf_ctx_t *ctx) {
    return ctx && ctx->env ? (int)ctx->env->local_cpu_node : 0;
}

/* Best destination node for an item: minimize modeled access cost with a
 * pressure penalty, and respect node capacity (a node must have room for the
 * item unless the item is already resident there). */
static int nf_best_node(const nf_ctx_t *ctx, const nf_item_t *it) {
    int best = -1;
    double best_cost = 1e300;
    int local = nf_local_node(ctx);
    for (int n = 0; n < ctx->topo_count; n++) {
        /* capacity check: must have room unless already resident */
        if (it->current_node != n) {
            double used = (double)ctx->topo[n].used_bytes;
            double cap = (double)ctx->topo[n].total_bytes;
            if (cap > 0 && used + (double)it->value_size > cap) continue;
        }
        double c = nf_numa_access_cost(ctx->env, local, n, it->value_size);
        c *= (1.0 + 2.0 * ctx->topo[n].pressure);
        if (c < best_cost) { best_cost = c; best = n; }
    }
    if (best < 0) best = it->current_node >= 0 ? it->current_node : 0;
    return best;
}

/* Net benefit of migrating an item to `dst` (can be negative). */
static double nf_benefit(const nf_ctx_t *ctx, const nf_item_t *it, int dst) {
    int local = nf_local_node(ctx);
    double cur = nf_numa_access_cost(ctx->env, local, it->current_node, it->value_size);
    double tgt = nf_numa_access_cost(ctx->env, local, dst, it->value_size);
    double rate = it->freq_est > 0 ? (double)it->freq_est
                : (it->access_count > 0 ? log2(1.0 + (double)it->access_count) : 0.0);
    double gain = (cur - tgt) * rate;
    double mcost = nf_numa_migrate_cost(ctx->env, it->current_node, dst, it->value_size);
    return gain - mcost;
}

/* Staircase decay for Composite-LRU-style hotness. */
static int nf_staircase_decay(uint64_t idle) {
    if (idle >= 1800) return 4;   /* >= 30 min : fully cleared */
    if (idle >= 300)  return 3;   /* >= 5 min  */
    if (idle >= 60)   return 2;   /* >= 1 min  */
    if (idle >= 10)   return 1;   /* >= 10 s   */
    return 0;
}

/* ===========================================================================
 * ALLOCATION operations (decide target node for each item/request)
 * ======================================================================== */
static int op_alloc_local_first(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int node = p_int(ctx, "node", 0); node = nf_ctx_node_index(ctx, node);
    for (size_t i = 0; i < in->count; i++) { nf_item_t it = in->items[i]; it.selected_node = node; nf_items_push(out, &it); }
    return NF_OK;
}
static int op_alloc_interleave(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) { nf_item_t it = in->items[i]; it.selected_node = nf_rng_int(&ctx->rng, ctx->topo_count); nf_items_push(out, &it); }
    return NF_OK;
}
static int op_alloc_round_robin(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) { nf_item_t it = in->items[i]; it.selected_node = (int)(i % (size_t)ctx->topo_count); nf_items_push(out, &it); }
    return NF_OK;
}
static int op_alloc_weighted(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    double total = 0; for (int n = 0; n < ctx->topo_count; n++) total += ctx->topo[n].weight > 0 ? ctx->topo[n].weight : 1.0;
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        double r = nf_rng_double(&ctx->rng) * total; double cum = 0; int sel = 0;
        for (int n = 0; n < ctx->topo_count; n++) { double w = ctx->topo[n].weight > 0 ? ctx->topo[n].weight : 1.0; cum += w; if (r < cum) { sel = n; break; } }
        it.selected_node = sel; nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_alloc_pressure_aware(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i]; int best = 0; double bp = 1e300;
        for (int n = 0; n < ctx->topo_count; n++) { double p = ctx->topo[n].pressure; if (p < bp) { bp = p; best = n; } }
        it.selected_node = best; nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_alloc_cxl_optimized(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; size_t min_sz = (size_t)p_int(ctx, "min_size", 1024); int big = nf_ctx_node_index(ctx, 1);
    for (size_t i = 0; i < in->count; i++) { nf_item_t it = in->items[i]; it.selected_node = it.value_size < min_sz ? 0 : big; nf_items_push(out, &it); }
    return NF_OK;
}
static int op_alloc_weighted_interleave(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    double total = 0; double w[NF_MAX_NODES];
    for (int n = 0; n < ctx->topo_count; n++) { w[n] = (ctx->topo[n].weight > 0 ? ctx->topo[n].weight : 1.0) * (1.0 - ctx->topo[n].pressure); if (w[n] < 0.01) w[n] = 0.01; total += w[n]; }
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i]; double r = nf_rng_double(&ctx->rng) * total; double cum = 0; int sel = 0;
        for (int n = 0; n < ctx->topo_count; n++) { cum += w[n]; if (r < cum) { sel = n; break; } }
        it.selected_node = sel; nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_alloc_adaptive(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; double thr = p_dbl(ctx, "threshold", 0.8);
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        if (ctx->topo_count > 0 && ctx->topo[0].pressure < thr) it.selected_node = 0;
        else { int best = 0; double bp = 1e300; for (int n = 0; n < ctx->topo_count; n++) { double p = ctx->topo[n].pressure; if (p < bp) { bp = p; best = n; } } it.selected_node = best; }
        nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_alloc_latency_aware(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) { nf_item_t it = in->items[i]; it.selected_node = nf_best_node(ctx, &it); nf_items_push(out, &it); }
    return NF_OK;
}

/* ===========================================================================
 * SCORE operations (derive fields from access history)
 * ======================================================================== */
static int op_score_hotness(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        uint64_t idle = ctx->tick > it.recency ? ctx->tick - it.recency : 0;
        int raw = it.access_count > 7 ? 7 : (int)it.access_count;
        int d = nf_staircase_decay(idle);
        int h = raw - d; if (h < 0) h = 0;
        it.hotness = (uint8_t)h; nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_cms_observe(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    if (ctx->tracker) for (size_t i = 0; i < in->count; i++) nf_tracker_observe(ctx->tracker, in->items[i].key);
    return nf_items_copy(out, in);
}
static int op_cms_estimate(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        it.freq_est = ctx->tracker ? nf_tracker_freq(ctx->tracker, it.key) : (it.access_count > 15 ? 15 : (uint32_t)it.access_count);
        nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_global_decay(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; if (ctx->tracker) nf_tracker_decay(ctx->tracker);
    return nf_items_copy(out, in);
}
static int op_score_ewma(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    double a = p_dbl(ctx, "alpha", 0.4), b = p_dbl(ctx, "beta", 0.4), g = p_dbl(ctx, "gamma", 0.2);
    uint64_t max_acc = 1, max_rec = 1;
    for (size_t i = 0; i < in->count; i++) { if (in->items[i].access_count > max_acc) max_acc = in->items[i].access_count; if (in->items[i].recency > max_rec) max_rec = in->items[i].recency; }
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        double fn = (double)it.freq_est / 15.0;
        double rn = (double)it.recency / (double)max_rec;
        double hn = (double)it.hotness / 7.0;
        it.ewma = a * fn + b * rn + g * hn; nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_score_cost_benefit(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        int dst = nf_best_node(ctx, &it);
        it.cost_benefit = nf_benefit(ctx, &it, dst);
        nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_decay_hotness(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        uint64_t idle = ctx->tick > it.recency ? ctx->tick - it.recency : 0;
        int d = nf_staircase_decay(idle);
        int h = (int)it.hotness - d; if (h < 0) h = 0; it.hotness = (uint8_t)h;
        nf_items_push(out, &it);
    }
    return NF_OK;
}

/* ===========================================================================
 * FILTER operations (drop non-matching items)
 * ======================================================================== */
static int op_filter_hot(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int th = p_int(ctx, "threshold", 5);
    for (size_t i = 0; i < in->count; i++) if (in->items[i].hotness >= (uint8_t)th) nf_items_push(out, &in->items[i]);
    return NF_OK;
}
static int op_filter_freq(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int th = p_int(ctx, "threshold", 2);
    for (size_t i = 0; i < in->count; i++) if ((int)in->items[i].freq_est >= th) nf_items_push(out, &in->items[i]);
    return NF_OK;
}
static int op_filter_cold(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int th = p_int(ctx, "threshold", 2);
    for (size_t i = 0; i < in->count; i++) if (in->items[i].hotness < (uint8_t)th) nf_items_push(out, &in->items[i]);
    return NF_OK;
}
static int op_filter_remote(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int node = p_int(ctx, "node", nf_local_node(ctx));
    for (size_t i = 0; i < in->count; i++) if (in->items[i].current_node != node) nf_items_push(out, &in->items[i]);
    return NF_OK;
}
static int op_filter_local(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int node = p_int(ctx, "node", nf_local_node(ctx));
    for (size_t i = 0; i < in->count; i++) if (in->items[i].current_node == node) nf_items_push(out, &in->items[i]);
    return NF_OK;
}
static int op_filter_size_min(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; size_t min = (size_t)p_int(ctx, "min", 4096);
    for (size_t i = 0; i < in->count; i++) if (in->items[i].value_size >= min) nf_items_push(out, &in->items[i]);
    return NF_OK;
}
static int op_filter_size_max(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; size_t max = (size_t)p_int(ctx, "max", 65536);
    for (size_t i = 0; i < in->count; i++) if (in->items[i].value_size <= max) nf_items_push(out, &in->items[i]);
    return NF_OK;
}
static int op_filter_benefit(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; double th = p_dbl(ctx, "threshold", 0.0);
    for (size_t i = 0; i < in->count; i++) if (in->items[i].cost_benefit > th) nf_items_push(out, &in->items[i]);
    return NF_OK;
}

/* ===========================================================================
 * RANK operations (stable sort, best-first)
 * ======================================================================== */
static int cmp_recency(const nf_item_t *a, const nf_item_t *b) { return a->recency < b->recency ? 1 : (a->recency > b->recency ? -1 : 0); }
static int cmp_freq(const nf_item_t *a, const nf_item_t *b) { return a->freq_est < b->freq_est ? 1 : (a->freq_est > b->freq_est ? -1 : 0); }
static int cmp_hotness(const nf_item_t *a, const nf_item_t *b) { return a->hotness < b->hotness ? 1 : (a->hotness > b->hotness ? -1 : 0); }
static int cmp_cost(const nf_item_t *a, const nf_item_t *b) { return a->cost_benefit < b->cost_benefit ? 1 : (a->cost_benefit > b->cost_benefit ? -1 : 0); }
static int cmp_ewma(const nf_item_t *a, const nf_item_t *b) { return a->ewma < b->ewma ? 1 : (a->ewma > b->ewma ? -1 : 0); }
static int cmp_size(const nf_item_t *a, const nf_item_t *b) { return a->value_size < b->value_size ? 1 : (a->value_size > b->value_size ? -1 : 0); }

static int rank_common(const nf_items_t *in, nf_items_t *out, nf_item_cmp_fn cmp) {
    int rc = nf_items_copy(out, in); if (rc != NF_OK) return rc; nf_items_sort(out, cmp); return NF_OK;
}
static int op_rank_lru(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) { (void)op; (void)ctx; return rank_common(in, out, cmp_recency); }
static int op_rank_frequency(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) { (void)op; (void)ctx; return rank_common(in, out, cmp_freq); }
static int op_rank_hotness(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) { (void)op; (void)ctx; return rank_common(in, out, cmp_hotness); }
static int op_rank_cost(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) { (void)op; (void)ctx; return rank_common(in, out, cmp_cost); }
static int op_rank_ewma(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) { (void)op; (void)ctx; return rank_common(in, out, cmp_ewma); }
static int op_rank_size(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) { (void)op; (void)ctx; return rank_common(in, out, cmp_size); }

/* ===========================================================================
 * DECIDE / EMIT operations
 * ======================================================================== */
static int op_select_dest_node(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int require_benefit = p_bool(ctx, "require_benefit", 0);
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        int dst = nf_best_node(ctx, &it);
        double benefit = nf_benefit(ctx, &it, dst);
        it.selected_node = dst;
        it.migrate = (dst != it.current_node) && (!require_benefit || benefit > 0.0);
        it.cost_benefit = benefit;
        nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_budget_limit(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int budget = p_int(ctx, "budget", ctx->budget);
    size_t n = (budget < 0 || (size_t)budget > in->count) ? in->count : (size_t)budget;
    for (size_t i = 0; i < n; i++) nf_items_push(out, &in->items[i]);
    ctx->stats.migrations_skipped += in->count - n;
    return NF_OK;
}
static int op_emit_migrate(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        if (it.migrate && it.selected_node >= 0 && it.selected_node != it.current_node) {
            /* capacity enforcement: target must have room */
            double cap = (double)ctx->topo[it.selected_node].total_bytes;
            double used = (double)ctx->topo[it.selected_node].used_bytes;
            int has_room = (cap <= 0) || (used + (double)it.value_size <= cap);
            if (has_room) {
                double mc = nf_numa_migrate_cost(ctx->env, it.current_node, it.selected_node, it.value_size);
                ctx->stats.total_cost_ns += mc;
                ctx->stats.migrations_done++;
                nf_numa_account_free(ctx->env, it.current_node, it.value_size);
                nf_numa_account_alloc(ctx->env, it.selected_node, it.value_size);
                it.current_node = it.selected_node;
            } else {
                ctx->stats.migrations_skipped++;
            }
        } else if (it.migrate) {
            ctx->stats.migrations_skipped++;
        }
        it.migrate = 0;
        nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_demote_cold(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int th = p_int(ctx, "threshold", 1);
    int dram = nf_ctx_node_index(ctx, p_int(ctx, "dram_node", 0));
    int cxl = nf_ctx_node_index(ctx, p_int(ctx, "cxl_node", 1));
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        if (it.current_node == dram && (int)it.freq_est < th) {
            it.selected_node = cxl; it.migrate = 1;
        }
        nf_items_push(out, &it);
    }
    return NF_OK;
}
static int op_balance_nodes(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op;
    double avg = 0; for (int n = 0; n < ctx->topo_count; n++) avg += ctx->topo[n].pressure; avg /= ctx->topo_count > 0 ? ctx->topo_count : 1;
    int mn = 0; double mp = 1e300; for (int n = 0; n < ctx->topo_count; n++) if (ctx->topo[n].pressure < mp) { mp = ctx->topo[n].pressure; mn = n; }
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        if (it.current_node >= 0 && ctx->topo[it.current_node].pressure > avg + 0.1) {
            it.selected_node = mn; it.migrate = 1;
        }
        nf_items_push(out, &it);
    }
    return NF_OK;
}

/* ===========================================================================
 * TRACK operation
 * ======================================================================== */
static int op_track_access(const nf_op_t *op, nf_ctx_t *ctx, const nf_items_t *in, nf_items_t *out) {
    (void)op; int local = nf_local_node(ctx);
    for (size_t i = 0; i < in->count; i++) {
        nf_item_t it = in->items[i];
        int is_local = (it.current_node == local);
        double cost = nf_numa_access_cost(ctx->env, local, it.current_node, it.value_size);
        ctx->stats.accesses++;
        ctx->stats.total_cost_ns += cost;
        if (is_local) ctx->stats.local_hits++; else ctx->stats.remote_hits++;
        int nd = it.current_node >= 0 ? it.current_node : 0;
        if (nd < NF_MAX_NODES) ctx->stats.node_accesses[nd]++;
        if (ctx->tracker) nf_tracker_record_access(ctx->tracker, nd, is_local, cost);
        nf_items_push(out, &it);
    }
    return NF_OK;
}

/* ===========================================================================
 * registration
 * ======================================================================== */
#define OP(name, title, desc, cat) { #name, title, desc, cat, op_##name, NULL, 0 }

void nf_ops_register_all(void) {
    static const nf_op_t ops[] = {
        /* allocation */
        OP(alloc_local_first,        "Alloc: Local First",        "Always place on the local DRAM node (or a chosen node).", "alloc"),
        OP(alloc_interleave,         "Alloc: Interleave",         "Place on a uniformly random node.", "alloc"),
        OP(alloc_round_robin,        "Alloc: Round Robin",        "Distribute requests across nodes in order.", "alloc"),
        OP(alloc_weighted,           "Alloc: Weighted",           "Weighted-random placement by static node weight.", "alloc"),
        OP(alloc_pressure_aware,     "Alloc: Pressure Aware",     "Place on the least-pressured node.", "alloc"),
        OP(alloc_cxl_optimized,      "Alloc: CXL Optimized",      "Small objects to DRAM, large objects to CXL.", "alloc"),
        OP(alloc_weighted_interleave,"Alloc: Weighted Interleave","Weighted-random by pressure-adjusted weight.", "alloc"),
        OP(alloc_adaptive,           "Alloc: Adaptive",           "DRAM-first while under threshold, else spill to least-pressured node.", "alloc"),
        OP(alloc_latency_aware,      "Alloc: Latency Aware",      "Place on the node with the lowest modeled access cost.", "alloc"),
        /* score */
        OP(score_hotness,           "Score: Hotness",            "Derive staircase-decayed hotness from access history.", "score"),
        OP(cms_observe,             "Score: CMS Observe",        "Feed keys into the Count-Min Sketch + Doorkeeper.", "score"),
        OP(cms_estimate,            "Score: CMS Estimate",       "Read frequency estimates from the Count-Min Sketch.", "score"),
        OP(global_decay,            "Score: Global Decay",       "Halve all frequency counters and clear the doorkeeper.", "score"),
        OP(score_ewma,              "Score: EWMA Blend",         "Blend frequency, recency and hotness into one score.", "score"),
        OP(score_cost_benefit,      "Score: Cost Benefit",       "Compute net benefit of migrating each item.", "score"),
        OP(decay_hotness,           "Score: Decay Hotness",      "Apply staircase idle decay to hotness.", "score"),
        /* filter */
        OP(filter_hot,              "Filter: Hot",               "Keep items at or above a hotness threshold.", "filter"),
        OP(filter_freq,             "Filter: Frequent",          "Keep items at or above a frequency threshold.", "filter"),
        OP(filter_cold,             "Filter: Cold",              "Keep items below a hotness threshold.", "filter"),
        OP(filter_remote,           "Filter: Remote",            "Keep items not resident on a chosen node.", "filter"),
        OP(filter_local,            "Filter: Local",             "Keep items resident on a chosen node.", "filter"),
        OP(filter_size_min,         "Filter: Size >= Min",       "Keep items at least a minimum size.", "filter"),
        OP(filter_size_max,         "Filter: Size <= Max",       "Keep items at most a maximum size.", "filter"),
        OP(filter_benefit,           "Filter: Benefit",            "Keep items with a positive migration cost-benefit.", "filter"),
        /* rank */
        OP(rank_lru,                "Rank: LRU",                 "Sort by recency (most recent first).", "rank"),
        OP(rank_frequency,          "Rank: Frequency",           "Sort by frequency estimate (highest first).", "rank"),
        OP(rank_hotness,            "Rank: Hotness",             "Sort by hotness (highest first).", "rank"),
        OP(rank_cost,               "Rank: Cost Benefit",        "Sort by migration cost-benefit (highest first).", "rank"),
        OP(rank_ewma,               "Rank: EWMA",                "Sort by blended EWMA score (highest first).", "rank"),
        OP(rank_size,               "Rank: Size",                "Sort by value size (largest first).", "rank"),
        /* decide / emit */
        OP(select_dest_node,        "Decide: Destination",       "Choose the best target node for each item.", "decide"),
        OP(budget_limit,            "Decide: Budget",            "Keep only the first N candidates.", "decide"),
        OP(emit_migrate,            "Emit: Migrate",             "Apply migrations and update placement + cost.", "emit"),
        OP(demote_cold,              "Emit: Demote Cold",          "Demote cold DRAM-resident items to a slower tier.", "emit"),
        OP(balance_nodes,           "Emit: Balance",             "Move items from over-pressured nodes to the least-pressured node.", "emit"),
        /* track */
        OP(track_access,            "Track: Access",             "Record modeled accesses into stats and the tracker.", "track"),
    };
    for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) nf_ops_register(&ops[i]);
}

#undef OP
