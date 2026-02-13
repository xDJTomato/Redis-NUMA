/* numa_key_migrate.h - NUMA Key-level migration module
 *
 * This module provides Redis Key-level migration between NUMA nodes.
 * It tracks key access patterns and migrates hot keys to appropriate nodes
 * for optimized memory access latency.
 *
 * Key Features:
 * - Key-level granularity migration (robj as migration unit)
 * - LRU-integrated hotness tracking
 * - Type-specific migration adapters for all Redis data types
 * - Atomic pointer switching to ensure consistency
 */

#ifndef NUMA_KEY_MIGRATE_H
#define NUMA_KEY_MIGRATE_H

#include "server.h"
#include <stdint.h>
#include <pthread.h>

/* Return codes */
#define NUMA_KEY_MIGRATE_OK       0
#define NUMA_KEY_MIGRATE_ERR     -1
#define NUMA_KEY_MIGRATE_ENOENT  -2  /* Key not found */
#define NUMA_KEY_MIGRATE_EINVAL  -3  /* Invalid parameter */
#define NUMA_KEY_MIGRATE_ENOMEM  -4  /* Out of memory */
#define NUMA_KEY_MIGRATE_ETYPE   -5  /* Unsupported type */

/* Hotness levels (0-7) */
#define HOTNESS_MIN_LEVEL  0
#define HOTNESS_MAX_LEVEL  7
#define HOTNESS_DEFAULT    3
#define MIGRATION_HOTNESS_THRESHOLD  5

/* Heat decay threshold (10 seconds in LRU ticks) */
#define HEAT_DECAY_THRESHOLD  10000

/* Configuration */
#define DEFAULT_MIGRATE_THRESHOLD   5
#define DEFAULT_BATCH_SIZE          50

/* ========== Data Structures ========== */

/* Key NUMA metadata */
typedef struct {
    int current_node;               /* Current NUMA node */
    uint8_t hotness_level;          /* Hotness level (0-7) */
    uint16_t last_access_time;      /* Last access timestamp (LRU clock) */
    size_t memory_footprint;        /* Memory footprint in bytes */
    uint64_t access_count;          /* Total access count */
} key_numa_metadata_t;

/* Migration request */
typedef struct {
    robj *key_obj;                  /* Target key object */
    int source_node;                /* Source NUMA node */
    int target_node;                /* Target NUMA node */
    size_t data_size;               /* Data size to migrate */
    uint64_t start_time;            /* Migration start time (us) */
} migration_request_t;

/* Migration statistics */
typedef struct {
    uint64_t total_migrations;      /* Total migrations performed */
    uint64_t successful_migrations; /* Successful migrations */
    uint64_t failed_migrations;     /* Failed migrations */
    uint64_t total_bytes_migrated;  /* Total bytes migrated */
    uint64_t total_migration_time_us; /* Total migration time (us) */
    uint64_t peak_concurrent_migrations; /* Peak concurrent migrations */
} numa_key_migrate_stats_t;

/* Module context */
typedef struct {
    int initialized;                /* Initialization flag */
    dict *key_metadata;             /* Key metadata mapping table */
    pthread_mutex_t mutex;          /* Concurrency control lock */
    numa_key_migrate_stats_t stats; /* Migration statistics */
} numa_key_migrate_ctx_t;

/* ========== Core Interfaces ========== */

/* Module initialization and cleanup */
int numa_key_migrate_init(void);
void numa_key_migrate_cleanup(void);

/* Single key migration */
int numa_migrate_single_key(redisDb *db, robj *key, int target_node);

/* Batch migration */
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node);

/* Pattern-based migration */
int numa_migrate_keys_by_pattern(redisDb *db, const char *pattern, int target_node);

/* Database-level migration */
int numa_migrate_entire_database(redisDb *db, int target_node);

/* ========== Hotness Tracking ========== */

/* Record key access (called from lookupKey) */
void numa_record_key_access(robj *key, robj *val);

/* Perform heat decay (called periodically) */
void numa_perform_heat_decay(void);

/* ========== Metadata Management ========== */

/* Get key metadata */
key_numa_metadata_t* numa_get_key_metadata(robj *key);

/* Get key's current NUMA node */
int numa_get_key_current_node(robj *key);

/* ========== Statistics ========== */

/* Get migration statistics */
void numa_get_migration_statistics(numa_key_migrate_stats_t *stats);

/* Reset migration statistics */
void numa_reset_migration_statistics(void);

/* ========== Type-specific Migration Adapters ========== */

/* These are internal functions but exposed for testing */
int migrate_string_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node);

#endif /* NUMA_KEY_MIGRATE_H */
