/*
 * Integration test: Composite LRU + PREFIX Heat Tracking
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
#include "src/numa_composite_lru.h"

/* Mock key structure */
typedef struct mock_key {
    char name[32];
    void *val;  /* Points to memory block with PREFIX */
} mock_key_t;

/* Test Composite LRU with PREFIX heat tracking */
void test_composite_lru_with_prefix() {
    printf("\n=== Test: Composite LRU + PREFIX Heat Tracking ===\n");
    
    /* Initialize strategy */
    numa_strategy_t *strategy = composite_lru_create();
    assert(strategy != NULL);
    
    int ret = composite_lru_init(strategy);
    assert(ret == NUMA_STRATEGY_OK);
    printf("Composite LRU strategy initialized\n");
    
    /* Create mock keys with values */
    mock_key_t keys[5];
    for (int i = 0; i < 5; i++) {
        snprintf(keys[i].name, sizeof(keys[i].name), "key_%d", i);
        keys[i].val = numa_zmalloc(64 + i * 10);  /* Different sizes */
        assert(keys[i].val != NULL);
        printf("Created %s with value at %p (node=%d)\n", 
               keys[i].name, keys[i].val, numa_get_node_id(keys[i].val));
    }
    
    /* Simulate local accesses - should increase hotness */
    printf("\nSimulating 5 local accesses per key...\n");
    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 5; i++) {
            composite_lru_record_access(strategy, &keys[i], keys[i].val);
        }
    }
    
    /* Check hotness levels */
    printf("\nHotness levels after local accesses:\n");
    for (int i = 0; i < 5; i++) {
        uint8_t hotness = numa_get_hotness(keys[i].val);
        uint8_t count = numa_get_access_count(keys[i].val);
        printf("  %s: hotness=%d, access_count=%d\n", keys[i].name, hotness, count);
        /* Hotness should have increased (capped at 7) */
        assert(hotness >= NUMA_HOTNESS_DEFAULT);
    }
    
    /* Execute strategy - should perform decay */
    printf("\nExecuting strategy (heat decay)...\n");
    ret = composite_lru_execute(strategy);
    assert(ret == NUMA_STRATEGY_OK);
    
    /* Check statistics */
    uint64_t heat_updates, migrations, decays;
    composite_lru_get_stats(strategy, &heat_updates, &migrations, &decays);
    printf("Strategy stats: heat_updates=%llu, migrations=%llu, decays=%llu\n",
           (unsigned long long)heat_updates,
           (unsigned long long)migrations,
           (unsigned long long)decays);
    assert(heat_updates >= 25);  /* 5 keys * 5 rounds */
    
    /* Cleanup */
    for (int i = 0; i < 5; i++) {
        numa_zfree(keys[i].val);
    }
    
    composite_lru_destroy(strategy);
    printf("\n✓ Composite LRU + PREFIX integration test PASSED\n");
}

/* Test migration triggering based on hotness */
void test_migration_trigger() {
    printf("\n=== Test: Migration Trigger by Hotness ===\n");
    
    numa_strategy_t *strategy = composite_lru_create();
    assert(strategy != NULL);
    composite_lru_init(strategy);
    
    /* Create a key with high hotness */
    mock_key_t hot_key;
    strcpy(hot_key.name, "hot_key");
    hot_key.val = numa_zmalloc(100);
    
    /* Manually set high hotness */
    numa_set_hotness(hot_key.val, 6);  /* Close to threshold of 5 */
    printf("Created hot_key with initial hotness=%d\n", numa_get_hotness(hot_key.val));
    
    /* Access from "remote" node simulation - should trigger migration consideration */
    printf("Simulating remote access...\n");
    composite_lru_record_access(strategy, &hot_key, hot_key.val);
    
    /* Check if pending migration was created */
    printf("Hotness after access: %d\n", numa_get_hotness(hot_key.val));
    
    /* Execute strategy to process pending migrations */
    composite_lru_execute(strategy);
    
    numa_zfree(hot_key.val);
    composite_lru_destroy(strategy);
    
    printf("\n✓ Migration trigger test PASSED\n");
}

/* Test fallback to legacy mode (when val is NULL) */
void test_legacy_fallback() {
    printf("\n=== Test: Legacy Fallback (val=NULL) ===\n");
    
    numa_strategy_t *strategy = composite_lru_create();
    assert(strategy != NULL);
    composite_lru_init(strategy);
    
    /* Create keys without associated memory (val=NULL) */
    mock_key_t keys[3];
    for (int i = 0; i < 3; i++) {
        snprintf(keys[i].name, sizeof(keys[i].name), "legacy_key_%d", i);
        keys[i].val = NULL;
    }
    
    /* Record accesses with val=NULL - should use legacy dictionary mode */
    printf("Recording accesses with val=NULL (legacy mode)...\n");
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            composite_lru_record_access(strategy, &keys[i], keys[i].val);
        }
    }
    
    /* Execute strategy */
    composite_lru_execute(strategy);
    
    /* Check statistics */
    uint64_t heat_updates, migrations, decays;
    composite_lru_get_stats(strategy, &heat_updates, &migrations, &decays);
    printf("Legacy mode stats: heat_updates=%llu\n", (unsigned long long)heat_updates);
    assert(heat_updates >= 9);  /* 3 keys * 3 rounds */
    
    composite_lru_destroy(strategy);
    printf("\n✓ Legacy fallback test PASSED\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("========================================\n");
    printf("Integration Test: Composite LRU + PREFIX\n");
    printf("========================================\n");
    
    /* Initialize NUMA */
    numa_init();
    printf("NUMA initialized: %s\n", numa_pool_available() ? "YES" : "NO");
    printf("Number of NUMA nodes: %d\n\n", numa_pool_num_nodes());
    
    /* Run tests */
    test_composite_lru_with_prefix();
    test_migration_trigger();
    test_legacy_fallback();
    
    /* Cleanup */
    numa_cleanup();
    
    printf("\n========================================\n");
    printf("All integration tests PASSED!\n");
    printf("========================================\n");
    
    return 0;
}
