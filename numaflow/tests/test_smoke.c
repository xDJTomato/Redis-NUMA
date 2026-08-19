/* test_smoke.c - end-to-end smoke test for the NUMAflow engine. */
#include "nf_ops.h"
#include "nf_exec.h"
#include "nf_strategy.h"
#include "nf_track.h"
#include "nf_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); failures++; } } while (0)

static void make_item(nf_item_t *it, const char *key, size_t size, int node, uint64_t acc, uint64_t rec) {
    memset(it, 0, sizeof(*it));
    strncpy(it->key, key, NF_KEY_MAX - 1);
    it->value_size = size;
    it->current_node = node;
    it->access_count = acc;
    it->recency = rec;
    it->hotness = 0;
    it->keep = 1;
}

int main(void) {
    nf_ops_register_all();
    CHECK(nf_ops_count() >= 30, "ops registered");
    CHECK(nf_ops_find("emit_migrate") != NULL, "find emit_migrate");
    CHECK(nf_ops_find("bogus") == NULL, "bogus op not found");

    /* strategy catalog */
    CHECK(strcmp(nf_strategy_default(), "caat") == 0, "default strategy is caat");
    CHECK(nf_strategy_count() >= 10, "strategy count");

    /* build caat and check structure: demote chain (d1-d4) fans out into
     * keep_dram (sink, items that stayed on DRAM) and off_dram -> promote
     * chain (p1-p6, sink) - see nf_strategy.c's build_caat comment for why
     * this isn't one linear chain. */
    nf_graph_t g; nf_graph_init(&g);
    CHECK(nf_strategy_build(&g, "caat") == NF_OK, "build caat");
    CHECK(g.node_count == 12, "caat has 12 nodes");
    CHECK(g.edge_count == 11, "caat has 11 edges");
    int order[64];
    CHECK(nf_graph_topo_sort(&g, order) == NF_OK, "caat topo sort");

    /* JSON round-trip */
    char *js = nf_graph_to_json(&g);
    CHECK(js != NULL, "graph to json");
    nf_graph_t g2; nf_graph_init(&g2);
    char err[256];
    CHECK(nf_graph_from_json(&g2, js, err, sizeof(err)) == NF_OK, "graph from json");
    CHECK(g2.node_count == g.node_count, "roundtrip node count");
    CHECK(g2.edge_count == g.edge_count, "roundtrip edge count");
    free(js);
    nf_graph_free(&g2);

    /* env + tracker + ctx */
    numaflow_env_t env; nf_numa_env_init(&env);
    CHECK(nf_numa_configure_default(&env, 2) == NF_OK, "configure 2 nodes");
    nf_tracker_t tr; nf_tracker_init(&tr, 2, 1024, 100000);
    nf_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.topo = env.nodes; ctx.topo_count = env.node_count;
    ctx.env = &env; ctx.tracker = &tr;
    ctx.tick = 1000; ctx.budget = 16;
    ctx.rng = nf_rng_seed(42);

    /* pre-populate frequency estimates by replaying access history (hot keys
     * are accessed many times; cold keys at most once) */
    for (int k = 0; k < 100; k++) nf_tracker_observe(&tr, "hot:key:1");
    for (int k = 0; k < 80; k++)  nf_tracker_observe(&tr, "hot:key:2");
    for (int k = 0; k < 60; k++)  nf_tracker_observe(&tr, "hot:key:3");
    nf_tracker_observe(&tr, "cold:key:1");

    /* build initial items: a few hot keys on CXL (node1) that should migrate to DRAM */
    nf_items_t initial; nf_items_init(&initial);
    nf_item_t it;
    make_item(&it, "hot:key:1", 256, 1, 100, 999);  nf_items_push(&initial, &it);
    make_item(&it, "hot:key:2", 256, 1, 80, 998);   nf_items_push(&initial, &it);
    make_item(&it, "hot:key:3", 256, 1, 60, 997);   nf_items_push(&initial, &it);
    make_item(&it, "cold:key:1", 256, 1, 1, 10);    nf_items_push(&initial, &it);
    make_item(&it, "cold:key:2", 256, 1, 0, 5);     nf_items_push(&initial, &it);

    /* run */
    nf_exec_t ex; nf_exec_init(&ex);
    int rc = nf_exec_run(&ex, &g, &initial, &ctx);
    CHECK(rc == NF_OK, "exec run");
    CHECK(ctx.stats.migrations_done > 0, "some migrations done");

    /* verify hot items moved to node 0; zero-access cold item was filtered out */
    int moved = 0, cold_present = 0;
    for (size_t i = 0; i < ex.result.count; i++) {
        if (strncmp(ex.result.items[i].key, "hot:", 4) == 0 && ex.result.items[i].current_node == 0) moved++;
        if (strcmp(ex.result.items[i].key, "cold:key:2") == 0) cold_present = 1;
    }
    CHECK(moved == 3, "all 3 hot items migrated to DRAM");
    CHECK(!cold_present, "zero-access cold item filtered out (never migrated)");
    CHECK(ex.result.count == 3, "result = 3 promoted hot candidates");

    nf_exec_free(&ex);
    nf_items_free(&initial);
    nf_tracker_free(&tr);
    nf_numa_env_destroy(&env);
    nf_graph_free(&g);

    if (failures == 0) { printf("ALL SMOKE TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", failures);
    return 1;
}
