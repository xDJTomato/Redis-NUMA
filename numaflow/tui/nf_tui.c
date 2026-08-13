/* nf_tui.c - interactive text UI (pure C11, ANSI/VT) for NUMAflow.
 *
 * Lets a user compose a workflow from atomic operations, save/load it as JSON,
 * run the fair benchmark, and create periodic memory-scheduling tasks that
 * report the tracked feedback score each tick.
 */
#include "nf_ops.h"
#include "nf_exec.h"
#include "nf_strategy.h"
#include "nf_bench.h"
#include "nf_json.h"
#include "nf_track.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void clear(void) { printf("\x1b[2J\x1b[H"); fflush(stdout); }

static int read_line(char *buf, int n) {
    if (!fgets(buf, n, stdin)) return 0;
    size_t l = strlen(buf); while (l && (buf[l-1]=='\n'||buf[l-1]=='\r')) buf[--l]='\0';
    return 1;
}

static void print_ops(void) {
    nf_ops_register_all();
    for (int i = 0; i < nf_ops_count(); i++) {
        const nf_op_t *o = nf_ops_get(i);
        printf("  %-26s [%-6s] %s\n", o->name, o->category, o->title);
    }
}

static void print_strategies(void) {
    for (int i = 0; i < nf_strategy_count(); i++) {
        const char *n = nf_strategy_name(i);
        printf("  %-26s %s\n", n, nf_strategy_desc(n));
    }
}

/* build a linear chain of ops chosen by the user */
static void build_chain(nf_graph_t *g) {
    char buf[NF_OP_MAX];
    nf_ops_register_all();
    printf("Compose a chain of atomic ops (one per line, blank to finish):\n");
    print_ops();
    int n = 0; char prev[8] = "";
    while (1) {
        printf("op> "); fflush(stdout);
        if (!read_line(buf, sizeof(buf)) || buf[0] == '\0') break;
        if (!nf_ops_find(buf)) { printf("unknown op '%s'\n", buf); continue; }
        char id[8]; snprintf(id, sizeof(id), "n%d", ++n);
        nf_graph_add_node(g, id, buf);
        if (n > 1) nf_graph_add_edge(g, prev, id);
        snprintf(prev, sizeof(prev), "%s", id);
    }
    printf("chain of %d node(s) ready.\n", n);
}

static void run_eval_once(void) {
    nf_bench_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.workload = "zipf"; cfg.keys = 2000; cfg.accesses = 60000; cfg.epoch = 2000;
    cfg.budget = 128; cfg.nodes = 2; cfg.seed = UINT64_C(20240517);
    nf_json_t *res = nf_bench_run(&cfg);
    nf_json_t *mig = nf_json_obj_get(res, "migration");
    printf("\nMigration strategy comparison (zipf):\n");
    for (size_t i = 0; i < nf_json_arr_len(mig); i++) {
        nf_json_t *m = nf_json_arr_get(mig, i);
        printf("  %-16s hit=%.1f%%  net=%.0f ns  migrations=%.0f\n",
            nf_json_obj_get_str(m, "strategy"),
            nf_json_obj_get_num(m, "local_hit_ratio", 0) * 100,
            nf_json_obj_get_num(m, "net_cost", 0),
            nf_json_obj_get_num(m, "migrations", 0));
    }
    nf_json_free(res);
}

static void run_scheduler(void) {
    char buf[64];
    int interval = 2, ticks = 5;
    printf("interval (seconds) [2]: "); fflush(stdout);
    if (read_line(buf, sizeof(buf)) && buf[0]) interval = atoi(buf);
    printf("number of ticks [5]: "); fflush(stdout);
    if (read_line(buf, sizeof(buf)) && buf[0]) ticks = atoi(buf);

    nf_bench_config_t cfg; memset(&cfg, 0, sizeof(cfg));
    cfg.workload = "hotspot"; cfg.keys = 2000; cfg.accesses = 60000; cfg.epoch = 2000;
    cfg.budget = 128; cfg.nodes = 2; cfg.seed = UINT64_C(777);

    printf("\nPeriodic task: eval every %ds for %d ticks (tracking feedback each tick):\n", interval, ticks);
    for (int t = 1; t <= ticks; t++) {
        nf_json_t *res = nf_bench_run(&cfg);
        nf_json_t *mig = nf_json_obj_get(res, "migration");
        nf_json_t *caat = NULL;
        for (size_t i = 0; i < nf_json_arr_len(mig); i++) {
            nf_json_t *m = nf_json_arr_get(mig, i);
            if (strcmp(nf_json_obj_get_str(m, "strategy"), "caat") == 0) caat = m;
        }
        printf("  tick %d/%d  caat hit=%.1f%%  feedback=%.3f  migrations=%.0f\n",
            t, ticks, caat ? nf_json_obj_get_num(caat, "local_hit_ratio", 0) * 100 : 0.0,
            caat ? nf_json_obj_get_num(caat, "feedback", 0) : 0.0,
            caat ? nf_json_obj_get_num(caat, "migrations", 0) : 0.0);
        nf_json_free(res);
        if (t < ticks) {
            /* simple blocking sleep (portable enough for a TUI demo) */
            time_t start = time(NULL); while ((time(NULL) - start) < interval) {}
        }
    }
}

int main(void) {
    nf_ops_register_all();
    nf_graph_t g; nf_graph_init(&g);
    char buf[256];
    while (1) {
        clear();
        printf("=== NUMAflow TUI ===\n\n");
        printf("  1. List atomic operations\n");
        printf("  2. List built-in strategies\n");
        printf("  3. Compose a custom workflow (chain of ops)\n");
        printf("  4. Show current workflow (JSON)\n");
        printf("  5. Save workflow to JSON file\n");
        printf("  6. Load workflow from JSON file\n");
        printf("  7. Run fair benchmark once\n");
        printf("  8. Create periodic scheduling task\n");
        printf("  9. Exit\n\n");
        printf("choice> "); fflush(stdout);
        if (!read_line(buf, sizeof(buf))) break;
        int c = atoi(buf);
        switch (c) {
            case 1: clear(); print_ops(); printf("\n<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break;
            case 2: clear(); print_strategies(); printf("\n<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break;
            case 3: clear(); nf_graph_free(&g); nf_graph_init(&g); build_chain(&g); printf("\n<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break;
            case 4: { char *js = nf_graph_to_json(&g); printf("%s\n", js); free(js); printf("\n<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break; }
            case 5: printf("file> "); fflush(stdout); read_line(buf, sizeof(buf)); { char *js = nf_graph_to_json(&g); FILE *f = fopen(buf, "wb"); if (f) { fwrite(js,1,strlen(js),f); fclose(f); printf("saved %s\n", buf);} else printf("cannot save\n"); free(js); } printf("<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break;
            case 6: printf("file> "); fflush(stdout); read_line(buf, sizeof(buf)); { char e[256]; nf_graph_free(&g); nf_graph_init(&g); if (nf_graph_load_file(&g, buf, e, sizeof(e)) != NF_OK) printf("load failed: %s\n", e); else printf("loaded %d nodes\n", g.node_count); } printf("<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break;
            case 7: clear(); run_eval_once(); printf("\n<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break;
            case 8: clear(); run_scheduler(); printf("\n<press enter>"); fflush(stdout); read_line(buf, sizeof(buf)); break;
            case 9: nf_graph_free(&g); return 0;
            default: break;
        }
    }
    nf_graph_free(&g);
    return 0;
}
