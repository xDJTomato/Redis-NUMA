/* nf_bench.c - fair QEMU-free strategy evaluator (pure C11). */
#include "nf_bench.h"
#include "nf_ops.h"
#include "nf_exec.h"
#include "nf_strategy.h"
#include "nf_track.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ---- key definition (immutable) ---- */
typedef struct { char key[NF_KEY_MAX]; size_t size; } keydef_t;
typedef struct { char key[NF_KEY_MAX]; size_t size; int node; uint64_t acc; uint64_t rec; } keystate_t;

typedef struct {
    double   local_hit_ratio;
    double   access_cost;
    double   migration_cost;
    double   net_cost;
    uint64_t migrations;
    double   feedback;
    uint64_t node_bytes[NF_MAX_NODES];
} run_result_t;

/* ---- tiny string -> index map ------------------------------------------- */
typedef struct { size_t cap; int *idx; char **keys; } strmap_t;
static uint64_t sm_hash(const char *s) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (; *s; s++) { h ^= (uint8_t)*s; h *= UINT64_C(0x100000001b3); }
    return h;
}
static void sm_init(strmap_t *m, size_t cap) {
    m->cap = cap * 2 + 1;
    m->idx = (int *)calloc(m->cap, sizeof(int));
    m->keys = (char **)calloc(m->cap, sizeof(char *));
}
static void sm_free(strmap_t *m) { free(m->idx); free(m->keys); }
static void sm_put(strmap_t *m, const char *key, int idx) {
    size_t i = sm_hash(key) % m->cap;
    while (m->keys[i]) i = (i + 1) % m->cap;
    m->keys[i] = (char *)key; m->idx[i] = idx + 1;
}
static int sm_get(strmap_t *m, const char *key) {
    size_t i = sm_hash(key) % m->cap;
    while (m->keys[i]) { if (strcmp(m->keys[i], key) == 0) return m->idx[i] - 1; i = (i + 1) % m->cap; }
    return -1;
}

void nf_bench_config_defaults(nf_bench_config_t *c) {
    if (!c->workload || !c->workload[0]) c->workload = "zipf";
    if (c->keys == 0) c->keys = 5000;
    if (c->accesses == 0) c->accesses = 200000;
    if (c->epoch == 0) c->epoch = 2000;
    if (c->budget == 0) c->budget = 256;
    if (c->nodes == 0) c->nodes = 2;
    if (c->seed == 0) c->seed = UINT64_C(20240517);
}

/* ---- workload generation ------------------------------------------------ */
static size_t pick_size(nf_rng_t *rng) {
    static const size_t sizes[] = { 64, 256, 1024, 4096, 16384, 65536, 262144 };
    static const int   prob[]   = { 30, 30, 18, 12, 6, 3, 1 };
    int total = 0; for (size_t i = 0; i < sizeof(prob)/sizeof(prob[0]); i++) total += prob[i];
    int r = nf_rng_int(rng, total); int cum = 0;
    for (size_t i = 0; i < sizeof(prob)/sizeof(prob[0]); i++) { cum += prob[i]; if (r < cum) return sizes[i]; }
    return sizes[0];
}

static void gen_trace(const nf_bench_config_t *cfg, size_t keycount, int **trace_out, size_t *len_out) {
    nf_rng_t rng = nf_rng_seed(cfg->seed);
    int *trace = (int *)malloc(cfg->accesses * sizeof(int));
    size_t n = cfg->accesses;

    if (strcmp(cfg->workload, "uniform") == 0) {
        for (size_t i = 0; i < n; i++) trace[i] = nf_rng_int(&rng, (int)keycount);
    } else if (strcmp(cfg->workload, "hotspot") == 0) {
        size_t hot = keycount / 5; if (hot < 1) hot = 1;
        for (size_t i = 0; i < n; i++) trace[i] = (nf_rng_double(&rng) < 0.8) ? nf_rng_int(&rng, (int)hot) : nf_rng_int(&rng, (int)keycount);
    } else if (strcmp(cfg->workload, "temporal") == 0) {
        /* recency-friendly: 80% from a recent window, 20% anywhere */
        size_t window = keycount / 10; if (window < 8) window = 8;
        int *recent = (int *)malloc(window * sizeof(int));
        for (size_t i = 0; i < window; i++) recent[i] = nf_rng_int(&rng, (int)keycount);
        size_t wpos = 0;
        for (size_t i = 0; i < n; i++) {
            int k;
            if (nf_rng_double(&rng) < 0.8) k = recent[nf_rng_int(&rng, (int)window)];
            else k = nf_rng_int(&rng, (int)keycount);
            trace[i] = k;
            recent[wpos] = k; wpos = (wpos + 1) % window;
        }
        free(recent);
    } else { /* zipf (default) */
        double s = 1.0;
        double *cdf = (double *)malloc(keycount * sizeof(double));
        double sum = 0;
        for (size_t i = 0; i < keycount; i++) { sum += 1.0 / pow((double)(i + 1), s); cdf[i] = sum; }
        for (size_t i = 0; i < keycount; i++) cdf[i] /= sum;
        for (size_t i = 0; i < n; i++) {
            double r = nf_rng_double(&rng);
            size_t lo = 0, hi = keycount - 1;
            while (lo < hi) { size_t mid = (lo + hi) / 2; if (cdf[mid] < r) lo = mid + 1; else hi = mid; }
            trace[i] = (int)lo;
        }
        free(cdf);
    }
    *trace_out = trace; *len_out = n;
}

