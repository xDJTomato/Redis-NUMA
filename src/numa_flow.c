/* =============================================================================
 * numa_flow.c - Redis adapter: run NUMAflow DAG workflows in-process.
 *
 * Thin bridge between the pure-C11 NUMAflow engine and the Redis NUMA runtime.
 * NOTE: this file compiles only under #ifdef HAVE_NUMA (Linux + libnuma); the
 * NUMAflow engine itself is platform-independent and tested separately.
 * ========================================================================== */
#include "server.h"
#include "numa_key_migrate.h"
#include "numa_flow.h"

#ifdef HAVE_NUMA

#include "nf_bridge.h"
#include "nf_adapt.h"
#include "nf_strategy.h"
#include "nf_track.h"
#include "numa_shim.h"

#define NUMA_FLOW_MAX_LOADED 16
#define NUMA_FLOW_DEFAULT_NAME "default"

/* ---- per-workflow runtime state ----------------------------------------- */
typedef struct {
    char            name[NF_STR_MAX];
    nf_graph_t      graph;
    int             enabled;
    int             interval_sec;      /* 0 = manual only */
    long long       last_run_ms;
    uint64_t        runs;
    int             adapt_enabled;
    nf_adapt_t      adapt;
    nf_adapt_mode_t mode;
    nf_bridge_result_t last;
} numa_flow_entry_t;

/* ---- bridge global state ------------------------------------------------ */
static numaflow_env_t g_env;
static nf_node_t       g_topo[NF_MAX_NODES];
static nf_tracker_t    g_tracker;
static numa_flow_entry_t g_entries[NUMA_FLOW_MAX_LOADED];
static int g_entry_count = 0;
static int g_initialized = 0;
static redisDb *g_db = NULL;

/* ---- bridge callbacks (implement nf_bridge_t) --------------------------- */
typedef struct { redisDb *db; dictIterator *iter; } numa_flow_iter_t;

static int numa_flow_enumerate(void *ud, nf_item_t *out) {
    numa_flow_iter_t *it = (numa_flow_iter_t *)ud;
    dictEntry *de = dictNext(it->iter);
    if (!de) return 1;   /* end of keyspace */
    /* Redis db dict keys are SDS strings, not robj objects.  Treat them as
     * SDS directly and derive NUMA telemetry from the value allocation, which
     * is the only pointer with a NUMA prefix in this path. */
    sds ks = (sds)dictGetKey(de);
    robj *val = (robj *)dictGetVal(de);
    size_t klen = sdslen(ks);
    if (klen >= NF_KEY_MAX) klen = NF_KEY_MAX - 1;
    memcpy(out->key, ks, klen);
    out->key[klen] = '\0';
    out->value = val;
    out->value_size = numa_object_sample_alloc_size(val);

    void *sample = numa_object_sample_alloc_ptr(val);
    if (!sample) sample = val;
    out->current_node = sample ? numa_get_node_id(sample) : -1;
    out->access_count = sample ? numa_get_access_count(sample) : 0;
    out->hotness = sample ? numa_get_hotness(sample) : 0;
    out->recency = sample ? numa_get_last_access(sample) : 0;
    return 0;
}

static int numa_flow_apply(void *ud, const char *key, int target) {
    numa_flow_iter_t *it = (numa_flow_iter_t *)ud;
    return (numa_migrate_key_by_name(it->db, key, target) == NUMA_KEY_MIGRATE_OK) ? 0 : -1;
}

/* ---- run one loaded workflow -------------------------------------------- */
static int numa_flow_run_entry(numa_flow_entry_t *e) {
    numa_flow_iter_t iter;
    iter.db = g_db;
    iter.iter = dictGetSafeIterator(g_db->dict);

    nf_bridge_t br; memset(&br, 0, sizeof(br));
    br.enumerate = numa_flow_enumerate;
    br.apply = numa_flow_apply;
    br.ud = &iter;
    br.ctx.topo = g_topo;
    br.ctx.topo_count = g_env.node_count;
    br.ctx.env = &g_env;
    br.ctx.tracker = &g_tracker;
    br.ctx.budget = 256;
    br.ctx.rng = nf_rng_seed(e->runs + 1);
    br.ctx.tick = (uint64_t)server.lruclock;
    br.tracker = &g_tracker;

    int rc = nf_bridge_run(&br, &e->graph, &e->last);
    dictReleaseIterator(iter.iter);
    e->runs++;

    /* self-adaptation: fold feedback, possibly rebuild the DAG structure */
    if (rc == NF_OK && e->adapt_enabled) {
        nf_adapt_mode_t mode = nf_adapt_tune(&e->adapt, e->last.feedback,
                                             e->last.migrations, e->last.enumerated);
        if (mode != e->mode) {
            e->mode = mode;
            nf_adapt_build_graph(&e->adapt, &e->graph);
            nf_adapt_apply_params(&e->adapt, &e->graph);
        } else {
            nf_adapt_apply_params(&e->adapt, &e->graph);
        }
    }
    return rc;
}

