/* numa_composite_lru.h - NUMA composite LRU strategy (slot 1, default policy).
 *
 * This module implements the default NUMA migration policy based on Redis's
 * native LRU mechanism, providing a dual-channel migration decision scheme of
 * a hot candidate pool plus a progressive scan:
 *   - Access path (record_access): only updates PREFIX hotness, no enqueue overhead
 *   - Candidate pool (fast channel): first threshold crossing on access writes to the
 *     ring buffer, processed first by serverCron
 *   - Progressive scan (fallback channel): serverCron scans scan_batch_size keys per
 *     run, advancing a cursor
 *   - Execution always re-reads the current PREFIX hotness, never relying on snapshots
 * All parameters can be set through a JSON config file and hot-updated at runtime via
 * NUMAMIGRATE CONFIG LOAD.
 */

#ifndef NUMA_COMPOSITE_LRU_H
#define NUMA_COMPOSITE_LRU_H

#include "sds.h"
#include "numa_strategy_slots.h"
#include "dict.h"
#include <stdint.h>

/* Forward declaration to avoid a circular dependency via server.h. */
typedef struct redisDb redisDb;

/* ========== Staircase decay constants (not configurable via JSON) ========== */
#define LAZY_DECAY_STEP1_SECS    10    /* Idle < 10s   : decay 0 (brief pause, fully exempt). */
#define LAZY_DECAY_STEP2_SECS    60    /* Idle < 60s   : decay 1. */
#define LAZY_DECAY_STEP3_SECS   300    /* Idle < 5min  : decay 2. */
#define LAZY_DECAY_STEP4_SECS  1800    /* Idle < 30min : decay 3. */
                                       /* Idle >= 30min: fully cleared. */

/* Hotness level range. */
#define COMPOSITE_LRU_HOTNESS_MAX     7
#define COMPOSITE_LRU_HOTNESS_MIN     0

/* Resource status codes. */
#define RESOURCE_AVAILABLE              0
#define RESOURCE_OVERLOADED             1
#define RESOURCE_BANDWIDTH_SATURATED    2
#define RESOURCE_MIGRATION_PRESSURE     3

/* ========== Configurable parameter struct (corresponds to the JSON file) ========== */
typedef struct {
    uint32_t decay_threshold_sec;       /* Periodic decay interval (seconds), default 10. */
    uint8_t  migrate_hotness_threshold; /* Hotness threshold to trigger migration, default 5. */
    uint8_t  stability_count;           /* Dictionary-path stability count threshold, default 3. */
    uint32_t hot_candidates_size;       /* Candidate pool ring buffer capacity, default 256. */
    uint32_t scan_batch_size;           /* Keys scanned per serverCron run, default 2500. */
    uint32_t migration_rate_multiplier; /* Base migration rate multiplier, default 5. */
    double   overload_threshold;        /* Node memory overload threshold (0~1), default 0.8. */
    double   bandwidth_threshold;       /* Bandwidth saturation threshold (0~1), default 0.9. */
    double   pressure_threshold;        /* Migration pressure threshold (0~1), default 0.7. */
    int      auto_migrate_enabled;      /* 1=enable background auto migration, 0=manual only, default 1. */
    int      access_tracking_enabled;   /* 1=track access hotness/candidates, 0=skip hot-path stats. */
    int      locality_stats_enabled;    /* 1=count local/remote accesses, 0=maintain migration hotness only. */
    int      debug_logging_enabled;     /* 1=print migration debug logs, 0=off. */
} composite_lru_config_t;

/* ========== Data structures ========== */

/* Key hotness info (dictionary fallback path). */
typedef struct {
    uint8_t  hotness;                   /* Hotness level (0-7). */
    uint8_t  stability_counter;         /* Stability counter. */
    uint16_t last_access;               /* Last access time (low 16 bits of LRU_CLOCK). */
    uint64_t access_count;              /* Cumulative access count. */
    int      current_node;              /* Current NUMA node. */
    int      preferred_node;            /* Migration target node. */
} composite_lru_heat_info_t;

