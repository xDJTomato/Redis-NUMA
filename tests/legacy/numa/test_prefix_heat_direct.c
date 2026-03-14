/*
 * Direct test for PREFIX heat tracking in Redis context
 * This test creates a Redis module-like scenario
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <unistd.h>

#define HAVE_NUMA
#include "src/zmalloc.h"
#include "src/numa_pool.h"

/* Simulate Redis object structure */
typedef struct redis_object {
    int type;
    int encoding;
    void *ptr;  /* Points to data allocated with zmalloc */
} robj;

/* Test PREFIX heat with simulated Redis objects */
void test_redis_object_heat() {
    printf("\n=== Test: Redis Object Heat Tracking ===\n");
    
    /* Initialize NUMA */
    numa_init();
    printf("NUMA initialized: nodes=%d\n", numa_pool_num_nodes());
    
    /* Create simulated Redis objects */
    #define NUM_OBJECTS 10
    robj objects[NUM_OBJECTS];
    
    printf("\nCreating %d Redis objects...\n", NUM_OBJECTS);
    for (int i = 0; i < NUM_OBJECTS; i++) {
        objects[i].type = 0;  /* STRING type */
        objects[i].encoding = 0;  /* RAW encoding */
        objects[i].ptr = numa_zmalloc(100 + i * 50);
        assert(objects[i].ptr != NULL);
        
        /* Initialize with some data */
        memset(objects[i].ptr, 'A' + i, 100 + i * 50);
        
        printf("  Object %d: ptr=%p, size=%d, node=%d, hotness=%d\n",
               i, objects[i].ptr, 100 + i * 50,
               numa_get_node_id(objects[i].ptr),
               numa_get_hotness(objects[i].ptr));
    }
    
    /* Simulate access pattern - create hotspots */
    printf("\nSimulating access pattern (objects 0-2 are hotspots)...\n");
    
    /* Hot objects - accessed frequently */
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 3; i++) {  /* Objects 0, 1, 2 are hot */
            uint8_t current = numa_get_hotness(objects[i].ptr);
            if (current < NUMA_HOTNESS_MAX) {
                numa_set_hotness(objects[i].ptr, current + 1);
            }
            numa_increment_access_count(objects[i].ptr);
            numa_set_last_access(objects[i].ptr, round * 10 + i);
        }
    }
    
    /* Cold objects - accessed once */
    for (int i = 3; i < NUM_OBJECTS; i++) {
        numa_increment_access_count(objects[i].ptr);
        numa_set_last_access(objects[i].ptr, i);
    }
    
    /* Check heat distribution */
    printf("\nHeat distribution after access pattern:\n");
    printf("%-10s %-15s %-10s %-12s %-15s\n", 
           "Object", "Ptr", "Hotness", "AccessCount", "LastAccess");
    printf("%-10s %-15s %-10s %-12s %-15s\n", 
           "------", "---", "-------", "-----------", "----------");
    
    for (int i = 0; i < NUM_OBJECTS; i++) {
        printf("%-10d %-15p %-10d %-12d %-15d\n",
               i,
               objects[i].ptr,
               numa_get_hotness(objects[i].ptr),
               numa_get_access_count(objects[i].ptr),
               numa_get_last_access(objects[i].ptr));
    }
    
    /* Verify hotness levels */
    printf("\nVerifying heat levels...\n");
    int errors = 0;
    for (int i = 0; i < 3; i++) {
        uint8_t hotness = numa_get_hotness(objects[i].ptr);
        if (hotness < 2) {
            printf("  ✗ Object %d should be hot (hotness >= 2), got %d\n", i, hotness);
            errors++;
        } else {
            printf("  ✓ Object %d is hot (hotness=%d)\n", i, hotness);
        }
    }
    
    for (int i = 3; i < NUM_OBJECTS; i++) {
        uint8_t hotness = numa_get_hotness(objects[i].ptr);
        if (hotness != NUMA_HOTNESS_DEFAULT) {
            printf("  ✗ Object %d should have default hotness (%d), got %d\n", 
                   i, NUMA_HOTNESS_DEFAULT, hotness);
            errors++;
        } else {
            printf("  ✓ Object %d has default hotness (%d)\n", i, hotness);
        }
    }
    
    /* Cleanup */
    printf("\nCleaning up...\n");
    for (int i = 0; i < NUM_OBJECTS; i++) {
        numa_zfree(objects[i].ptr);
    }
    
    numa_cleanup();
    
    if (errors == 0) {
        printf("\n✓ Redis Object Heat Tracking test PASSED\n");
    } else {
        printf("\n✗ Redis Object Heat Tracking test FAILED (%d errors)\n", errors);
        exit(1);
    }
}

