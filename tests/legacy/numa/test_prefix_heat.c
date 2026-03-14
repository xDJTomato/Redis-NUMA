/*
 * Test program for PREFIX heat tracking API
 * 
 * Compile: gcc -DHAVE_NUMA -o test_prefix_heat test_prefix_heat.c -I./src -L./src -lnuma -lpthread
 * Or use the Makefile target
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#define HAVE_NUMA
#include "src/zmalloc.h"
#include "src/numa_pool.h"

/* Test basic heat tracking */
void test_basic_heat_tracking() {
    printf("\n=== Test 1: Basic Heat Tracking ===\n");
    
    /* Allocate memory */
    void *ptr = numa_zmalloc(100);
    assert(ptr != NULL);
    printf("Allocated 100 bytes at %p\n", ptr);
    
    /* Check default values */
    uint8_t hotness = numa_get_hotness(ptr);
    uint8_t count = numa_get_access_count(ptr);
    uint16_t last = numa_get_last_access(ptr);
    int node = numa_get_node_id(ptr);
    
    printf("Default values:\n");
    printf("  hotness = %d (expected: %d)\n", hotness, NUMA_HOTNESS_DEFAULT);
    printf("  access_count = %d (expected: 0)\n", count);
    printf("  last_access = %d (expected: 0)\n", last);
    printf("  node_id = %d\n", node);
    
    assert(hotness == NUMA_HOTNESS_DEFAULT);
    assert(count == 0);
    assert(last == 0);
    assert(node >= 0);
    
    /* Test set/get hotness */
    printf("\nTesting hotness set/get...\n");
    for (int i = 0; i <= NUMA_HOTNESS_MAX + 2; i++) {
        numa_set_hotness(ptr, i);
        uint8_t got = numa_get_hotness(ptr);
        uint8_t expected = (i > NUMA_HOTNESS_MAX) ? NUMA_HOTNESS_MAX : i;
        printf("  Set hotness=%d, Got=%d (expected=%d) %s\n", 
               i, got, expected, got == expected ? "✓" : "✗");
        assert(got == expected);
    }
    
    /* Test access count */
    printf("\nTesting access count increment...\n");
    for (int i = 0; i < 5; i++) {
        numa_increment_access_count(ptr);
        uint8_t count = numa_get_access_count(ptr);
        printf("  After increment %d: count=%d\n", i+1, count);
    }
    
    /* Test last access */
    printf("\nTesting last access time...\n");
    for (uint16_t time = 0; time < 10; time += 3) {
        numa_set_last_access(ptr, time);
        uint16_t got = numa_get_last_access(ptr);
        printf("  Set last_access=%d, Got=%d %s\n", time, got, got == time ? "✓" : "✗");
        assert(got == time);
    }
    
    numa_zfree(ptr);
    printf("\n✓ Basic heat tracking test PASSED\n");
}

/* Test multiple allocations */
void test_multiple_allocations() {
    printf("\n=== Test 2: Multiple Allocations ===\n");
    
    void *ptrs[10];
    
    /* Allocate multiple blocks */
    for (int i = 0; i < 10; i++) {
        ptrs[i] = numa_zmalloc(50 + i * 10);
        assert(ptrs[i] != NULL);
        
        /* Set different hotness values */
        numa_set_hotness(ptrs[i], i % (NUMA_HOTNESS_MAX + 1));
        numa_set_last_access(ptrs[i], i * 100);
        
        printf("Block %d: ptr=%p, hotness=%d, last_access=%d\n",
               i, ptrs[i], numa_get_hotness(ptrs[i]), numa_get_last_access(ptrs[i]));
    }
    
    /* Verify all values */
    int errors = 0;
    for (int i = 0; i < 10; i++) {
        uint8_t hotness = numa_get_hotness(ptrs[i]);
        uint16_t last = numa_get_last_access(ptrs[i]);
        
        if (hotness != i % (NUMA_HOTNESS_MAX + 1)) {
            printf("ERROR: Block %d hotness mismatch: expected=%d, got=%d\n",
                   i, i % (NUMA_HOTNESS_MAX + 1), hotness);
            errors++;
        }
        if (last != i * 100) {
            printf("ERROR: Block %d last_access mismatch: expected=%d, got=%d\n",
                   i, i * 100, last);
            errors++;
        }
    }
    
    /* Free all */
    for (int i = 0; i < 10; i++) {
        numa_zfree(ptrs[i]);
    }
    
    if (errors == 0) {
        printf("\n✓ Multiple allocations test PASSED\n");
    } else {
        printf("\n✗ Multiple allocations test FAILED with %d errors\n", errors);
        exit(1);
    }
}

