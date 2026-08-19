/* numa_configurable_strategy.h - configurable NUMA allocation strategy interface
 *
 * Provides a runtime-configurable NUMA memory allocation policy, adjustable via config file and command-line directives.
 */

#ifndef NUMA_CONFIGURABLE_STRATEGY_H
#define NUMA_CONFIGURABLE_STRATEGY_H

#include <stddef.h>
#include <stdint.h>
#include "zmalloc.h"
#include "atomicvar.h"
#include <stddef.h>

/* Configurable NUMA strategy types.
 * 9 enum values, but only 7 independent behaviors: WEIGHTED_INTERLEAVE
 * shares its node-selection code with WEIGHTED (select_weighted_node() in
 * numa_configurable_strategy.c, differing only in weight source), and
 * ADAPTIVE/LATENCY_AWARE are kernel-side placeholders that behave as
 * LOCAL_FIRST - their real implementation is the matching alloc_adaptive/
 * alloc_latency_aware atomic op in NUMAflow (numaflow/src/nf_ops.c),
 * reachable via NUMA FLOW LOAD, not the zmalloc hot path. */
typedef enum {
    NUMA_STRATEGY_CONFIG_LOCAL_FIRST = 0,    /* Local-first policy. */
    NUMA_STRATEGY_CONFIG_INTERLEAVE,         /* Interleaved allocation policy. */
    NUMA_STRATEGY_CONFIG_ROUND_ROBIN,        /* Round-robin allocation policy. */
    NUMA_STRATEGY_CONFIG_WEIGHTED,           /* Weighted allocation policy. */
    NUMA_STRATEGY_CONFIG_PRESSURE_AWARE,     /* Pressure-aware policy. */
    NUMA_STRATEGY_CONFIG_CXL_OPTIMIZED,      /* CXL-optimized policy. */
    NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE,/* Pressure-aware weighted interleave policy. */
    NUMA_STRATEGY_CONFIG_ADAPTIVE,           /* Adaptive policy (to be implemented). */
    NUMA_STRATEGY_CONFIG_LATENCY_AWARE,      /* Latency-aware policy (to be implemented). */
    NUMA_STRATEGY_CONFIG_COUNT               /* Sentinel for the total strategy count. */
} numa_config_strategy_type_t;

/* Strategy configuration parameters. */
typedef struct {
    numa_config_strategy_type_t strategy_type;  /* Strategy type. */
    int *node_weights;                          /* Per-node weight array. */
    int num_nodes;                              /* Number of nodes. */
    double balance_threshold;                   /* Balance threshold. */
    int enable_cxl_optimization;                /* Whether CXL optimization is enabled. */
    size_t min_allocation_size;                 /* Minimum allocation size. */
    int auto_rebalance;                         /* Whether auto-rebalance is enabled. */
    uint64_t rebalance_interval_us;             /* Rebalance interval. */
    uint64_t enabled_nodes_mask;                 /* Mask of NUMA nodes allowed for auto allocation, 0=all. */
} numa_strategy_config_t;

/* Runtime policy state. */
typedef struct {
    numa_strategy_config_t config;
    int current_strategy;                       /* Currently used strategy. */
    uint64_t last_rebalance_time;               /* Last rebalance time. */
    redisAtomic int *allocation_counters;        /* Per-node allocation counters (atomic, lock-free updates). */
    redisAtomic size_t *bytes_allocated_per_node; /* Per-node allocated bytes (atomic, lock-free updates). */
    redisAtomic int *pressure_weights;           /* Pressure weight array (atomic updates, lock-free reads on the allocation path). */
    redisAtomic int *bw_usage_percent;           /* Per-node bandwidth utilization 0-100 (atomic updates, lock-free reads on the migration path). */
    int cpu_node;                                /* NUMA node of the Redis main-thread CPU. */
    double *distance_factors;                    /* Per-node distance factors (computed once at startup). */
} numa_runtime_state_t;

/* ========== Configuration management API ========== */

/* Initialize the configurable strategy system. */
int numa_config_strategy_init(void);

/* Clean up the strategy system. */
void numa_config_strategy_cleanup(void);

/* Load the strategy configuration from a file. */
int numa_config_load_from_file(const char *config_file);

/* Apply the strategy configuration. */
int numa_config_apply_strategy(const numa_strategy_config_t *config);

/* Get the current strategy configuration. */
const numa_strategy_config_t* numa_config_get_current(void);

/* ========== Runtime control API ========== */

/* Set the NUMA allocation strategy. */
int numa_config_set_strategy(numa_config_strategy_type_t strategy);

/* Set the node weights. */
int numa_config_set_node_weights(int *weights, int num_nodes);

/* Set the auto-allocation node mask; mask=0 enables all nodes. */
int numa_config_set_enabled_nodes_mask(uint64_t mask);
uint64_t numa_config_get_enabled_nodes_mask(void);

/* Enable/disable CXL optimization. */
int numa_config_set_cxl_optimization(int enable);

/* Set the balance threshold. */
int numa_config_set_balance_threshold(double threshold);

/* Manually trigger a rebalance. */
int numa_config_trigger_rebalance(void);

/* Update the pressure weights (called by serverCron every second). */
void numa_config_update_pressure_weights(void);

/* Get the cached bandwidth utilization (0-100), lock-free reads on the allocation/migration path. */
int numa_config_get_cached_bw(int node);

/* Get the cached pressure weight, lock-free reads on the allocation path. */
int numa_config_get_cached_pressure_weight(int node);

/* ========== Memory allocation API ========== */

/* Smart memory allocation - picks the best strategy according to the current configuration. */
void *numa_config_malloc(size_t size);

/* Smart zeroed allocation. */
void *numa_config_calloc(size_t nmemb, size_t size);

/* Allocate on a given node. */
void *numa_config_malloc_onnode(size_t size, int node);

/* ========== Query and statistics API ========== */

/* Get the strategy execution statistics. */
void numa_config_get_statistics(uint64_t *allocations_per_node, 
                               size_t *bytes_per_node,
                               int num_nodes);

/* Get the node load info. */
double numa_config_get_node_utilization(int node_id);

/* Check whether a rebalance is needed. */
int numa_config_needs_rebalance(void);

/* Get the best allocation node. */
int numa_config_get_best_node(size_t size);

/* ========== Command-line interface ========== */

/* Handle NUMA configuration commands. */
int numa_config_handle_command(int argc, char **argv);

/* Show the current configuration state. */
void numa_config_show_status(void);

/* Show help. */
void numa_config_show_help(void);

#endif /* NUMA_CONFIGURABLE_STRATEGY_H */