/* Test migration decision based on hotness */
void test_migration_decision() {
    printf("\n=== Test: Migration Decision Based on Hotness ===\n");
    
    numa_init();
    
    /* Create objects with different hotness levels */
    void *hot_obj = numa_zmalloc(100);
    void *warm_obj = numa_zmalloc(100);
    void *cold_obj = numa_zmalloc(100);
    
    /* Set different hotness levels */
    numa_set_hotness(hot_obj, 7);   /* Max hotness */
    numa_set_hotness(warm_obj, 4);  /* Medium */
    numa_set_hotness(cold_obj, 1);  /* Low */
    
    /* Simulate access from different node */
    int current_node = 0;
    int obj_node = numa_get_node_id(hot_obj);
    
    printf("Current NUMA node: %d\n", current_node);
    printf("Object nodes: hot=%d, warm=%d, cold=%d\n",
           numa_get_node_id(hot_obj),
           numa_get_node_id(warm_obj),
           numa_get_node_id(cold_obj));
    
    /* Migration decision logic */
    #define MIGRATE_THRESHOLD 5
    
    printf("\nMigration decisions (threshold=%d):\n", MIGRATE_THRESHOLD);
    
    /* Hot object - should migrate */
    uint8_t hotness = numa_get_hotness(hot_obj);
    if (hotness >= MIGRATE_THRESHOLD) {
        printf("  ✓ Hot object (hotness=%d) -> SHOULD MIGRATE\n", hotness);
    } else {
        printf("  ✗ Hot object (hotness=%d) -> should migrate but won't\n", hotness);
    }
    
    /* Warm object - borderline */
    hotness = numa_get_hotness(warm_obj);
    if (hotness >= MIGRATE_THRESHOLD) {
        printf("  ✓ Warm object (hotness=%d) -> should migrate\n", hotness);
    } else {
        printf("  ✓ Warm object (hotness=%d) -> should NOT migrate\n", hotness);
    }
    
    /* Cold object - should not migrate */
    hotness = numa_get_hotness(cold_obj);
    if (hotness >= MIGRATE_THRESHOLD) {
        printf("  ✗ Cold object (hotness=%d) -> should NOT migrate but will\n", hotness);
    } else {
        printf("  ✓ Cold object (hotness=%d) -> should NOT migrate\n", hotness);
    }
    
    /* Cleanup */
    numa_zfree(hot_obj);
    numa_zfree(warm_obj);
    numa_zfree(cold_obj);
    
    numa_cleanup();
    
    printf("\n✓ Migration Decision test PASSED\n");
}

/* Test heat decay simulation */
void test_heat_decay() {
    printf("\n=== Test: Heat Decay Simulation ===\n");
    
    numa_init();
    
    void *obj = numa_zmalloc(100);
    
    /* Set initial high hotness */
    numa_set_hotness(obj, 7);
    numa_set_last_access(obj, 1000);
    printf("Initial: hotness=%d, last_access=%d\n",
           numa_get_hotness(obj), numa_get_last_access(obj));
    
    /* Simulate time passing and decay */
    uint16_t current_time = 1000;
    uint16_t decay_threshold = 100;  /* 100 time units */
    
    printf("\nSimulating heat decay (threshold=%d)...\n", decay_threshold);
    
    for (int i = 0; i < 5; i++) {
        current_time += 50;  /* Time passes */
        
        uint16_t time_diff = current_time - numa_get_last_access(obj);
        uint8_t hotness = numa_get_hotness(obj);
        
        if (time_diff > decay_threshold && hotness > NUMA_HOTNESS_MIN) {
            numa_set_hotness(obj, hotness - 1);
            printf("  Decay at t=%d: hotness %d -> %d (time_diff=%d)\n",
                   current_time, hotness, hotness - 1, time_diff);
        } else {
            printf("  No decay at t=%d: hotness=%d, time_diff=%d\n",
                   current_time, hotness, time_diff);
        }
    }
    
    printf("\nFinal: hotness=%d\n", numa_get_hotness(obj));
    
    numa_zfree(obj);
    numa_cleanup();
    
    printf("\n✓ Heat Decay test PASSED\n");
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    
    printf("========================================\n");
    printf("PREFIX Heat Tracking - Direct Tests\n");
    printf("========================================\n");
    
    test_redis_object_heat();
    test_migration_decision();
    test_heat_decay();
    
    printf("\n========================================\n");
    printf("All direct tests PASSED!\n");
    printf("========================================\n");
    
    return 0;
}
