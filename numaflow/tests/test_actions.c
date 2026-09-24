/* Public mode-based actions must remain equivalent to their legacy policies. */
#include "nf_ops.h"
#include "nf_track.h"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(test, message) do { if (!(test)) { fprintf(stderr, "FAIL: %s\n", message); failures++; } } while (0)
typedef struct { const char *action, *mode, *legacy; } case_t;
static const case_t cases[] = {
    {"place_items","local","alloc_local_first"}, {"place_items","interleave","alloc_interleave"},
    {"place_items","round_robin","alloc_round_robin"}, {"place_items","weighted","alloc_weighted"},
    {"place_items","pressure","alloc_pressure_aware"}, {"place_items","cxl","alloc_cxl_optimized"},
    {"place_items","weighted_pressure","alloc_weighted_interleave"}, {"place_items","adaptive","alloc_adaptive"},
    {"place_items","latency","alloc_latency_aware"},
    {"score_items","hotness","score_hotness"}, {"score_items","frequency","cms_estimate"},
    {"score_items","blend","score_ewma"}, {"score_items","benefit","score_cost_benefit"},
    {"score_items","decay_hotness","decay_hotness"},
    {"filter_items","hot","filter_hot"}, {"filter_items","frequent","filter_freq"},
    {"filter_items","cold","filter_cold"}, {"filter_items","remote","filter_remote"},
    {"filter_items","local","filter_local"}, {"filter_items","size_min","filter_size_min"},
    {"filter_items","size_max","filter_size_max"}, {"filter_items","benefit","filter_benefit"},
    {"rank_items","recent","rank_lru"}, {"rank_items","frequency","rank_frequency"},
    {"rank_items","hotness","rank_hotness"}, {"rank_items","benefit","rank_cost"},
    {"rank_items","blend","rank_ewma"}, {"rank_items","size","rank_size"},
    {"route_items","destination","select_dest_node"}, {"route_items","budget","budget_limit"},
    {"move_items","migrate","emit_migrate"}, {"move_items","demote","demote_cold"},
    {"move_items","balance","balance_nodes"},
    {"track_items","access","track_access"}, {"track_items","observe","cms_observe"},
    {"track_items","decay","global_decay"}
};

static int execute(const char *name, const char *mode, const char *next, nf_items_t *out) {
    numaflow_env_t env; nf_numa_env_init(&env); nf_numa_configure_default(&env, 2);
    nf_tracker_t tracker; nf_tracker_init(&tracker, 2, 64, 10000);
    nf_params_t params; nf_params_init(&params); if (mode) nf_params_set(&params, "mode", mode);
    nf_ctx_t ctx = {0}; ctx.topo=env.nodes; ctx.topo_count=env.node_count;
    ctx.env=&env; ctx.tracker=&tracker; ctx.params=&params; ctx.tick=30;
    ctx.budget=2; ctx.rng=nf_rng_seed(42);
    nf_items_t in; nf_items_init(&in);
    for (int i=0;i<3;i++) {
        nf_item_t item={0}; snprintf(item.key, sizeof(item.key), "key%d", i);
        item.value_size=(size_t)(256+i*2048); item.current_node=1;
        item.access_count=10-i*4; item.recency=25-i*10;
        item.hotness=5-i*2; item.freq_est=4-i; item.ewma=0.7-i*0.2;
        item.cost_benefit=100-i*80; item.selected_node=0; item.migrate=1;
        nf_items_push(&in,&item);
    }
    const nf_op_t *op=nf_ops_find(name);
    int rc;
    if (next) {
        nf_items_t marked; nf_items_init(&marked);
        rc=op?op->run(op,&ctx,&in,&marked):NF_ENOENT;
        const nf_op_t *apply=nf_ops_find(next);
        if (rc==NF_OK) rc=apply?apply->run(apply,&ctx,&marked,out):NF_ENOENT;
        nf_items_free(&marked);
    } else rc=op?op->run(op,&ctx,&in,out):NF_ENOENT;
    nf_items_free(&in);nf_params_free(&params);nf_tracker_free(&tracker);nf_numa_env_destroy(&env);
    return rc;
}
int main(void) {
    nf_ops_register_all();
    for (size_t i=0;i<sizeof(cases)/sizeof(cases[0]);i++) {
        nf_items_t actual, expected; nf_items_init(&actual); nf_items_init(&expected);
        int a=execute(cases[i].action,cases[i].mode,NULL,&actual);
        const char *apply=(strcmp(cases[i].action,"move_items")==0 &&
                            strcmp(cases[i].mode,"migrate")!=0) ? "emit_migrate" : NULL;
        int b=execute(cases[i].legacy,NULL,apply,&expected);
        if (a!=b || actual.count!=expected.count ||
            (a==NF_OK && memcmp(actual.items,expected.items,actual.count*sizeof(nf_item_t))!=0)) {
            fprintf(stderr,"Mismatch: %s mode %s vs %s\n",cases[i].action,cases[i].mode,cases[i].legacy);failures++;
        }
        nf_items_free(&actual);nf_items_free(&expected);
    }
    nf_items_t out;nf_items_init(&out);
    CHECK(execute("filter_items","not-a-mode",NULL,&out)==NF_EINVAL,"invalid mode rejected");
    nf_items_free(&out);
    if(failures)return 1;
    printf("ALL 36 ACTION MODES MATCH LEGACY POLICIES/PIPELINES\n");return 0;
}