/* ---- placement ----------------------------------------------------------- */
static void reset_env_accounting(numaflow_env_t *env, keystate_t *st, size_t n) {
    for (int i = 0; i < env->node_count; i++) { env->nodes[i].used_bytes = 0; env->nodes[i].pressure = 0; }
    for (size_t i = 0; i < n; i++) nf_numa_account_alloc(env, st[i].node, st[i].size);
}

static void place_fixed(keystate_t *st, size_t n, int node) { for (size_t i = 0; i < n; i++) st[i].node = node; }

static void place_alloc(const nf_bench_config_t *cfg, numaflow_env_t *env, nf_ctx_t *ctx, keydef_t *defs, keystate_t *st, size_t n, const char *alloc_name) {
    (void)cfg; (void)env;
    nf_graph_t ag; nf_graph_init(&ag);
    if (nf_strategy_build(&ag, alloc_name) != NF_OK) { nf_graph_free(&ag); place_fixed(st, n, 0); return; }
    nf_items_t items; nf_items_init(&items);
    for (size_t i = 0; i < n; i++) {
        nf_item_t it; memset(&it, 0, sizeof(it));
        strncpy(it.key, defs[i].key, NF_KEY_MAX - 1);
        it.value_size = defs[i].size; it.current_node = -1;
        nf_items_push(&items, &it);
    }
    nf_exec_t ex; nf_exec_init(&ex);
    nf_exec_run(&ex, &ag, &items, ctx);
    for (size_t i = 0; i < ex.result.count && i < n; i++) st[i].node = ex.result.items[i].selected_node;
    nf_exec_free(&ex); nf_items_free(&items); nf_graph_free(&ag);
}

/* ---- one migration-strategy run ----------------------------------------- */
static void run_migration(const nf_bench_config_t *cfg, numaflow_env_t *env, keydef_t *defs, keystate_t *st, strmap_t *map, const int *trace, size_t tlen, const char *name, run_result_t *out) {
    memset(out, 0, sizeof(*out));
    place_fixed(st, cfg->keys, env->node_count > 1 ? 1 : 0); /* cold start on CXL */
    reset_env_accounting(env, st, cfg->keys);

    nf_tracker_t tr; nf_tracker_init(&tr, env->node_count, 4096, 100000);
    nf_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.topo = env->nodes; ctx.topo_count = env->node_count;
    ctx.env = env; ctx.tracker = &tr; ctx.budget = cfg->budget;
    ctx.rng = nf_rng_seed(cfg->seed);

    nf_graph_t g; nf_graph_init(&g);
    int have_graph = (strcmp(name, "noop") != 0);
    if (have_graph) nf_strategy_build(&g, name);
    nf_exec_t ex; nf_exec_init(&ex);

    int local = (int)env->local_cpu_node;
    uint64_t tick = 0;
    size_t epochs = tlen / cfg->epoch; if (epochs == 0) epochs = 1;
    for (size_t e = 0; e < epochs; e++) {
        size_t base = e * cfg->epoch;
        size_t end = base + cfg->epoch; if (end > tlen) end = tlen;
        for (size_t a = base; a < end; a++) {
            int ki = trace[a]; keystate_t *k = &st[ki];
            int is_local = (k->node == local);
            double cost = nf_numa_access_cost(env, local, k->node, k->size);
            out->access_cost += cost;
            out->local_hit_ratio += is_local ? 1.0 : 0.0;
            k->acc++; k->rec = ++tick;
            nf_tracker_observe(&tr, k->key);
            nf_tracker_record_access(&tr, k->node, is_local, cost);
        }
        if (have_graph) {
            nf_items_t snapshot; nf_items_init(&snapshot);
            for (size_t i = 0; i < cfg->keys; i++) {
                nf_item_t it; memset(&it, 0, sizeof(it));
                strncpy(it.key, defs[i].key, NF_KEY_MAX - 1);
                it.value_size = defs[i].size; it.current_node = st[i].node;
                it.access_count = st[i].acc; it.recency = st[i].rec;
                nf_items_push(&snapshot, &it);
            }
            ctx.tick = tick;
            ctx.stats.migrations_done = 0; ctx.stats.migrations_skipped = 0; ctx.stats.total_cost_ns = 0;
            nf_exec_free(&ex); nf_exec_init(&ex);
            if (nf_exec_run(&ex, &g, &snapshot, &ctx) == NF_OK) {
                out->migrations += ctx.stats.migrations_done;
                out->migration_cost += ctx.stats.total_cost_ns;
                for (size_t i = 0; i < ex.result.count; i++) {
                    int ki = sm_get(map, ex.result.items[i].key);
                    if (ki >= 0) st[ki].node = ex.result.items[i].current_node;
                }
            }
            nf_items_free(&snapshot);
        }
    }
    out->local_hit_ratio /= (tlen > 0 ? (double)tlen : 1.0);
    out->net_cost = out->access_cost + out->migration_cost;
    nf_tracker_update_feedback(&tr);
    out->feedback = nf_tracker_get_score(&tr);
    for (size_t i = 0; i < cfg->keys; i++) if (st[i].node >= 0 && st[i].node < NF_MAX_NODES) out->node_bytes[st[i].node] += st[i].size;

    nf_exec_free(&ex); nf_graph_free(&g); nf_tracker_free(&tr);
}

