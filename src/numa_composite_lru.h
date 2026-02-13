/* numa_composite_lru.h - NUMA Composite LRU Strategy (Slot 1)
 *
 * This module implements the default NUMA migration strategy based on
 * Redis's native LRU mechanism. It provides stability-oriented hotness
 * management and intelligent migration triggering.
 *
 * Key Features:
 * - Stability-first: Avoids frequent hotness fluctuations
 * - Resource-aware: Considers node load and bandwidth
 * - Gradual decay: Uses stability counter to prevent misjudgment
 * - Deep integration with Key Migration module
 */

#ifndef NUMA_COMPOSITE_LRU_H
#define NUMA_COMPOSITE_LRU_H

#include "numa_strategy_slots.h"
#include "dict.h"
#include "adlist.h"
#include <stdint.h>

/* ========== Configuration Constants ========== */

/* Core parameters */
#define COMPOSITE_LRU_DEFAULT_DECAY_THRESHOLD    10000000  /* 10 seconds (us) */
#define COMPOSITE_LRU_DEFAULT_STABILITY_COUNT    3         /* 3 consecutive misses */
#define COMPOSITE_LRU_DEFAULT_MIGRATE_THRESHOLD  5         /* Hotness >= 5 triggers */

/* Resource thresholds */
#define COMPOSITE_LRU_DEFAULT_OVERLOAD_THRESHOLD     0.8   /* 80% memory usage */
#define COMPOSITE_LRU_DEFAULT_BANDWIDTH_THRESHOLD    0.9   /* 90% bandwidth */
#define COMPOSITE_LRU_DEFAULT_PRESSURE_THRESHOLD     0.7   /* 70% migration pressure */

/* Performance parameters */
#define COMPOSITE_LRU_MAX_PENDING_MIGRATIONS     1000      /* Max pending migrations */
#define COMPOSITE_LRU_PENDING_TIMEOUT            30000000  /* 30 seconds timeout (us) */

/* Hotness levels */
#define COMPOSITE_LRU_HOTNESS_MAX     7
#define COMPOSITE_LRU_HOTNESS_MIN     0

/* Resource status */
#define RESOURCE_AVAILABLE              0
#define RESOURCE_OVERLOADED             1
#define RESOURCE_BANDWIDTH_SATURATED    2
#define RESOURCE_MIGRATION_PRESSURE     3

/* ========== Data Structures ========== */

/* Key heat information */
typedef struct {
    uint8_t  hotness;                   /* Hotness level (0-7) */
    uint8_t  stability_counter;         /* Stability counter */
    uint16_t last_access;               /* Last access time (LRU_CLOCK low 16 bits) */
    uint64_t access_count;              /* Total access count */
    int      current_node;              /* Current NUMA node */
    int      preferred_node;            /* Preferred target node for migration */
} composite_lru_heat_info_t;

/* Pending migration entry */
typedef struct {
    void    *key;                       /* Key pointer (robj*) */
    int      target_node;               /* Target node */
    uint64_t enqueue_time;              /* Time when enqueued */
    uint8_t  priority;                  /* Migration priority */
} pending_migration_t;

/* Strategy private data */
typedef struct {
    /* Heat control parameters */
    uint32_t decay_threshold;           /* Decay threshold (us) */
    uint8_t  stability_count;           /* Stability counter threshold */
    uint8_t  migrate_hotness_threshold; /* Migration hotness threshold */
    
    /* Resource monitoring parameters */
    double   overload_threshold;        /* Node overload threshold (0.0-1.0) */
    double   bandwidth_threshold;       /* Bandwidth saturation threshold */
    double   pressure_threshold;        /* Migration pressure threshold */
    
    /* Internal state */
    uint64_t last_decay_time;           /* Last decay execution time */
    dict    *key_heat_map;              /* Key heat map */
    list    *pending_migrations;        /* Pending migration queue */
    
    /* Statistics */
    uint64_t heat_updates;              /* Heat update count */
    uint64_t migrations_triggered;      /* Migrations triggered */
    uint64_t decay_operations;          /* Decay operations count */
    uint64_t migrations_completed;      /* Completed migrations */
    uint64_t migrations_failed;         /* Failed migrations */
    uint64_t pending_timeouts;          /* Pending migration timeouts */
} composite_lru_data_t;

/* ========== Public Interfaces ========== */

/* Module initialization - register strategy factory */
int numa_composite_lru_register(void);

/* Strategy factory functions (exposed for testing) */
numa_strategy_t* composite_lru_create(void);
void composite_lru_destroy(numa_strategy_t *strategy);

/* Strategy operations (exposed for testing) */
int composite_lru_init(numa_strategy_t *strategy);
int composite_lru_execute(numa_strategy_t *strategy);
void composite_lru_cleanup(numa_strategy_t *strategy);

/* Heat management */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val);
void composite_lru_decay_heat(composite_lru_data_t *data);

/* Statistics query */
void composite_lru_get_stats(numa_strategy_t *strategy, 
                             uint64_t *heat_updates,
                             uint64_t *migrations_triggered,
                             uint64_t *decay_operations);

#endif /* NUMA_COMPOSITE_LRU_H */
