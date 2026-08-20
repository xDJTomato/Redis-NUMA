/* nf_cli.c - NUMAflow command-line interface (pure C11). */
#include "nf_ops.h"
#include "nf_exec.h"
#include "nf_strategy.h"
#include "nf_template.h"
#include "nf_bench.h"
#include "nf_track.h"
#include "nf_json.h"
#include "numa_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    printf("NUMAflow - N8N-style memory-scheduling strategy engine\n\n");
    printf("usage: numaflow <command> [options]\n\n");
    printf("commands:\n");
    printf("  ops                     list atomic operations\n");
    printf("  strategies              list built-in strategies\n");
    printf("  templates               list beginner templates (grouped)\n");
    printf("  template <name> [file]  export a template's DAG as JSON\n");
    printf("  workflow <name> <file>  export a strategy's DAG as JSON\n");
    printf("  run <file>             execute a workflow DAG (validate)\n");
    printf("  dump-ops <file>         export the op catalog as JSON (for GUI)\n");
    printf("  dump-templates <file>   export the template catalog as JSON (for GUI)\n");
    printf("  eval [opts]             run the fair benchmark, print JSON\n");
    printf("    --workload <w>  zipf|uniform|hotspot|temporal\n");
    printf("    --keys <n>       number of keys\n");
    printf("    --accesses <n>   number of accesses\n");
    printf("    --epoch <n>      accesses per decision window\n");
    printf("    --budget <n>     migration budget\n");
    printf("    --nodes <n>      NUMA node count\n");
    printf("    --seed <n>       PRNG seed\n");
    printf("    --cxl-latency-ns <n>      override tier-1 latency (e.g. from a real CXLMemSim run)\n");
    printf("    --cxl-bandwidth-mbps <n>  override tier-1 bandwidth (e.g. from a real CXLMemSim run)\n");
    printf("    --out <file>     write JSON to file\n");
    printf("  replay [opts]           feed a real placement trace through the\n");
    printf("                          cost model, print bench_*.json-shaped JSON\n");
    printf("    --trace <name>=<file>    one JSON trace per strategy (repeatable);\n");
    printf("                             must cover noop,composite_lru,tinylfu,caat\n");
    printf("    --nodes <n>              emulated NUMA node count (default 2)\n");
    printf("    --cxl-latency-ns <n>      same calibration override as `eval`\n");
    printf("    --cxl-bandwidth-mbps <n>  same calibration override as `eval`\n");
    printf("    --out <file>     write JSON to file\n");
}

static const char *argval(int argc, char **argv, int *i, const char *flag) {
    if (*i + 1 < argc) { (*i)++; return argv[*i]; }
    fprintf(stderr, "missing value for %s\n", flag); exit(2);
}

static int write_file(const char *path, const char *data);

static int cmd_ops(void) {
    nf_ops_register_all();
    for (int i = 0; i < nf_ops_count(); i++) {
        const nf_op_t *o = nf_ops_get(i);
        printf("%-24s [%-6s] %s\n", o->name, o->category, o->title);
    }
    printf("\n%d atomic operations registered\n", nf_ops_count());
    return 0;
}

static int cmd_strategies(void) {
    for (int i = 0; i < nf_strategy_count(); i++) {
        const char *n = nf_strategy_name(i);
        printf("%-26s [%s] %s\n", n, nf_strategy_is_allocation(n) ? "alloc" : "migr", nf_strategy_desc(n));
    }
    return 0;
}

static int cmd_templates(void) {
    const char *last_cat = "";
    for (int i = 0; i < nf_template_count(); i++) {
        const nf_template_t *t = nf_template_get(i);
        if (strcmp(t->category, last_cat) != 0) {
            printf("\n[%s]\n", t->category);
            last_cat = t->category;
        }
        printf("  %-24s %s\n", t->name, t->description);
        printf("         use: %s\n", t->use_case);
    }
    printf("\n%d templates across 5 categories\n", nf_template_count());
    return 0;
}

static int cmd_template(const char *name, const char *path) {
    nf_graph_t g; nf_graph_init(&g);
    if (nf_template_build(&g, name) != NF_OK) { fprintf(stderr, "unknown template %s\n", name); return 1; }
    char *js = nf_graph_to_json(&g);
    printf("%s\n", js);
    int rc = path ? write_file(path, js) : 0;
    free(js); nf_graph_free(&g);
    return rc;
}

