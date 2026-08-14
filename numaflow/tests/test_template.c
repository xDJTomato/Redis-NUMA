/* test_template.c - verify every template builds a valid, runnable DAG. */
#include "nf_template.h"
#include "nf_ops.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, m); g_fail++; } } while (0)

int main(void) {
    nf_ops_register_all();
    int n = nf_template_count();
    CHECK(n >= 20, "at least 20 templates");
    CHECK(nf_template_find("tier_caat") != NULL, "find tier_caat");
    CHECK(strcmp(nf_template_default(), "tier_caat") == 0, "default is tier_caat");

    int built = 0;
    for (int i = 0; i < n; i++) {
        const nf_template_t *t = nf_template_get(i);
        nf_graph_t g; nf_graph_init(&g);
        int rc = nf_template_build(&g, t->name);
        CHECK(rc == NF_OK, t->name);
        CHECK(g.node_count > 0, "template has nodes");
        int order[64];
        if (nf_graph_topo_sort(&g, order) == NF_OK) {
            for (int j = 0; j < g.node_count; j++) {
                if (nf_ops_find(g.nodes[j].op) == NULL) {
                    fprintf(stderr, "FAIL: template %s references unknown op %s\n", t->name, g.nodes[j].op);
                    g_fail++;
                }
            }
        } else {
            fprintf(stderr, "FAIL: template %s has a cycle\n", t->name);
            g_fail++;
        }
        built++;
        nf_graph_free(&g);
    }
    CHECK(built == n, "all templates built");
    printf("built %d templates\n", built);

    if (g_fail == 0) { printf("ALL TEMPLATE TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", g_fail);
    return 1;
}
