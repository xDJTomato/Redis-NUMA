/* test_alloc.c - functional tests for the independent NUMA-aware allocator. */
#include "nf_alloc.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL [%s:%d] %s\n", __FILE__, __LINE__, m); g_fail++; } } while (0)

int main(void) {
    nf_alloc_t *a = nf_alloc_create(NULL);
    CHECK(a != NULL, "create allocator");
    CHECK(nf_alloc_num_classes() == 40, "40 size classes");
    CHECK(nf_alloc_class_size(0) == 8, "first class 8");
    CHECK(nf_alloc_class_size(39) == 16384, "last class 16KB");

    /* small allocations of many sizes; usable >= requested */
    static const size_t sizes[] = { 1, 7, 8, 9, 31, 32, 33, 100, 255, 256, 1024, 4095, 4096, 16384, 16385, 65536, 1<<20 };
    void *ptrs[64];
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) {
        ptrs[i] = nf_alloc_malloc(a, sizes[i]);
        CHECK(ptrs[i] != NULL, "malloc ok");
        size_t us = nf_alloc_usable_size(a, ptrs[i]);
        CHECK(us >= sizes[i], "usable >= requested");
        memset(ptrs[i], 0xAB, sizes[i]); /* touch */
    }
    /* free all, then re-allocate (reuse) */
    for (size_t i = 0; i < sizeof(sizes)/sizeof(sizes[0]); i++) nf_alloc_free(a, ptrs[i]);
    void *r = nf_alloc_malloc(a, 64);
    CHECK(r != NULL, "reuse after free");
    nf_alloc_free(a, r);

    /* onnode */
    void *n0 = nf_alloc_malloc_onnode(a, 512, 0);
    void *n1 = nf_alloc_malloc_onnode(a, 512, 1);
    CHECK(n0 != NULL && n1 != NULL, "onnode alloc");
    nf_alloc_free(a, n0); nf_alloc_free(a, n1);

    /* realloc grow / shrink */
    void *rp = nf_alloc_malloc(a, 100);
    memcpy(rp, "hello", 6);
    rp = nf_alloc_realloc(a, rp, 5000);
    CHECK(rp != NULL && memcmp(rp, "hello", 6) == 0, "realloc grow preserves data");
    rp = nf_alloc_realloc(a, rp, 16);
    CHECK(rp != NULL && memcmp(rp, "hello", 6) == 0, "realloc shrink preserves data");
    nf_alloc_free(a, rp);

    /* calloc zeroes */
    void *cz = nf_alloc_calloc(a, 10, 16);
    CHECK(cz != NULL, "calloc");
    int allzero = 1; for (int i = 0; i < 160; i++) if (((char*)cz)[i]) allzero = 0;
    CHECK(allzero, "calloc zeroed");
    nf_alloc_free(a, cz);

    /* stats */
    nf_alloc_stats_t st; nf_alloc_get_stats(a, &st);
    CHECK(st.alloc_count > 0, "stats alloc_count");
    CHECK(st.backend_bytes > 0, "stats backend_bytes");
    CHECK(st.num_nodes == 2, "2 nodes");
    double frag = nf_alloc_internal_frag(a);
    CHECK(frag >= 0.0 && frag < 1.0, "internal frag in sane range");

    nf_alloc_destroy(a);
    if (g_fail == 0) { printf("ALL ALLOCATOR TESTS PASSED\n"); return 0; }
    printf("%d FAILURES\n", g_fail);
    return 1;
}
