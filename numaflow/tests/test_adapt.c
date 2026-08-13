/* test_adapt.c - tests for the bridge and self-adapting DAG engine. */
#include "nf_bridge.h"
#include "nf_adapt.h"
#include "nf_strategy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, m); g_fail++; } } while (0)

/* ---- mock key-value store ---------------------------------------------- */
typedef struct { char key[NF_KEY_MAX]; int node; size_t size; uint64_t acc; uint8_t hot; } mock_key_t;
typedef struct { mock_key_t *keys; int count; int cursor; int applied; } mock_store_t;

static int mock_enumerate(void *ud, nf_item_t *out) {
    mock_store_t *s = (mock_store_t *)ud;
    if (s->cursor >= s->count) return 1;
    mock_key_t *k = &s->keys[s->cursor++];
    strncpy(out->key, k->key, NF_KEY_MAX - 1);
    out->key[NF_KEY_MAX - 1] = '\0';
    out->value_size = k->size;
    out->current_node = k->node;
    out->access_count = k->acc;
    out->hotness = k->hot;
    out->recency = k->acc;
    return 0;
}
static int mock_apply(void *ud, const char *key, int target) {
    mock_store_t *s = (mock_store_t *)ud;
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->keys[i].key, key) == 0) { s->keys[i].node = target; s->applied++; return 0; }
    return -1;
}

static void test_bridge(void) {
    nf_ops_register_all();
    numaflow_env_t env; nf_numa_env_init(&env); nf_numa_configure_default(&env, 2);

    mock_key_t ks[] = {
        { "hot:1", 1, 256, 100, 7 },
        { "hot:2", 1, 256, 80, 7 },
        { "hot:3", 1, 256, 60, 7 },
        { "cold:1", 1, 256, 0, 0 },
    };
    mock_store_t store = { ks, 4, 0, 0 };

    nf_graph_t g; nf_graph_init(&g);
    CHECK(nf_strategy_build(&g, "caat") == NF_OK, "build caat");

    nf_bridge_t br; memset(&br, 0, sizeof(br));
    br.enumerate = mock_enumerate; br.apply = mock_apply; br.ud = &store;
    br.ctx.topo = env.nodes; br.ctx.topo_count = env.node_count; br.ctx.env = &env;
    br.ctx.budget = 16; br.ctx.rng = nf_rng_seed(1); br.ctx.tick = 1;

    nf_bridge_result_t res;
    CHECK(nf_bridge_run(&br, &g, &res) == NF_OK, "bridge run");
    CHECK(res.enumerated == 4, "enumerated 4");
    CHECK(res.migrations == 3, "3 migrations decided");
    CHECK(res.applied == 3, "3 migrations applied");
    CHECK(store.applied == 3, "store saw 3 applies");
    CHECK(res.feedback > 0.0, "feedback (DRAM residency) > 0");
    int moved = 0;
    for (int i = 0; i < 4; i++) if (ks[i].node == 0 && strncmp(ks[i].key, "hot:", 4) == 0) moved++;
    CHECK(moved == 3, "hot keys moved to DRAM (node 0)");

    nf_graph_free(&g); nf_numa_env_destroy(&env);
}

static void test_adapt(void) {
    nf_adapt_t a; nf_adapt_init(&a);
    nf_adapt_add_param(&a, "filter_benefit", "threshold", 0.0, 0.0, 2.0, 0.5, 1);
    nf_adapt_add_param(&a, "demote_cold", "threshold", 1.0, 0.0, 3.0, 1.0, 0);
    nf_adapt_add_param(&a, "budget_limit", "budget", 256.0, 64.0, 1024.0, 64.0, 0);
    CHECK(a.param_count == 3, "3 params registered");

    /* low residency + low churn -> aggressive */
    nf_adapt_mode_t m1 = nf_adapt_tune(&a, 0.30, 10, 100);
    CHECK(m1 == NF_ADAPT_AGGRESSIVE, "low residency -> aggressive");

    /* high churn + low residency -> conservative */
    nf_adapt_mode_t m2 = nf_adapt_tune(&a, 0.40, 60, 100);
    CHECK(m2 == NF_ADAPT_CONSERVATIVE, "high churn -> conservative");

    /* good residency -> balanced */
    nf_adapt_mode_t m3 = nf_adapt_tune(&a, 0.90, 5, 100);
    CHECK(m3 == NF_ADAPT_BALANCED, "good residency -> balanced");

    /* 4th round returns to param[0], which is now actually adjusted */
    nf_adapt_tune(&a, 0.95, 3, 100);
    double v0 = a.params[0].value;
    CHECK(v0 != 0.0, "param value moved from initial");

    /* build each mode and verify a valid graph */
    nf_graph_t g; nf_graph_init(&g);
    a.mode = NF_ADAPT_CONSERVATIVE; nf_adapt_build_graph(&a, &g);
    CHECK(g.node_count == 7, "conservative has 7 nodes");
    a.mode = NF_ADAPT_AGGRESSIVE; nf_adapt_build_graph(&a, &g);
    CHECK(g.node_count == 11, "aggressive has 11 nodes");
    a.mode = NF_ADAPT_BALANCED; nf_adapt_build_graph(&a, &g);
    CHECK(g.node_count == 10, "balanced (CAAT) has 10 nodes");

    /* apply_params writes tuned values back into the graph */
    a.mode = NF_ADAPT_BALANCED; nf_adapt_build_graph(&a, &g);
    nf_adapt_apply_params(&a, &g);
    const nf_gnode_t *n = nf_graph_find_node(&g, "n3");
    CHECK(n != NULL && n->params.count > 0, "tuned param applied to graph");
    nf_graph_free(&g);
}

int main(void) {
    test_bridge();
    test_adapt();
    if (g_fail == 0) { printf("ALL BRIDGE/ADAPT TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", g_fail);
    return 1;
}