/* ---- one allocation-strategy run ---------------------------------------- */
static void run_allocation(const nf_bench_config_t *cfg, numaflow_env_t *env, keydef_t *defs, keystate_t *st, const int *trace, size_t tlen, const char *name, run_result_t *out) {
    memset(out, 0, sizeof(*out));
    nf_tracker_t tr; nf_tracker_init(&tr, env->node_count, 4096, 100000);
    nf_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.topo = env->nodes; ctx.topo_count = env->node_count; ctx.env = env; ctx.tracker = &tr;
    ctx.rng = nf_rng_seed(cfg->seed);
    place_alloc(cfg, env, &ctx, defs, st, cfg->keys, name);
    reset_env_accounting(env, st, cfg->keys);

    int local = (int)env->local_cpu_node;
    for (size_t a = 0; a < tlen; a++) {
        int ki = trace[a]; keystate_t *k = &st[ki];
        int is_local = (k->node == local);
        double cost = nf_numa_access_cost(env, local, k->node, k->size);
        out->access_cost += cost;
        out->local_hit_ratio += is_local ? 1.0 : 0.0;
        nf_tracker_record_access(&tr, k->node, is_local, cost);
    }
    out->local_hit_ratio /= (tlen > 0 ? (double)tlen : 1.0);
    out->net_cost = out->access_cost;
    nf_tracker_update_feedback(&tr);
    out->feedback = nf_tracker_get_score(&tr);
    for (size_t i = 0; i < cfg->keys; i++) if (st[i].node >= 0 && st[i].node < NF_MAX_NODES) out->node_bytes[st[i].node] += st[i].size;
    nf_tracker_free(&tr);
}

/* ---- result JSON helpers ------------------------------------------------ */
static nf_json_t *result_to_json(const char *name, const run_result_t *r, int nodes) {
    nf_json_t *o = nf_json_new_obj();
    nf_json_obj_set(o, "strategy", nf_json_new_str(name));
    nf_json_obj_set(o, "local_hit_ratio", nf_json_new_num(r->local_hit_ratio));
    nf_json_obj_set(o, "access_cost", nf_json_new_num(r->access_cost));
    nf_json_obj_set(o, "migration_cost", nf_json_new_num(r->migration_cost));
    nf_json_obj_set(o, "net_cost", nf_json_new_num(r->net_cost));
    nf_json_obj_set(o, "migrations", nf_json_new_num((double)r->migrations));
    nf_json_obj_set(o, "feedback", nf_json_new_num(r->feedback));
    nf_json_t *nb = nf_json_new_arr();
    for (int i = 0; i < nodes; i++) nf_json_arr_push(nb, nf_json_new_num((double)r->node_bytes[i]));
    nf_json_obj_set(o, "node_bytes", nb);
    return o;
}