static numa_flow_entry_t *numa_flow_find(const char *name) {
    for (int i = 0; i < g_entry_count; i++)
        if (strcasecmp(g_entries[i].name, name) == 0) return &g_entries[i];
    return NULL;
}

void numa_flow_observe_access(const char *key) {
    if (!g_initialized || !key) return;
    nf_tracker_observe(&g_tracker, key);
}

/* ---- default-strategy entry management ---------------------------------- */

int numa_flow_load_default(const char *strategy_name, int interval_sec) {
    if (numa_flow_find(NUMA_FLOW_DEFAULT_NAME)) return C_ERR;
    if (g_entry_count >= NUMA_FLOW_MAX_LOADED) return C_ERR;

    numa_flow_entry_t *e = &g_entries[g_entry_count];
    memset(e, 0, sizeof(*e));
    strncpy(e->name, NUMA_FLOW_DEFAULT_NAME, NF_STR_MAX - 1);
    nf_graph_init(&e->graph);
    if (nf_strategy_build(&e->graph, strategy_name) != NF_OK) {
        _serverLog(LL_WARNING, "[NUMA Flow] unknown default strategy '%s'", strategy_name);
        return C_ERR;
    }
    e->enabled = 1;
    e->interval_sec = interval_sec > 0 ? interval_sec : 1;
    e->mode = NF_ADAPT_BALANCED;
    g_entry_count++;
    _serverLog(LL_NOTICE, "[NUMA Flow] default strategy '%s' auto-loaded (interval=%ds)",
               strategy_name, e->interval_sec);
    return C_OK;
}

int numa_flow_set_default(const char *strategy_name) {
    numa_flow_entry_t *e = numa_flow_find(NUMA_FLOW_DEFAULT_NAME);
    if (!e) return numa_flow_load_default(strategy_name, 1);
    if (nf_strategy_build(&e->graph, strategy_name) != NF_OK) return C_ERR;
    _serverLog(LL_NOTICE, "[NUMA Flow] default strategy switched to '%s'", strategy_name);
    return C_OK;
}

int numa_flow_run_default(uint64_t *scanned, uint64_t *migrated) {
    numa_flow_entry_t *e = numa_flow_find(NUMA_FLOW_DEFAULT_NAME);
    if (!e) return C_ERR;
    numa_flow_run_entry(e);
    if (scanned) *scanned = e->last.enumerated;
    if (migrated) *migrated = e->last.migrations;
    return C_OK;
}