/* Hot candidate pool entry (ring buffer element). */
typedef struct {
    sds      key;                       /* Key name (SDS). */
    void    *val;                       /* robj pointer (used to re-read PREFIX hotness). */
    void    *data_ptr;                  /* Data pointer (used to detect the physical NUMA node). */
    int      target_node;               /* Target node at write time (the CPU's node). */
    uint8_t  hotness_snapshot;          /* Hotness snapshot at write time (priority sorting only; re-read before executing). */
} hot_candidate_t;

/* Strategy private data. */
typedef struct {
    /* Database context (used for actual migration calls). */
    redisDb *db;

    /* Runtime configuration (loaded from JSON). */
    composite_lru_config_t config;

    /* Hot candidate pool (ring buffer, fast channel). */
    hot_candidate_t *hot_candidates;    /* Size = config.hot_candidates_size. */
    uint32_t  candidates_head;          /* Write cursor (mod size selects the slot, overwrites the oldest). */
    uint32_t  candidates_tail;          /* Consume cursor (points to the oldest valid candidate). */
    uint32_t  candidates_count;         /* Current valid count (at most hot_candidates_size). */

    /* Progressive scan cursor (fallback channel). */
    dictIterator *scan_iter;            /* Current scan position, NULL means the next round starts over. */

    /* Internal state. */
    uint64_t last_decay_time;           /* Last periodic decay time (microseconds). */
    dict    *key_heat_map;              /* Dictionary fallback heat map. */

    /* Statistics. */
    uint64_t heat_updates;              /* Number of hotness updates. */
    uint64_t migrations_triggered;      /* Number of migrations triggered. */
    uint64_t decay_operations;          /* Number of decay operations. */
    uint64_t migrations_completed;      /* Number of completed migrations. */
    uint64_t migrations_failed;         /* Number of failed migrations. */
    uint64_t candidates_written;        /* Number of writes to the candidate pool. */
    uint64_t scan_keys_checked;         /* Cumulative keys checked by the progressive scan. */
    uint64_t migrations_bw_blocked;     /* Migrations blocked by bandwidth saturation. */
    uint64_t migrations_overloaded;     /* Migrations blocked by node memory overload. */
    uint64_t accesses_local;            /* Accesses to data on the local node (DRAM). */
    uint64_t accesses_remote;           /* Accesses to data on a remote node (CXL). */
} composite_lru_data_t;

/* ========== Public interface ========== */

/* Module initialization: register the factory with the strategy manager. */
int numa_composite_lru_register(void);

/* Main-thread binding: call in main() to pin the migration target node to the main thread's node. */
void composite_lru_set_main_thread(void);

/* Strategy factory function. */
numa_strategy_t* composite_lru_create(void);
void composite_lru_destroy(numa_strategy_t *strategy);

/* Strategy operation functions. */
int  composite_lru_init(numa_strategy_t *strategy);
int  composite_lru_execute(numa_strategy_t *strategy);
int  composite_lru_execute_step(numa_strategy_t *strategy, uint64_t deadline_us, uint32_t budget);
void composite_lru_cleanup(numa_strategy_t *strategy);

/* Hotness management. */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val, void *data_ptr, uint16_t lru_clock);
void composite_lru_decay_heat(composite_lru_data_t *data);

/* JSON configuration loading and application. */
int  composite_lru_load_config(const char *path, composite_lru_config_t *out);
int  composite_lru_apply_config(numa_strategy_t *strategy, const composite_lru_config_t *cfg);
int  composite_lru_set_config(numa_strategy_t *strategy, const char *key, const char *value);
int  composite_lru_get_config(numa_strategy_t *strategy, const char *key, char *buf, size_t buf_len);
void composite_lru_config_defaults(composite_lru_config_t *cfg);

/* Manually trigger one progressive scan round (called by NUMAMIGRATE SCAN). */
int  composite_lru_scan_once(numa_strategy_t *strategy, uint32_t batch_size,
                             uint64_t *scanned_out, uint64_t *migrated_out);

/* Statistics queries. */
void composite_lru_get_stats(numa_strategy_t *strategy,
                             uint64_t *heat_updates,
                             uint64_t *migrations_triggered,
                             uint64_t *decay_operations);

#endif /* NUMA_COMPOSITE_LRU_H */
