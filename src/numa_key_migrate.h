/* numa_key_migrate.h - NUMA key-level migration module
 *
 * This module implements fine-grained migration of Redis keys between NUMA
 * nodes, tracking key access patterns to move hot keys to the best node and
 * reduce memory access latency.
 *
 * Main features:
 * - Key-level granularity migration (robj is the migration unit)
 * - LRU-integrated hotness tracking (staircase lazy decay)
 * - Dedicated migration adapters for each Redis data type
 * - Atomic pointer swaps ensure consistency during migration
 */

#ifndef NUMA_KEY_MIGRATE_H
#define NUMA_KEY_MIGRATE_H

#include "server.h"
#include <stdint.h>
#include <pthread.h>

/* Return codes. */
#define NUMA_KEY_MIGRATE_OK       0    /* Operation succeeded. */
#define NUMA_KEY_MIGRATE_ERR     -1    /* General error. */
#define NUMA_KEY_MIGRATE_ENOENT  -2    /* Key does not exist. */
#define NUMA_KEY_MIGRATE_EINVAL  -3    /* Invalid argument. */
#define NUMA_KEY_MIGRATE_ENOMEM  -4    /* Out of memory. */
#define NUMA_KEY_MIGRATE_ETYPE   -5    /* Unsupported data type. */

#define HOTNESS_MIN_LEVEL  0   /* Lowest hotness (cold data). */
#define HOTNESS_MAX_LEVEL  7   /* Highest hotness. */
#define HOTNESS_DEFAULT    3   /* Initial default hotness. */
#define MIGRATION_HOTNESS_THRESHOLD  5   /* Hotness threshold to trigger migration. */

/* Hotness decay threshold (LRU ticks, 10 seconds), used by the legacy periodic decay (kept for compatibility). */
#define HEAT_DECAY_THRESHOLD  10000

/* Staircase lazy-decay thresholds (LRU clock seconds, applied on every access).
 * Kept in sync with the constants in numa_composite_lru.h for unified hotness semantics. */
#define KEY_LAZY_DECAY_STEP1_SECS    10    /* Idle < 10s   : decay 0 (brief pause, exempt). */
#define KEY_LAZY_DECAY_STEP2_SECS    60    /* Idle < 60s   : decay 1. */
#define KEY_LAZY_DECAY_STEP3_SECS   300    /* Idle < 5min  : decay 2. */
#define KEY_LAZY_DECAY_STEP4_SECS  1800    /* Idle < 30min : decay 3. */
                                           /* Idle >= 30min: fully cleared. */

/* Configuration parameters. */
#define DEFAULT_MIGRATE_THRESHOLD   5    /* Default migration hotness threshold. */
#define DEFAULT_BATCH_SIZE          50   /* Default batch migration count. */

/* ========== Data structures ========== */

/* Key NUMA metadata. */
typedef struct {
    int current_node;               /* Current NUMA node. */
    uint8_t hotness_level;          /* Hotness level (0-7). */
    uint16_t last_access_time;      /* Last access time (LRU clock). */
    size_t memory_footprint;        /* Memory footprint in bytes. */
    uint64_t access_count;          /* Cumulative access count. */
} key_numa_metadata_t;

/* Migration request. */
typedef struct {
    robj *key_obj;                  /* Target key object. */
    int source_node;                /* Source NUMA node. */
    int target_node;                /* Target NUMA node. */
    size_t data_size;               /* Size of the data to migrate. */
    uint64_t start_time;            /* Migration start time (microseconds). */
} migration_request_t;

/* Migration statistics. */
typedef struct {
    uint64_t total_migrations;              /* Total migration count. */
    uint64_t successful_migrations;         /* Successful migration count. */
    uint64_t failed_migrations;             /* Failed migration count. */
    uint64_t total_bytes_migrated;          /* Total bytes migrated. */
    uint64_t total_migration_time_us;       /* Total migration time (microseconds). */
    uint64_t peak_concurrent_migrations;    /* Peak concurrent migrations. */
} numa_key_migrate_stats_t;

/* Module global context. */
typedef struct {
    int initialized;                /* Initialization flag. */
    dict *key_metadata;             /* Key metadata hash table (robj* -> metadata). */
    pthread_mutex_t mutex;          /* Concurrency control lock. */
    numa_key_migrate_stats_t stats; /* Migration statistics. */
} numa_key_migrate_ctx_t;

/* ========== Core interface ========== */

/* Module initialization and cleanup. */
int numa_key_migrate_init(void);
void numa_key_migrate_cleanup(void);

/* Single-key migration: migrate the given key to the target node. */
int numa_migrate_single_key(redisDb *db, robj *key, int target_node);

/* Migrate by key name (SDS): used by the composite_lru candidate pool. */
int numa_migrate_key_by_name(redisDb *db, const char *keyname, int target_node);

/* Get the representative data allocation base address of an object, used for locality stats and migration dedup. */
void *numa_object_sample_alloc_ptr(robj *val);

/* Estimate the migration cost of an object: convert the representative allocation size into strategy step budget units. */
size_t numa_object_sample_alloc_size(robj *val);
uint32_t numa_object_migration_cost_units(robj *val);

/* Batch migration: migrate all keys in the list to the target node. */
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node);

/* Pattern migration: migrate all keys matching the pattern to the target node. */
int numa_migrate_keys_by_pattern(redisDb *db, const char *pattern, int target_node);

/* Full-database migration: migrate the whole database to the target node. */
int numa_migrate_entire_database(redisDb *db, int target_node);

/* ========== Hotness tracking ========== */

/* Record a key access (called from lookupKey). */
void numa_record_key_access(robj *key, robj *val);

/* Apply hotness decay (called periodically). */
void numa_perform_heat_decay(void);

/* ========== Metadata management ========== */

/* Get the NUMA metadata of a key. */
key_numa_metadata_t* numa_get_key_metadata(robj *key);

/* Get the NUMA node where a key currently lives. */
int numa_get_key_current_node(robj *key);

/* Notify the NUMA module to clean up metadata when a key is deleted (prevents memory leaks). */
void numa_on_key_delete(robj *key);

/* ========== Statistics ========== */

/* Get the migration statistics. */
void numa_get_migration_statistics(numa_key_migrate_stats_t *stats);

/* Reset the migration statistics. */
void numa_reset_migration_statistics(void);

/* ========== Per-type migration adapters (internal, exposed for testing) ========== */
int migrate_string_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node);
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node);

/* ========== Redis command interface ========== */

/* Query whether the module is initialized (used by numa_command.c). */
int numa_key_migrate_is_initialized(void);

#endif /* NUMA_KEY_MIGRATE_H */