static int write_file(const char *path, const char *data) {
    FILE *fp = fopen(path, "wb");
    if (!fp) { fprintf(stderr, "cannot write %s\n", path); return 1; }
    fwrite(data, 1, strlen(data), fp);
    fclose(fp);
    return 0;
}

static int cmd_workflow(const char *name, const char *path) {
    nf_graph_t g; nf_graph_init(&g);
    if (nf_strategy_build(&g, name) != NF_OK) { fprintf(stderr, "unknown strategy %s\n", name); return 1; }
    char *js = nf_graph_to_json(&g);
    printf("%s\n", js);
    int rc = path ? write_file(path, js) : 0;
    free(js); nf_graph_free(&g);
    return rc;
}

static int cmd_dump_templates(const char *path) {
    nf_json_t *arr = nf_json_new_arr();
    for (int i = 0; i < nf_template_count(); i++) {
        const nf_template_t *t = nf_template_get(i);
        nf_json_t *j = nf_json_new_obj();
        nf_json_obj_set(j, "name", nf_json_new_str(t->name));
        nf_json_obj_set(j, "category", nf_json_new_str(t->category));
        nf_json_obj_set(j, "description", nf_json_new_str(t->description));
        nf_json_obj_set(j, "use_case", nf_json_new_str(t->use_case));
        nf_json_arr_push(arr, j);
    }
    char *js = nf_json_serialize(arr);
    int rc = write_file(path, js);
    if (rc == 0) printf("wrote %s\n", path);
    free(js); nf_json_free(arr);
    return rc;
}

static int cmd_dump_ops(const char *path) {
    nf_ops_register_all();
    nf_json_t *arr = nf_json_new_arr();
    for (int i = 0; i < nf_ops_count(); i++) {
        const nf_op_t *o = nf_ops_get(i);
        nf_json_t *j = nf_json_new_obj();
        nf_json_obj_set(j, "name", nf_json_new_str(o->name));
        nf_json_obj_set(j, "title", nf_json_new_str(o->title));
        nf_json_obj_set(j, "description", nf_json_new_str(o->description));
        nf_json_obj_set(j, "category", nf_json_new_str(o->category));
        nf_json_t *ps = nf_json_new_arr();
        for (int k = 0; k < o->param_count; k++) {
            nf_json_t *p = nf_json_new_obj();
            nf_json_obj_set(p, "name", nf_json_new_str(o->params[k].name));
            nf_json_obj_set(p, "type", nf_json_new_str(o->params[k].type == NF_PARAM_INT ? "int" : o->params[k].type == NF_PARAM_DOUBLE ? "double" : o->params[k].type == NF_PARAM_BOOL ? "bool" : "string"));
            nf_json_obj_set(p, "default", nf_json_new_str(o->params[k].default_value ? o->params[k].default_value : ""));
            nf_json_obj_set(p, "description", nf_json_new_str(o->params[k].description ? o->params[k].description : ""));
            nf_json_arr_push(ps, p);
        }
        nf_json_obj_set(j, "params", ps);
        nf_json_arr_push(arr, j);
    }
    char *js = nf_json_serialize(arr);
    int rc = write_file(path, js);
    if (rc == 0) printf("wrote %s\n", path);
    free(js); nf_json_free(arr);
    return rc;
}

/* Execute a workflow file against a tiny synthetic input and report the
 * outcome (used by the GUI Run button to validate a user's DAG). */
