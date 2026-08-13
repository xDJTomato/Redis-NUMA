/* test_all.c - comprehensive NUMAflow unit test suite. */
#include "nf_common.h"
#include "nf_json.h"
#include "nf_graph.h"
#include "nf_ops.h"
#include "nf_exec.h"
#include "nf_track.h"
#include "nf_strategy.h"
#include "numa_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int g_fail = 0;
static int g_checks = 0;
#define CHECK(c, m) do { g_checks++; if (!(c)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, m); g_fail++; } } while (0)

static void test_json(void) {
    const char *err = NULL;
    nf_json_t *v = nf_json_parse("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":2.5}}", &err);
    CHECK(v != NULL, "parse object");
    if (!v) return;
    CHECK(nf_json_obj_get_num(v, "a", 0) == 1.0, "obj num");
    nf_json_t *b = nf_json_obj_get(v, "b");
    CHECK(b && b->type == NF_JSON_ARR && nf_json_arr_len(b) == 3, "arr len");
    CHECK(nf_json_bool(nf_json_arr_get(b, 0)) == true, "arr bool");
    CHECK(nf_json_arr_get(b, 1)->type == NF_JSON_NULL, "arr null");
    CHECK(strcmp(nf_json_str(nf_json_arr_get(b, 2)), "x") == 0, "arr str");
    char *s = nf_json_serialize(v);
    nf_json_t *v2 = nf_json_parse(s, &err);
    CHECK(v2 != NULL, "roundtrip parse");
    CHECK(nf_json_obj_get_num(v2, "a", 0) == 1.0, "roundtrip value");
    free(s); nf_json_free(v2); nf_json_free(v);
}

static void test_graph(void) {
    nf_graph_t g; nf_graph_init(&g);
    CHECK(nf_graph_add_node(&g, "a", "op1") == NF_OK, "add a");
    CHECK(nf_graph_add_node(&g, "b", "op2") == NF_OK, "add b");
    CHECK(nf_graph_add_node(&g, "a", "op3") == NF_EEXIST, "dup id");
    CHECK(nf_graph_add_edge(&g, "a", "b") == NF_OK, "add edge");
    CHECK(nf_graph_add_edge(&g, "b", "c") == NF_ENOENT, "edge to missing");
    int order[8];
    CHECK(nf_graph_topo_sort(&g, order) == NF_OK, "topo ok");
    CHECK(order[0] == 0 && order[1] == 1, "topo order a,b");
    /* cycle */
    nf_graph_add_edge(&g, "b", "a");
    CHECK(nf_graph_topo_sort(&g, order) == NF_ECYCLE, "cycle detected");
    nf_graph_free(&g);
}

static void test_tracker(void) {
    nf_tracker_t t; nf_tracker_init(&t, 2, 64, 1000);
    CHECK(nf_tracker_freq(&t, "k1") == 0, "initial freq 0");
    CHECK(nf_tracker_observe(&t, "k1") == 0, "first observe filtered by doorkeeper");
    CHECK(nf_tracker_freq(&t, "k1") == 0, "still 0 after first observe");
    CHECK(nf_tracker_observe(&t, "k1") == 1, "second observe counted");
    CHECK(nf_tracker_freq(&t, "k1") == 1, "freq 1 after second observe");
    for (int i = 0; i < 10; i++) nf_tracker_observe(&t, "k1");
    CHECK(nf_tracker_freq(&t, "k1") >= 2, "freq grew");
    uint32_t before = nf_tracker_freq(&t, "k1");
    nf_tracker_decay(&t);
    CHECK(nf_tracker_freq(&t, "k1") == before / 2, "decay halves");
    nf_tracker_free(&t);
}

static void test_shim(void) {
    numaflow_env_t e; nf_numa_env_init(&e);
    CHECK(nf_numa_configure_default(&e, 2) == NF_OK, "configure 2 nodes");
    CHECK(e.node_count == 2, "2 nodes");
    double local = nf_numa_access_cost(&e, 0, 0, 64);
    double remote = nf_numa_access_cost(&e, 0, 1, 64);
    CHECK(remote > local, "remote cost > local cost");
    CHECK(nf_numa_migrate_cost(&e, 1, 0, 64) > 0, "migrate cost positive");
    void *p = nf_numa_alloc_onnode(&e, 128, 1);
    CHECK(p != NULL, "alloc onnode");
    CHECK(nf_numa_node_of_addr(&e, p) == 1, "node of addr");
    nf_numa_free(&e, p);
    nf_numa_env_destroy(&e);
}

static void test_ops(void) {
    nf_ops_register_all();
    CHECK(nf_ops_count() >= 34, "ops registered");
    CHECK(nf_ops_find("emit_migrate") != NULL, "find emit");
    CHECK(nf_ops_find("filter_hot") != NULL, "find filter_hot");
    CHECK(nf_ops_find("demote_cold") != NULL, "find demote_cold");
    CHECK(nf_ops_find("nope") == NULL, "missing op null");
}

static void test_exec(void) {
    nf_ops_register_all();
    nf_graph_t g; nf_graph_init(&g);
    CHECK(nf_strategy_build(&g, "composite_lru") == NF_OK, "build composite");
    int order[16];
    CHECK(nf_graph_topo_sort(&g, order) == NF_OK, "composite topo");
    char *js = nf_graph_to_json(&g);
    CHECK(js != NULL && strstr(js, "filter_hot") != NULL, "composite json has filter_hot");
    free(js); nf_graph_free(&g);

    /* run a tiny promotion */
    nf_graph_t g2; nf_graph_init(&g2);
    CHECK(nf_strategy_build(&g2, "tinylfu") == NF_OK, "build tinylfu");
    numaflow_env_t env; nf_numa_env_init(&env); nf_numa_configure_default(&env, 2);
    nf_tracker_t tr; nf_tracker_init(&tr, 2, 64, 100000);
    for (int i = 0; i < 20; i++) nf_tracker_observe(&tr, "hot");
    nf_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.topo = env.nodes; ctx.topo_count = env.node_count; ctx.env = &env; ctx.tracker = &tr; ctx.budget = 8; ctx.tick = 1; ctx.rng = nf_rng_seed(1);
    nf_items_t init; nf_items_init(&init);
    nf_item_t it; memset(&it, 0, sizeof(it)); strcpy(it.key, "hot"); it.value_size = 256; it.current_node = 1; nf_items_push(&init, &it);
    nf_exec_t ex; nf_exec_init(&ex);
    CHECK(nf_exec_run(&ex, &g2, &init, &ctx) == NF_OK, "tinylfu run");
    CHECK(ctx.stats.migrations_done == 1, "tinylfu promoted hot key");
    nf_exec_free(&ex); nf_items_free(&init); nf_tracker_free(&tr); nf_numa_env_destroy(&env); nf_graph_free(&g2);
}

int main(void) {
    test_json();
    test_graph();
    test_tracker();
    test_shim();
    test_ops();
    test_exec();
    printf("%d checks, %d failures\n", g_checks, g_fail);
    if (g_fail == 0) { printf("ALL TESTS PASSED\n"); return 0; }
    return 1;
}
