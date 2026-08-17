/* numa_migrate.h - NUMA memory migration module
 *
 * This module provides basic functionality to migrate memory objects between NUMA nodes.
 * Phase 1: basic migration functionality for testing
 * Phase 2: automatic load balancing with hotness tracking (planned)
 */

#ifndef NUMA_MIGRATE_H
#define NUMA_MIGRATE_H

#include <stddef.h>
#include <stdint.h>

/* Migration operation return codes. */
#define NUMA_MIGRATE_OK        0    /* Operation succeeded. */
#define NUMA_MIGRATE_ERR      -1    /* General error. */
#define NUMA_MIGRATE_INVALID  -2    /* Invalid argument. */
#define NUMA_MIGRATE_NOMEM    -3    /* Out of memory. */

/* Migration statistics. */
typedef struct {
    uint64_t total_migrations;      /* Completed migration count. */
    uint64_t bytes_migrated;        /* Total bytes migrated. */
    uint64_t failed_migrations;     /* Failed migration count. */
    uint64_t migration_time_us;     /* Total migration time (microseconds). */
} numa_migrate_stats_t;

/* Initialize the migration module. */
int numa_migrate_init(void);

/* Clean up the migration module and release resources. */
void numa_migrate_cleanup(void);

/* Migrate a memory block from the current node to the target node.
 *
 * @param ptr:         user pointer of the memory block (not the raw pointer)
 * @param size:        memory block size
 * @param target_node: target NUMA node ID
 * @return:            the new pointer on success, NULL on failure
 *
 * Note: after a successful migration the old pointer is invalid and must not be used.
 */
void *numa_migrate_memory(void *ptr, size_t size, int target_node);

/* Get the migration statistics. */
void numa_migrate_get_stats(numa_migrate_stats_t *stats);

/* Reset the migration statistics. */
void numa_migrate_reset_stats(void);

#endif /* NUMA_MIGRATE_H */