static int cmd_run(const char *path) {
    nf_ops_register_all();
    nf_graph_t g; nf_graph_init(&g);
    char err[256];
    if (nf_graph_load_file(&g, path, err, sizeof(err)) != NF_OK) { fprintf(stderr, "load: %s\n", err); return 1; }
    numaflow_env_t env; nf_numa_env_init(&env); nf_numa_configure_default(&env, 2);
    nf_tracker_t tr; nf_tracker_init(&tr, 2, 4096, 100000);
    nf_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    ctx.topo = env.nodes; ctx.topo_count = env.node_count; ctx.env = &env; ctx.tracker = &tr; ctx.budget = 256; ctx.rng = nf_rng_seed(1); ctx.tick = 1;
    nf_items_t init; nf_items_init(&init);
    for (int i = 0; i < 5; i++) {
        nf_item_t it; memset(&it, 0, sizeof(it));
        snprintf(it.key, NF_KEY_MAX, "key%d", i); it.value_size = 256; it.current_node = 1; it.access_count = (uint64_t)(10 - i); it.recency = (uint64_t)(i + 1);
        nf_items_push(&init, &it);
    }
    nf_exec_t ex; nf_exec_init(&ex);
    int rc = nf_exec_run(&ex, &g, &init, &ctx);
    printf("workflow=%s nodes=%d edges=%d\n", g.name[0] ? g.name : "custom", g.node_count, g.edge_count);
    if (rc == NF_OK) printf("execution=OK result_items=%lu migrations=%lu\n", (unsigned long)ex.result.count, (unsigned long)ctx.stats.migrations_done);
    else printf("execution=ERROR error=%s\n", ex.error);
    nf_exec_free(&ex); nf_items_free(&init); nf_tracker_free(&tr); nf_numa_env_destroy(&env); nf_graph_free(&g);
    return rc == NF_OK ? 0 : 1;
}

static int cmd_eval(int argc, char **argv) {
    nf_bench_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    const char *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--workload") == 0) cfg.workload = argval(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--keys") == 0) cfg.keys = (size_t)atoll(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--accesses") == 0) cfg.accesses = (size_t)atoll(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--epoch") == 0) cfg.epoch = (size_t)atoll(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--budget") == 0) cfg.budget = atoi(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--nodes") == 0) cfg.nodes = atoi(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--seed") == 0) cfg.seed = strtoull(argval(argc, argv, &i, argv[i]), NULL, 10);
        else if (strcmp(argv[i], "--cxl-latency-ns") == 0) cfg.cxl_latency_ns = atof(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--cxl-bandwidth-mbps") == 0) cfg.cxl_bandwidth_mbps = atof(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--out") == 0) out = argval(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
    }
    nf_bench_config_defaults(&cfg);
    nf_json_t *res = nf_bench_run(&cfg);
    char *js = nf_json_serialize(res);
    if (out) { int rc = write_file(out, js); if (rc == 0) printf("wrote %s\n", out); }
    else printf("%s\n", js);
    free(js); nf_json_free(res);
    return 0;
}

/* ---- replay: feed a real placement trace through the cost model --------
 * Unlike `eval` (which generates its own synthetic access trace), `replay`
 * takes a trace of what a strategy *actually* decided on a real system - one
 * JSON array per strategy of {key, size, access_count, origin_node,
 * final_node} records - and runs it through the same pure cost-model
 * functions (nf_numa_access_cost / nf_numa_migrate_cost) `eval` uses, with
 * the same --cxl-latency-ns/--cxl-bandwidth-mbps calibration. This is a
 * modeled projection of a *real* placement decision onto calibrated
 * hardware parameters, not a measurement - the trace itself carries no
 * timing information, only "who accessed what, how often, and where it
 * ended up". See ADR-12 in docs/new/09-architecture-decisions.md. */
typedef struct {
    double   local_hit_ratio;
    double   access_cost;
    double   migration_cost;
    double   net_cost;
    uint64_t migrations;
    uint64_t node_bytes[NF_MAX_NODES];
} replay_result_t;