/* Test edge cases */
void test_edge_cases() {
    printf("\n=== Test 3: Edge Cases ===\n");
    
    /* Test NULL pointer handling */
    printf("Testing NULL pointer handling...\n");
    
    uint8_t hotness = numa_get_hotness(NULL);
    printf("  numa_get_hotness(NULL) = %d (expected: %d)\n", hotness, NUMA_HOTNESS_MIN);
    assert(hotness == NUMA_HOTNESS_MIN);
    
    uint8_t count = numa_get_access_count(NULL);
    printf("  numa_get_access_count(NULL) = %d (expected: 0)\n", count);
    assert(count == 0);
    
    uint16_t last = numa_get_last_access(NULL);
    printf("  numa_get_last_access(NULL) = %d (expected: 0)\n", last);
    assert(last == 0);
    
    int node = numa_get_node_id(NULL);
    printf("  numa_get_node_id(NULL) = %d (expected: -1)\n", node);
    assert(node == -1);
    
    /* These should not crash */
    numa_set_hotness(NULL, 5);
    numa_increment_access_count(NULL);
    numa_set_last_access(NULL, 100);
    printf("  NULL pointer setters handled gracefully\n");
    
    /* Test hotness boundary */
    printf("\nTesting hotness boundary values...\n");
    void *ptr = numa_zmalloc(64);
    
    numa_set_hotness(ptr, 255);  /* Max uint8_t */
    hotness = numa_get_hotness(ptr);
    printf("  Set hotness=255, Got=%d (expected: %d)\n", hotness, NUMA_HOTNESS_MAX);
    assert(hotness == NUMA_HOTNESS_MAX);
    
    numa_zfree(ptr);
    printf("\n✓ Edge cases test PASSED\n");
}

/* Test with different allocation sizes (Slab vs Pool) */
void test_slab_vs_pool() {
    printf("\n=== Test 4: Slab vs Pool Allocations ===\n");
    
    /* Small allocation (should use Slab) */
    void *small = numa_zmalloc(64);
    printf("Small allocation (64 bytes): ptr=%p\n", small);
    numa_set_hotness(small, 5);
    printf("  Set hotness=5, Got=%d\n", numa_get_hotness(small));
    assert(numa_get_hotness(small) == 5);
    
    /* Medium allocation (should use Pool) */
    void *medium = numa_zmalloc(256);
    printf("Medium allocation (256 bytes): ptr=%p\n", medium);
    numa_set_hotness(medium, 6);
    printf("  Set hotness=6, Got=%d\n", numa_get_hotness(medium));
    assert(numa_get_hotness(medium) == 6);
    
    /* Large allocation (should use direct) */
    void *large = numa_zmalloc(1024);
    printf("Large allocation (1024 bytes): ptr=%p\n", large);
    numa_set_hotness(large, 7);
    printf("  Set hotness=7, Got=%d\n", numa_get_hotness(large));
    assert(numa_get_hotness(large) == 7);
    
    numa_zfree(small);
    numa_zfree(medium);
    numa_zfree(large);
    
    printf("\n✓ Slab vs Pool test PASSED\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("========================================\n");
    printf("PREFIX Heat Tracking API Test Suite\n");
    printf("========================================\n");
    
    /* Initialize NUMA */
    numa_init();
    printf("NUMA initialized: %s\n", 
           numa_pool_available() ? "YES" : "NO");
    printf("Number of NUMA nodes: %d\n", numa_pool_num_nodes());
    
    /* Run tests */
    test_basic_heat_tracking();
    test_multiple_allocations();
    test_edge_cases();
    test_slab_vs_pool();
    
    /* Cleanup */
    numa_cleanup();
    
    printf("\n========================================\n");
    printf("All tests PASSED!\n");
    printf("========================================\n");
    
    return 0;
}
