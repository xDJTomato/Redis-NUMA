/* numa_migrate.h - NUMA memory migration module
 *
 * This module provides functionality to migrate memory objects between NUMA nodes.
 * Phase 1: Basic migration functionality for testing
 * Phase 2: Automatic load balancing with hotness tracking
 */

#ifndef NUMA_MIGRATE_H
#define NUMA_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

/* Migration result codes */
#define NUMA_MIGRATE_OK        0
#define NUMA_MIGRATE_ERR      -1
#define NUMA_MIGRATE_INVALID  -2
#define NUMA_MIGRATE_NOMEM    -3

/* Migration statistics */
typedef struct {
    uint64_t total_migrations;      /* Total number of migrations performed */
    uint64_t bytes_migrated;        /* Total bytes migrated */
    uint64_t failed_migrations;     /* Number of failed migrations */
    uint64_t migration_time_us;     /* Total time spent in migration (microseconds) */
} numa_migrate_stats_t;

/* Initialize migration module */
int numa_migrate_init(void);

/* Cleanup migration module */
void numa_migrate_cleanup(void);

/* Migrate a memory block from current node to target node
 * 
 * @param ptr: Pointer to memory block (user pointer, not raw)
 * @param size: Size of memory block
 * @param target_node: Target NUMA node ID
 * @return: New pointer on success, NULL on failure
 * 
 * Note: The old pointer becomes invalid after successful migration
 */
void *numa_migrate_memory(void *ptr, size_t size, int target_node);

/* Get migration statistics */
void numa_migrate_get_stats(numa_migrate_stats_t *stats);

/* Reset migration statistics */
void numa_migrate_reset_stats(void);

/* Test function: Migrate a test buffer and verify */
int numa_migrate_test(void);

#endif /* NUMA_MIGRATE_H */
