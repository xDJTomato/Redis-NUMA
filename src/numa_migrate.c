/* numa_migrate.c - NUMA memory migration module implementation
 *
 * Basic migration functionality for testing.
 * Uses numa_zmalloc_onnode to allocate on target node and copies data.
 */

#define _GNU_SOURCE
#include "numa_migrate.h"
#include "zmalloc.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <numa.h>

/* Internal statistics */
static numa_migrate_stats_t migrate_stats = {0};
static int migrate_initialized = 0;

/* Get current time in microseconds */
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* Initialize migration module */
int numa_migrate_init(void)
{
    if (migrate_initialized)
        return 0;
    
    if (numa_available() == -1) {
        return NUMA_MIGRATE_ERR;
    }
    
    memset(&migrate_stats, 0, sizeof(migrate_stats));
    migrate_initialized = 1;
    return NUMA_MIGRATE_OK;
}

/* Cleanup migration module */
void numa_migrate_cleanup(void)
{
    migrate_initialized = 0;
}

/* Migrate a memory block from current node to target node */
void *numa_migrate_memory(void *ptr, size_t size, int target_node)
{
    if (!migrate_initialized || ptr == NULL || size == 0) {
        return NULL;
    }
    
    /* Validate target node */
    if (target_node < 0 || target_node > numa_max_node()) {
        return NULL;
    }
    
    uint64_t start_time = get_time_us();
    
    /* Step 1: Allocate new memory on target node */
    void *new_ptr = numa_zmalloc_onnode(size, target_node);
    if (!new_ptr) {
        migrate_stats.failed_migrations++;
        return NULL;
    }
    
    /* Step 2: Copy data from old location to new location */
    memcpy(new_ptr, ptr, size);
    
    /* Step 3: Free old memory */
    zfree(ptr);
    
    /* Step 4: Update statistics */
    migrate_stats.total_migrations++;
    migrate_stats.bytes_migrated += size;
    migrate_stats.migration_time_us += (get_time_us() - start_time);
    
    return new_ptr;
}

/* Get migration statistics */
void numa_migrate_get_stats(numa_migrate_stats_t *stats)
{
    if (stats) {
        *stats = migrate_stats;
    }
}

/* Reset migration statistics */
void numa_migrate_reset_stats(void)
{
    memset(&migrate_stats, 0, sizeof(migrate_stats));
}

/* Test function: Migrate a test buffer and verify */
int numa_migrate_test(void)
{
    if (!migrate_initialized) {
        printf("Migration module not initialized\n");
        return -1;
    }
    
    int num_nodes = numa_max_node() + 1;
    
    printf("=== NUMA Migration Test ===\n");
    printf("Available NUMA nodes: %d\n", num_nodes);
    
    if (num_nodes < 2) {
        printf("\nNote: Only 1 NUMA node available. Running basic allocation test.\n");
        
        /* Basic allocation and free test */
        size_t test_size = 1024;
        char *test_data = zmalloc(test_size);
        if (!test_data) {
            printf("Failed to allocate test buffer\n");
            return -1;
        }
        
        /* Fill with test pattern */
        for (size_t i = 0; i < test_size; i++) {
            test_data[i] = (char)(i % 256);
        }
        
        /* Verify data */
        int data_ok = 1;
        for (size_t i = 0; i < test_size; i++) {
            if (test_data[i] != (char)(i % 256)) {
                data_ok = 0;
                break;
            }
        }
        
        zfree(test_data);
        
        if (data_ok) {
            printf("Basic allocation test: PASSED\n");
            printf("\n=== Migration Test COMPLETED (single node) ===\n");
            return 0;
        } else {
            printf("Basic allocation test: FAILED\n");
            return -1;
        }
    }
    
    /* Test 1: Basic migration */
    printf("\nTest 1: Basic memory migration\n");
    
    size_t test_size = 1024;
    char *test_data = zmalloc(test_size);
    if (!test_data) {
        printf("Failed to allocate test buffer\n");
        return -1;
    }
    
    /* Fill with test pattern */
    for (size_t i = 0; i < test_size; i++) {
        test_data[i] = (char)(i % 256);
    }
    
    printf("Allocated %zu bytes on node %d\n", test_size, numa_node_of_cpu(sched_getcpu()));
    
    /* Try to migrate to another node */
    int target_node = (numa_node_of_cpu(sched_getcpu()) + 1) % num_nodes;
    printf("Migrating to node %d...\n", target_node);
    
    char *migrated_data = numa_migrate_memory(test_data, test_size, target_node);
    if (!migrated_data) {
        printf("Migration failed!\n");
        zfree(test_data);
        return -1;
    }
    
    /* Verify data integrity */
    int data_ok = 1;
    for (size_t i = 0; i < test_size; i++) {
        if (migrated_data[i] != (char)(i % 256)) {
            data_ok = 0;
            printf("Data corruption at offset %zu: expected %d, got %d\n",
                   i, (int)(i % 256), (int)migrated_data[i]);
            break;
        }
    }
    
    if (data_ok) {
        printf("Data integrity check: PASSED\n");
    } else {
        printf("Data integrity check: FAILED\n");
        zfree(migrated_data);
        return -1;
    }
    
    /* Test 2: Multiple migrations */
    printf("\nTest 2: Multiple migrations\n");
    
    numa_migrate_reset_stats();
    int num_migrations = 10;
    char *current_ptr = migrated_data;
    
    for (int i = 0; i < num_migrations; i++) {
        int next_node = (target_node + i) % num_nodes;
        char *new_ptr = numa_migrate_memory(current_ptr, test_size, next_node);
        if (!new_ptr) {
            printf("Migration %d failed!\n", i + 1);
            break;
        }
        current_ptr = new_ptr;
        printf("Migration %d: moved to node %d\n", i + 1, next_node);
    }
    
    /* Check final statistics */
    numa_migrate_stats_t stats;
    numa_migrate_get_stats(&stats);
    
    printf("\nMigration Statistics:\n");
    printf("  Total migrations: %lu\n", (unsigned long)stats.total_migrations);
    printf("  Bytes migrated: %lu\n", (unsigned long)stats.bytes_migrated);
    printf("  Failed migrations: %lu\n", (unsigned long)stats.failed_migrations);
    printf("  Total time: %lu us\n", (unsigned long)stats.migration_time_us);
    if (stats.total_migrations > 0) {
        printf("  Average time per migration: %lu us\n",
               (unsigned long)(stats.migration_time_us / stats.total_migrations));
    }
    
    /* Cleanup */
    zfree(current_ptr);
    
    printf("\n=== Migration Test COMPLETED ===\n");
    return 0;
}
