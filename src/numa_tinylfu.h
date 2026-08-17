/* numa_tinylfu.h - TinyLFU hot data fast discovery and migration strategy (slot 2).
 *
 * Based on Caffeine's TinyLFU algorithm, using a Count-Min Sketch plus a
 * Doorkeeper Bloom Filter to estimate access frequency and discover hot data
 * in O(1) with a very low memory footprint (~50KB).
 *
 * Key differences from Composite LRU (Slot 1):
 *   - Frequency rather than hotness: CMS directly counts accesses, no staircase decay
 *   - Global decay: all counters are halved periodically (not per-key lazy decay)
 *   - Doorkeeper: filters one-time accesses to avoid counter pollution
 *   - Fixed memory: CMS + Bloom Filter sizes are fixed, independent of key count
 */

#ifndef NUMA_TINYLFU_H
#define NUMA_TINYLFU_H

#include "sds.h"
#include "numa_strategy_slots.h"
#include <stdint.h>

typedef struct redisDb redisDb;

/* ========== Count-Min Sketch parameters ========== */
#define TINYLFU_CMS_DEPTH       4
#define TINYLFU_CMS_WIDTH_DEFAULT  16384   /* Must be a power of two. */
#define TINYLFU_COUNTER_MAX     15         /* 4-bit counter maximum. */

/* ========== Doorkeeper Bloom Filter parameters ========== */
#define TINYLFU_DK_HASH_COUNT   2

/* ========== Default configuration ========== */
#define TINYLFU_DEFAULT_MIGRATE_THRESHOLD   2
#define TINYLFU_DEFAULT_RESET_INTERVAL      50000
#define TINYLFU_DEFAULT_RING_SIZE           1024
#define TINYLFU_DEFAULT_MIGRATION_BUDGET    512

/* ========== Count-Min Sketch ========== */
typedef struct {
    uint8_t *rows[TINYLFU_CMS_DEPTH]; /* 4-bit packed: 2 counters/byte */
    uint32_t width;                    /* Number of columns (power of two). */
    uint32_t width_mask;               /* width - 1, for fast modulo. */
    uint32_t bytes_per_row;            /* width / 2 */
} tinylfu_cms_t;

/* ========== Doorkeeper Bloom Filter ========== */
typedef struct {
    uint8_t *bits;
    uint32_t num_bits;                 /* Bit count = CMS_DEPTH * width. */
    uint32_t num_bytes;
} tinylfu_doorkeeper_t;

/* ========== Migration candidate entry ========== */
typedef struct {
    sds      key;
    void    *val;
    void    *data_ptr;
    int      target_node;
    uint8_t  freq_snapshot;
    uint32_t cost_units;
} tinylfu_candidate_t;

/* ========== Configurable parameters ========== */
typedef struct {
    uint32_t cms_width;                /* CMS column count. */
    uint8_t  migrate_threshold;        /* Minimum frequency to trigger migration. */
    uint32_t reset_interval;           /* Global decay runs every N accesses. */
    uint32_t ring_size;                /* Candidate ring buffer size. */
    uint32_t migration_budget;         /* Max migrations per serverCron run. */
    int      auto_migrate_enabled;     /* 1=enable auto migration. */
    int      debug_logging_enabled;
} tinylfu_config_t;

/* ========== Strategy private data ========== */
typedef struct {
    redisDb *db;

    tinylfu_config_t config;

    /* Count-Min Sketch */
    tinylfu_cms_t cms;

    /* Doorkeeper Bloom Filter */
    tinylfu_doorkeeper_t doorkeeper;

    /* Global operation counter (triggers decay). */
    uint64_t total_ops;

    /* Candidate ring buffer. */
    tinylfu_candidate_t *ring;
    uint32_t ring_head;
    uint32_t ring_tail;
    uint32_t ring_count;

    /* Statistics. */
    uint64_t stat_accesses;            /* Total access count. */
    uint64_t stat_doorkeeper_filtered; /* Filtered by the doorkeeper (one-time accesses). */
    uint64_t stat_candidates_enqueued; /* Enqueued candidate count. */
    uint64_t stat_migrations_done;     /* Completed migration count. */
    uint64_t stat_migrations_failed;   /* Failed migration count. */
    uint64_t stat_resets;              /* Global decay count. */
    uint64_t stat_accesses_local;
    uint64_t stat_accesses_remote;
    uint64_t stat_accesses_node0;
    uint64_t stat_accesses_node1;
    uint64_t stat_accesses_node2;
    uint64_t stat_accesses_node3;
    uint64_t stat_accesses_unknown;
} tinylfu_data_t;

/* ========== Public interface ========== */

/* Register the factory with the strategy manager. */
int numa_tinylfu_register(void);

/* Record an access (called from lookupKey). */
void tinylfu_record_access(numa_strategy_t *strategy, void *key,
                           void *val, void *data_ptr);

/* Strategy factory function. */
numa_strategy_t* tinylfu_create(void);
void tinylfu_destroy(numa_strategy_t *strategy);

/* Set the main-thread NUMA node (called from main()). */
void tinylfu_set_main_thread_node(int node);

/* vtable implementation. */
int  tinylfu_init(numa_strategy_t *strategy);
int  tinylfu_execute(numa_strategy_t *strategy);
int  tinylfu_execute_step(numa_strategy_t *strategy, uint64_t deadline_us, uint32_t budget);
void tinylfu_cleanup(numa_strategy_t *strategy);

#endif /* NUMA_TINYLFU_H */