static char *read_whole_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long len = ftell(fp);
    if (len < 0 || fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static int replay_trace_file(const char *path, const numaflow_env_t *env, replay_result_t *out) {
    memset(out, 0, sizeof(*out));
    char *buf = read_whole_file(path);
    if (!buf) { fprintf(stderr, "cannot read trace %s\n", path); return -1; }

    const char *err = NULL;
    nf_json_t *arr = nf_json_parse(buf, &err);
    free(buf);
    if (!arr) { fprintf(stderr, "parse %s: %s\n", path, err ? err : "unknown error"); return -1; }
    if (nf_json_type(arr) != NF_JSON_ARR) {
        fprintf(stderr, "trace %s: expected a top-level JSON array\n", path);
        nf_json_free(arr);
        return -1;
    }

    /* The accessing CPU is always on env->local_cpu_node (this is a
     * single-threaded model, same convention nf_bench.c uses) - "local"
     * means the data sits on that node *now* (final_node), which is
     * independent of where a given key originally landed. Comparing
     * final_node against a per-record origin_node instead would make
     * local_hit_ratio tautologically 100% for any strategy that never
     * migrates (final always equals origin when nothing moved it), making
     * noop look best by construction regardless of where it actually
     * happened to land - exactly the kind of structurally-guaranteed,
     * uninformative metric this session already got burned by once. */
    int local_node = (int)env->local_cpu_node;
    double total_weight = 0.0, local_weight = 0.0;
    size_t n = nf_json_arr_len(arr);
    for (size_t i = 0; i < n; i++) {
        nf_json_t *rec = nf_json_arr_get(arr, i);
        size_t size   = (size_t)nf_json_obj_get_num(rec, "size", 0.0);
        double acc    = nf_json_obj_get_num(rec, "access_count", 0.0);
        int origin    = (int)nf_json_obj_get_num(rec, "origin_node", 0.0);
        int final_node = (int)nf_json_obj_get_num(rec, "final_node", (double)origin);
        if (origin < 0 || origin >= env->node_count) origin = 0;
        if (final_node < 0 || final_node >= env->node_count) final_node = origin;

        out->access_cost += nf_numa_access_cost(env, local_node, final_node, size) * acc;
        total_weight += acc;
        if (final_node == local_node) local_weight += acc;

        if (final_node != origin) {
            out->migration_cost += nf_numa_migrate_cost(env, origin, final_node, size);
            out->migrations++;
        }
        out->node_bytes[final_node] += size;
    }
    nf_json_free(arr);
    out->local_hit_ratio = total_weight > 0.0 ? local_weight / total_weight : 0.0;
    out->net_cost = out->access_cost + out->migration_cost;
    return 0;
}

static nf_json_t *replay_result_to_json(const char *name, const replay_result_t *r, int nodes) {
    nf_json_t *o = nf_json_new_obj();
    nf_json_obj_set(o, "strategy", nf_json_new_str(name));
    nf_json_obj_set(o, "local_hit_ratio", nf_json_new_num(r->local_hit_ratio));
    nf_json_obj_set(o, "access_cost", nf_json_new_num(r->access_cost));
    nf_json_obj_set(o, "migration_cost", nf_json_new_num(r->migration_cost));
    nf_json_obj_set(o, "net_cost", nf_json_new_num(r->net_cost));
    nf_json_obj_set(o, "migrations", nf_json_new_num((double)r->migrations));
    /* No tracker ran over a static trace, so there is no feedback score;
     * kept as a zeroed field only so this stays drop-in shape-compatible
     * with eval's result_to_json() for report.py. */
    nf_json_obj_set(o, "feedback", nf_json_new_num(0.0));
    nf_json_t *nb = nf_json_new_arr();
    for (int i = 0; i < nodes; i++) nf_json_arr_push(nb, nf_json_new_num((double)r->node_bytes[i]));
    nf_json_obj_set(o, "node_bytes", nb);
    return o;
}

#define NF_CLI_MAX_TRACES 16

static int cmd_replay(int argc, char **argv) {
    const char *trace_names[NF_CLI_MAX_TRACES];
    const char *trace_paths[NF_CLI_MAX_TRACES];
    int trace_count = 0;
    int nodes = 2;
    double cxl_latency_ns = 0.0, cxl_bandwidth_mbps = 0.0;
    const char *out = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) {
            const char *spec = argval(argc, argv, &i, argv[i]);
            const char *eq = strchr(spec, '=');
            if (!eq || eq == spec) { fprintf(stderr, "--trace expects name=file, got '%s'\n", spec); return 2; }
            if (trace_count >= NF_CLI_MAX_TRACES) { fprintf(stderr, "too many --trace entries (max %d)\n", NF_CLI_MAX_TRACES); return 2; }
            char *name = (char *)malloc((size_t)(eq - spec) + 1);
            memcpy(name, spec, (size_t)(eq - spec)); name[eq - spec] = '\0';
            trace_names[trace_count] = name;
            trace_paths[trace_count] = eq + 1;
            trace_count++;
        }
        else if (strcmp(argv[i], "--nodes") == 0) nodes = atoi(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--cxl-latency-ns") == 0) cxl_latency_ns = atof(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--cxl-bandwidth-mbps") == 0) cxl_bandwidth_mbps = atof(argval(argc, argv, &i, argv[i]));
        else if (strcmp(argv[i], "--out") == 0) out = argval(argc, argv, &i, argv[i]);
        else if (strcmp(argv[i], "--help") == 0) { usage(); return 0; }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (trace_count == 0) { fprintf(stderr, "at least one --trace name=file is required\n"); return 2; }

    numaflow_env_t env; nf_numa_env_init(&env);
    nf_numa_configure_default(&env, nodes);
    if (cxl_latency_ns > 0.0 && env.node_count > 1) env.nodes[1].latency_ns = cxl_latency_ns;
    if (cxl_bandwidth_mbps > 0.0 && env.node_count > 1) env.nodes[1].bandwidth_mbps = cxl_bandwidth_mbps;

    nf_json_t *root = nf_json_new_obj();
    nf_json_t *cj = nf_json_new_obj();
    nf_json_obj_set(cj, "source", nf_json_new_str("real_placement_trace"));
    nf_json_obj_set(cj, "nodes", nf_json_new_num((double)env.node_count));
    nf_json_obj_set(cj, "cxl_latency_ns", nf_json_new_num(cxl_latency_ns));
    nf_json_obj_set(cj, "cxl_bandwidth_mbps", nf_json_new_num(cxl_bandwidth_mbps));
    nf_json_obj_set(root, "config", cj);

    nf_json_t *tj = nf_json_new_arr();
    for (int i = 0; i < env.node_count; i++) {
        nf_json_t *node = nf_json_new_obj();
        nf_json_obj_set(node, "name", nf_json_new_str(env.nodes[i].name));
        nf_json_obj_set(node, "latency_ns", nf_json_new_num(env.nodes[i].latency_ns));
        nf_json_obj_set(node, "bandwidth_mbps", nf_json_new_num(env.nodes[i].bandwidth_mbps));
        nf_json_arr_push(tj, node);
    }
    nf_json_obj_set(root, "topology", tj);

    nf_json_t *mj = nf_json_new_arr();
    int rc = 0;
    for (int i = 0; i < trace_count; i++) {
        replay_result_t r;
        if (replay_trace_file(trace_paths[i], &env, &r) != 0) { rc = 1; continue; }
        nf_json_arr_push(mj, replay_result_to_json(trace_names[i], &r, env.node_count));
    }
    nf_json_obj_set(root, "migration", mj);
    for (int i = 0; i < trace_count; i++) free((void *)trace_names[i]);
    nf_numa_env_destroy(&env);

    if (rc != 0) { nf_json_free(root); return 1; }

    char *js = nf_json_serialize(root);
    if (out) { rc = write_file(out, js); if (rc == 0) printf("wrote %s\n", out); }
    else printf("%s\n", js);
    free(js); nf_json_free(root);
    return rc;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    const char *cmd = argv[1];
    if (strcmp(cmd, "ops") == 0) return cmd_ops();
    if (strcmp(cmd, "strategies") == 0) return cmd_strategies();
    if (strcmp(cmd, "templates") == 0) return cmd_templates();
    if (strcmp(cmd, "template") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: template <name> [file]\n"); return 1; }
        return cmd_template(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (strcmp(cmd, "workflow") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: workflow <name> [file]\n"); return 1; }
        return cmd_workflow(argv[2], argc > 3 ? argv[3] : NULL);
    }
    if (strcmp(cmd, "run") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: run <file>\n"); return 1; }
        return cmd_run(argv[2]);
    }
    if (strcmp(cmd, "dump-templates") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: dump-templates <file>\n"); return 1; }
        return cmd_dump_templates(argv[2]);
    }
    if (strcmp(cmd, "dump-ops") == 0) {
        if (argc < 3) { fprintf(stderr, "usage: dump-ops <file>\n"); return 1; }
        return cmd_dump_ops(argv[2]);
    }
    if (strcmp(cmd, "eval") == 0) return cmd_eval(argc - 1, argv + 1);
    if (strcmp(cmd, "replay") == 0) return cmd_replay(argc - 1, argv + 1);
    usage();
    return 1;
}
