/* numa_balanced_allocator.h - balanced NUMA memory allocator
 *
 * Solves the memory allocation imbalance problem in CXL environments by implementing smart load balancing across NUMA nodes.
 */

#ifndef NUMA_BALANCED_ALLOCATOR_H
#define NUMA_BALANCED_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

/* Load balancing strategies. */
#define BALANCE_STRATEGY_ROUND_ROBIN    0  /* Round-robin allocation. */
#define BALANCE_STRATEGY_WEIGHTED       1  /* Weighted allocation. */
#define BALANCE_STRATEGY_PRESSURE_BASED 2  /* Pressure-aware allocation. */

/* Node weight configuration. */
typedef struct {
    int node_id;
    int weight;              /* Allocation weight (1-100). */
    size_t reserved_memory;  /* Reserved memory size. */
    int cxl_distance;        /* CXL access latency class. */
} node_weight_config_t;

/* Load balancing configuration. */
typedef struct {
    int strategy;                    /* Allocation strategy. */
    node_weight_config_t *weights;   /* Node weight array. */
    int num_weights;                 /* Weight config count. */
    double balance_threshold;        /* Load imbalance threshold. */
    uint64_t rebalance_interval_us;  /* Rebalance interval. */
    int enable_cxl_optimization;     /* Whether CXL optimization is enabled. */
} balance_config_t;

/* Node load statistics. */
typedef struct {
    int node_id;
    size_t total_memory;
    size_t used_memory;
    size_t free_memory;
    double utilization_rate;
    uint64_t allocation_count;
    uint64_t bytes_allocated;
    int cxl_latency_class;  /* CXL latency class. */
} node_load_stats_t;

/* Balanced allocator context. */
typedef struct {
    balance_config_t config;
    node_load_stats_t *node_stats;
    int num_nodes;
    int initialized;
    int current_rr_index;    /* Round-robin index. */
    uint64_t last_rebalance;
} balanced_allocator_t;

/* ========== Public API ========== */

/* Initialize the balanced allocator. */
int numa_balanced_init(const balance_config_t *config);

/* Clean up the balanced allocator. */
void numa_balanced_cleanup(void);

/* Smart memory allocation - picks the best node according to the load. */
void *numa_balanced_malloc(size_t size);

/* Smart zeroed memory allocation. */
void *numa_balanced_calloc(size_t nmemb, size_t size);

/* Allocate memory on a given node. */
void *numa_balanced_malloc_onnode(size_t size, int node);

/* Get the best node for allocation. */
int numa_balanced_get_best_node(size_t size);

/* Update the node load statistics. */
int numa_balanced_update_stats(void);

/* Check whether a rebalance is needed. */
int numa_balanced_need_rebalance(void);

/* Perform a load rebalance. */
int numa_balanced_rebalance(void);

/* Get the load info of a node. */
const node_load_stats_t* numa_balanced_get_node_stats(int node_id);

/* Get the load info of all nodes. */
const node_load_stats_t* numa_balanced_get_all_stats(int *num_nodes);

/* Dynamically adjust the weights. */
int numa_balanced_adjust_weight(int node_id, int new_weight);

/* Set the CXL optimization parameters. */
int numa_balanced_set_cxl_params(int enable_optimization, int latency_threshold_ms);

#endif /* NUMA_BALANCED_ALLOCATOR_H */
