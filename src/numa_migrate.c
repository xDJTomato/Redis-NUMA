/* numa_migrate.c - NUMA memory migration module implementation
 *
 * Basic migration functionality for the testing phase.
 * Allocates memory on the target node with numa_zmalloc_onnode and copies the data.
 */

#define _GNU_SOURCE
#include "numa_migrate.h"
#include "zmalloc.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sched.h>
#include <numa.h>

/* Internal statistics. */
static numa_migrate_stats_t migrate_stats = {0};
static int migrate_initialized = 0;

/* Get the current time (microseconds). */
static uint64_t get_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* Initialize the migration module. */
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

/* Clean up the migration module. */
void numa_migrate_cleanup(void)
{
    migrate_initialized = 0;
}

/* Migrate a memory block from the current node to the target node. */
void *numa_migrate_memory(void *ptr, size_t size, int target_node)
{
    if (!migrate_initialized || ptr == NULL || size == 0) {
        return NULL;
    }
    
    /* Validate the target node. */
    if (target_node < 0 || target_node > numa_max_node()) {
        return NULL;
    }
    
    uint64_t start_time = get_time_us();
    
    /* Step 1: allocate new memory on the target node. */
    void *new_ptr = numa_zmalloc_onnode(size, target_node);
    if (!new_ptr) {
        migrate_stats.failed_migrations++;
        return NULL;
    }
    
    /* Step 2: copy the data from the old address to the new address. */
    memcpy(new_ptr, ptr, size);
    
    /* Step 3: free the old memory. */
    zfree(ptr);
    
    /* Step 4: update the statistics. */
    migrate_stats.total_migrations++;
    migrate_stats.bytes_migrated += size;
    migrate_stats.migration_time_us += (get_time_us() - start_time);
    
    return new_ptr;
}

/* Get the migration statistics. */
void numa_migrate_get_stats(numa_migrate_stats_t *stats)
{
    if (stats) {
        *stats = migrate_stats;
    }
}

/* Reset the migration statistics. */
void numa_migrate_reset_stats(void)
{
    memset(&migrate_stats, 0, sizeof(migrate_stats));
}