/* ---- NUMA FLOW command -------------------------------------------------- */
void numa_flow_command(client *c) {
    if (c->argc < 3) {
        addReplyError(c, "Usage: NUMA FLOW <LOAD|RUN|LIST|STATUS|UNLOAD|ADAPT|DEFAULT> ...");
        return;
    }
    const char *sub = (const char *)c->argv[2]->ptr;

    if (!strcasecmp(sub, "LOAD")) {
        if (c->argc < 5) {
            addReplyError(c, "Usage: NUMA FLOW LOAD <name> <path> [interval_sec] [ADAPT]");
            return;
        }
        const char *name = (const char *)c->argv[3]->ptr;
        const char *path = (const char *)c->argv[4]->ptr;
        if (g_entry_count >= NUMA_FLOW_MAX_LOADED) { addReplyError(c, "too many workflows loaded"); return; }
        if (numa_flow_find(name)) { addReplyError(c, "workflow name already exists"); return; }

        numa_flow_entry_t *e = &g_entries[g_entry_count];
        memset(e, 0, sizeof(*e));
        strncpy(e->name, name, NF_STR_MAX - 1);
        nf_graph_init(&e->graph);
        char err[NF_STR_MAX];
        if (nf_graph_load_file(&e->graph, path, err, sizeof(err)) != NF_OK) {
            addReplyErrorFormat(c, "cannot load workflow: %s", err);
            return;
        }
        e->interval_sec = 0;
        int adapt = 0;
        for (int i = 5; i < c->argc; i++) {
            const char *a = (const char *)c->argv[i]->ptr;
            if (!strcasecmp(a, "ADAPT")) adapt = 1;
            else e->interval_sec = atoi(a);
        }
        e->enabled = 1;
        e->adapt_enabled = adapt;
        e->mode = NF_ADAPT_BALANCED;
        if (adapt) {
            nf_adapt_init(&e->adapt);
            nf_adapt_add_param(&e->adapt, "filter_benefit", "threshold", 0.0, 0.0, 2.0, 0.5, 1);
            nf_adapt_add_param(&e->adapt, "demote_cold", "threshold", 1.0, 0.0, 3.0, 1.0, 0);
            nf_adapt_add_param(&e->adapt, "budget_limit", "budget", 256.0, 64.0, 1024.0, 64.0, 0);
        }
        g_entry_count++;
        addReplyStatus(c, "OK");
        return;
    }

    if (!strcasecmp(sub, "RUN")) {
        if (c->argc >= 4) {
            numa_flow_entry_t *e = numa_flow_find((const char *)c->argv[3]->ptr);
            if (!e) { addReplyError(c, "workflow not found"); return; }
            numa_flow_run_entry(e);
            addReplyStatus(c, "OK");
            return;
        }
        int ran = 0;
        for (int i = 0; i < g_entry_count; i++) if (g_entries[i].enabled) { numa_flow_run_entry(&g_entries[i]); ran++; }
        addReplyLongLong(c, ran);
        return;
    }

    if (!strcasecmp(sub, "LIST")) {
        addReplyArrayLen(c, g_entry_count);
        for (int i = 0; i < g_entry_count; i++) {
            addReplyArrayLen(c, 4);
            addReplyBulkCString(c, g_entries[i].name);
            addReplyLongLong(c, g_entries[i].enabled);
            addReplyLongLong(c, g_entries[i].interval_sec);
            addReplyLongLong(c, (long long)g_entries[i].runs);
        }
        return;
    }

    if (!strcasecmp(sub, "STATUS")) {
        if (c->argc < 4) { addReplyError(c, "Usage: NUMA FLOW STATUS <name>"); return; }
        numa_flow_entry_t *e = numa_flow_find((const char *)c->argv[3]->ptr);
        if (!e) { addReplyError(c, "workflow not found"); return; }
        addReplyArrayLen(c, 6);
        addReplyBulkCString(c, e->name);
        addReplyBulkCString(c, nf_adapt_mode_name(e->mode));
        addReplyLongLong(c, e->last.enumerated);
        addReplyLongLong(c, e->last.migrations);
        addReplyLongLong(c, e->last.applied);
        addReplyDouble(c, e->last.feedback);
        return;
    }

    if (!strcasecmp(sub, "UNLOAD")) {
        if (c->argc < 4) { addReplyError(c, "Usage: NUMA FLOW UNLOAD <name>"); return; }
        for (int i = 0; i < g_entry_count; i++) {
            if (strcasecmp(g_entries[i].name, (const char *)c->argv[3]->ptr) == 0) {
                nf_graph_free(&g_entries[i].graph);
                memmove(&g_entries[i], &g_entries[i + 1],
                        (size_t)(g_entry_count - i - 1) * sizeof(numa_flow_entry_t));
                g_entry_count--;
                addReplyStatus(c, "OK");
                return;
            }
        }
        addReplyError(c, "workflow not found");
        return;
    }

    if (!strcasecmp(sub, "ADAPT")) {
        if (c->argc < 5) { addReplyError(c, "Usage: NUMA FLOW ADAPT <name> <ON|OFF>"); return; }
        numa_flow_entry_t *e = numa_flow_find((const char *)c->argv[3]->ptr);
        if (!e) { addReplyError(c, "workflow not found"); return; }
        e->adapt_enabled = !strcasecmp((const char *)c->argv[4]->ptr, "ON");
        addReplyStatus(c, "OK");
        return;
    }

    if (!strcasecmp(sub, "DEFAULT")) {
        if (c->argc < 4) { addReplyError(c, "Usage: NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>"); return; }
        if (numa_flow_set_default((const char *)c->argv[3]->ptr) != C_OK) {
            addReplyErrorFormat(c, "Unknown strategy preset: %s", (const char *)c->argv[3]->ptr);
            return;
        }
        addReplyStatus(c, "OK");
        return;
    }

    addReplyError(c, "unknown FLOW subcommand");
}

/* ---- lifecycle ----------------------------------------------------------- */
int numa_flow_init(void) {
    if (g_initialized) return C_OK;
    nf_ops_register_all();

    int num_nodes = numa_max_node() + 1;
    if (num_nodes < 1) num_nodes = 1;
    if (num_nodes > NF_MAX_NODES) num_nodes = NF_MAX_NODES;
    nf_numa_configure_default(&g_env, num_nodes);
    for (int i = 0; i < num_nodes; i++) {
        g_env.nodes[i].distance = (double)numa_distance(0, i);
        g_topo[i] = g_env.nodes[i];
    }
    nf_tracker_init(&g_tracker, num_nodes, 4096, 100000);

    g_db = &server.db[0];
    g_entry_count = 0;
    g_initialized = 1;
    _serverLog(LL_NOTICE, "[NUMA Flow] bridge initialized (%d nodes)", num_nodes);
    return C_OK;
}

void numa_flow_cleanup(void) {
    if (!g_initialized) return;
    for (int i = 0; i < g_entry_count; i++) nf_graph_free(&g_entries[i].graph);
    nf_tracker_free(&g_tracker);
    nf_numa_env_destroy(&g_env);
    g_entry_count = 0;
    g_initialized = 0;
}

void numa_flow_cron(void) {
    if (!g_initialized) return;
    long long now = mstime();
    for (int i = 0; i < g_entry_count; i++) {
        numa_flow_entry_t *e = &g_entries[i];
        if (!e->enabled || e->interval_sec <= 0) continue;
        if (now - e->last_run_ms < (long long)e->interval_sec * 1000) continue;
        e->last_run_ms = now;
        numa_flow_run_entry(e);
    }
}

#endif /* HAVE_NUMA */
