/* nf_bridge.c - bridge between the DAG engine and a key-value store (pure C11). */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "nf_bridge.h"
#include "nf_strategy.h"

#include <stdlib.h>
#include <string.h>

/* ---- tiny string->int map (key -> original node) ------------------------ */
/* NOTE: this table auto-resizes (see kn_put) - it used to be a fixed-capacity
 * table sized once at kn_init() with no resize logic, so any caller that
 * enumerated more items than that initial guess would fill every slot and
 * kn_put's linear-probe loop (`while (m->keys[i]) i++`) would spin forever
 * with no empty slot left to find. That was never hit before because
 * nothing previously enumerated more than kn_init()'s hardcoded capacity
 * through this path at once. */
typedef struct { size_t cap; size_t count; char **keys; int *vals; } kn_map_t;
static uint64_t kn_hash(const char *s) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (; *s; s++) { h ^= (uint8_t)*s; h *= UINT64_C(0x100000001b3); }
    return h;
}
static void kn_init(kn_map_t *m, size_t cap) {
    m->cap = cap * 2 + 1;
    m->count = 0;
    m->keys = (char **)calloc(m->cap, sizeof(char *));
    m->vals = (int *)calloc(m->cap, sizeof(int));
}
static void kn_free(kn_map_t *m) {
    for (size_t i = 0; i < m->cap; i++) free(m->keys[i]);
    free(m->keys); free(m->vals);
}
static void kn_grow(kn_map_t *m) {
    size_t old_cap = m->cap;
    char **old_keys = m->keys;
    int *old_vals = m->vals;
    m->cap = old_cap * 2;
    m->keys = (char **)calloc(m->cap, sizeof(char *));
    m->vals = (int *)calloc(m->cap, sizeof(int));
    for (size_t i = 0; i < old_cap; i++) {
        if (!old_keys[i]) continue;
        size_t j = kn_hash(old_keys[i]) % m->cap;
        while (m->keys[j]) j = (j + 1) % m->cap;
        m->keys[j] = old_keys[i];
        m->vals[j] = old_vals[i];
    }
    free(old_keys);
    free(old_vals);
}
static void kn_put(kn_map_t *m, const char *k, int v) {
    /* keep the load factor under ~70% so linear probing stays cheap and a
     * free slot is always guaranteed to exist */
    if ((m->count + 1) * 10 >= m->cap * 7) kn_grow(m);
    size_t i = kn_hash(k) % m->cap;
    while (m->keys[i]) i = (i + 1) % m->cap;
    m->keys[i] = strdup(k); m->vals[i] = v;
    m->count++;
}
static int kn_get(kn_map_t *m, const char *k, int *found) {
    size_t i = kn_hash(k) % m->cap;
    while (m->keys[i]) {
        if (strcmp(m->keys[i], k) == 0) { if (found) *found = 1; return m->vals[i]; }
        i = (i + 1) % m->cap;
    }
    if (found) *found = 0;
    return 0;
}

int nf_bridge_run(nf_bridge_t *br, const nf_graph_t *g, nf_bridge_result_t *out) {
    if (!br || !g || !out || !br->enumerate || !br->apply) return NF_EINVAL;
    memset(out, 0, sizeof(*out));

    /* ensure a topology if the host did not provide one */
    if (br->ctx.topo_count == 0) {
        static numaflow_env_t _env;
        static nf_node_t _topo[NF_MAX_NODES];
        nf_numa_env_init(&_env); nf_numa_configure_default(&_env, 2);
        for (int i = 0; i < _env.node_count; i++) _topo[i] = _env.nodes[i];
        br->ctx.topo = _topo; br->ctx.topo_count = _env.node_count; br->ctx.env = &_env;
    }
    if (br->ctx.budget == 0) br->ctx.budget = 256;

    /* enumerate */
    nf_items_t items; nf_items_init(&items);
    kn_map_t map; kn_init(&map, 1024);
    uint64_t total_bytes = 0, dram_bytes = 0;
    int local = (int)br->ctx.env->local_cpu_node;
    nf_item_t it;
    while (1) {
        memset(&it, 0, sizeof(it));
        int rc = br->enumerate(br->ud, &it);
        if (rc != 0) break;
        nf_items_push(&items, &it);
        kn_put(&map, it.key, it.current_node);
        total_bytes += it.value_size;
        if (it.current_node == local) dram_bytes += it.value_size;
        out->enumerated++;
    }

    /* run the DAG */
    nf_exec_t ex; nf_exec_init(&ex);
    int rc = nf_exec_run(&ex, g, &items, &br->ctx);
    if (rc != NF_OK) {
        nf_exec_free(&ex); nf_items_free(&items); kn_free(&map);
        return rc;
    }

    /* apply migration decisions: an item migrated iff its current node
     * changed (emit_migrate updates current_node = selected_node). */
    for (size_t i = 0; i < ex.result.count; i++) {
        const nf_item_t *r = &ex.result.items[i];
        int found = 0;
        int orig = kn_get(&map, r->key, &found);
        if (found && r->current_node != orig && r->current_node >= 0) {
            out->migrations++;
            /* update residency accounting */
            if (orig == local) dram_bytes -= r->value_size;
            if (r->current_node == local) dram_bytes += r->value_size;
            if (br->apply(br->ud, r->key, r->current_node) == 0) out->applied++;
            else out->failed++;
        }
    }

    /* health feedback = DRAM residency ratio (proxy for local-hit ratio) */
    out->feedback = total_bytes ? (double)dram_bytes / (double)total_bytes : 0.0;
    if (br->tracker) {
        /* fold residency into the tracker feedback loop (0..1 hit ratio) */
        br->tracker->ewma_hit_ratio = br->tracker->alpha * out->feedback +
            (1.0 - br->tracker->alpha) * br->tracker->ewma_hit_ratio;
        br->tracker->feedback_score = br->tracker->ewma_hit_ratio;
    }

    nf_exec_free(&ex);
    nf_items_free(&items);
    kn_free(&map);
    return NF_OK;
}

int nf_bridge_run_named(nf_bridge_t *br, const char *strategy, nf_bridge_result_t *out) {
    nf_graph_t g; nf_graph_init(&g);
    if (nf_strategy_build(&g, strategy) != NF_OK) { nf_graph_free(&g); return NF_ENOENT; }
    int rc = nf_bridge_run(br, &g, out);
    nf_graph_free(&g);
    return rc;
}