nf_json_t *nf_bench_run(const nf_bench_config_t *cfg_in) {
    nf_bench_config_t cfg = *cfg_in;
    nf_bench_config_defaults(&cfg);
    nf_ops_register_all();

    numaflow_env_t env; nf_numa_env_init(&env);
    nf_numa_configure_default(&env, cfg.nodes);
    if (cfg.cxl_latency_ns > 0.0 && env.node_count > 1) {
        env.nodes[1].latency_ns = cfg.cxl_latency_ns;
    }
    if (cfg.cxl_bandwidth_mbps > 0.0 && env.node_count > 1) {
        env.nodes[1].bandwidth_mbps = cfg.cxl_bandwidth_mbps;
    }

    keydef_t *defs = (keydef_t *)calloc(cfg.keys, sizeof(keydef_t));
    nf_rng_t rng = nf_rng_seed(cfg.seed);
    strmap_t map; sm_init(&map, cfg.keys);
    for (size_t i = 0; i < cfg.keys; i++) {
        snprintf(defs[i].key, NF_KEY_MAX, "k:%lu", (unsigned long)i);
        defs[i].size = pick_size(&rng);
        sm_put(&map, defs[i].key, (int)i);
    }

    /* Constrain the DRAM tier to ~50% of the total data so strategies must
     * actually rank candidates rather than just promote everything. */
    {
        uint64_t total = 0;
        for (size_t i = 0; i < cfg.keys; i++) total += defs[i].size;
        if (env.node_count > 0) {
            uint64_t dram = total / 2;
            if (dram < 4096) dram = 4096;
            env.nodes[0].total_bytes = dram;
        }
    }

    int *trace = NULL; size_t tlen = 0;
    gen_trace(&cfg, cfg.keys, &trace, &tlen);

    keystate_t *st = (keystate_t *)calloc(cfg.keys, sizeof(keystate_t));
    for (size_t i = 0; i < cfg.keys; i++) { strncpy(st[i].key, defs[i].key, NF_KEY_MAX - 1); st[i].size = defs[i].size; }

    nf_json_t *root = nf_json_new_obj();
    nf_json_t *cj = nf_json_new_obj();
    nf_json_obj_set(cj, "workload", nf_json_new_str(cfg.workload));
    nf_json_obj_set(cj, "keys", nf_json_new_num((double)cfg.keys));
    nf_json_obj_set(cj, "accesses", nf_json_new_num((double)cfg.accesses));
    nf_json_obj_set(cj, "epoch", nf_json_new_num((double)cfg.epoch));
    nf_json_obj_set(cj, "budget", nf_json_new_num((double)cfg.budget));
    nf_json_obj_set(cj, "nodes", nf_json_new_num((double)cfg.nodes));
    nf_json_obj_set(cj, "seed", nf_json_new_num((double)cfg.seed));
    nf_json_obj_set(root, "config", cj);

    nf_json_t *tj = nf_json_new_arr();
    for (int i = 0; i < env.node_count; i++) {
        nf_json_t *n = nf_json_new_obj();
        nf_json_obj_set(n, "name", nf_json_new_str(env.nodes[i].name));
        nf_json_obj_set(n, "latency_ns", nf_json_new_num(env.nodes[i].latency_ns));
        nf_json_obj_set(n, "bandwidth_mbps", nf_json_new_num(env.nodes[i].bandwidth_mbps));
        nf_json_arr_push(tj, n);
    }
    nf_json_obj_set(root, "topology", tj);

    static const char *mig[] = { "noop", "composite_lru", "tinylfu", "caat" };
    nf_json_t *mj = nf_json_new_arr();
    for (size_t i = 0; i < sizeof(mig)/sizeof(mig[0]); i++) {
        run_result_t r;
        run_migration(&cfg, &env, defs, st, &map, trace, tlen, mig[i], &r);
        nf_json_arr_push(mj, result_to_json(mig[i], &r, env.node_count));
    }
    nf_json_obj_set(root, "migration", mj);

    nf_json_t *aj = nf_json_new_arr();
    for (int i = 0; i < nf_strategy_count(); i++) {
        const char *name = nf_strategy_name(i);
        if (!nf_strategy_is_allocation(name)) continue;
        run_result_t r;
        run_allocation(&cfg, &env, defs, st, trace, tlen, name, &r);
        nf_json_arr_push(aj, result_to_json(name, &r, env.node_count));
    }
    nf_json_obj_set(root, "allocation", aj);

    free(st); free(trace); free(defs); sm_free(&map); nf_numa_env_destroy(&env);
    return root;
}
