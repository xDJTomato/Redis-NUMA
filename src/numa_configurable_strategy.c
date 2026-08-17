/* numa_configurable_strategy.c - configurable NUMA allocation strategy implementation. */

#define _GNU_SOURCE
#include "numa_configurable_strategy.h"
#include "zmalloc.h"
#include "server.h"
#include "evict.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <math.h>
#include <numa.h>

/* Global runtime state. */
static numa_runtime_state_t g_runtime_state = {0};
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

/* Strategy name mapping. */
static const char* strategy_names[] = {
    "local_first",
    "interleaved",
    "round_robin",
    "weighted",
    "pressure_aware",
    "cxl_optimized",
    "weighted_interleave",
    "adaptive",
    "latency_aware"
};

/* Get the strategy name. */
const char* get_strategy_name(numa_config_strategy_type_t strategy) {
    if (strategy >= 0 && strategy < (int)(sizeof(strategy_names)/sizeof(strategy_names[0]))) {
        return strategy_names[strategy];
    }
    return "unknown";
}

/* Parse a strategy name. */
numa_config_strategy_type_t parse_strategy_name(const char* name) {
    for (size_t i = 0; i < sizeof(strategy_names)/sizeof(strategy_names[0]); i++) {
        if (strcasecmp(name, strategy_names[i]) == 0) {
            return (numa_config_strategy_type_t)i;
        }
    }
    return (numa_config_strategy_type_t)-1; /* Unknown strategy names return -1 to signal an error. */
}

/* Initialize the runtime state. */
static int init_runtime_state(int num_nodes) {
    if (g_runtime_state.config.node_weights) {
        zfree(g_runtime_state.config.node_weights);
    }
    if (g_runtime_state.allocation_counters) {
        zfree(g_runtime_state.allocation_counters);
    }
    if (g_runtime_state.bytes_allocated_per_node) {
        zfree(g_runtime_state.bytes_allocated_per_node);
    }
    if (g_runtime_state.pressure_weights) {
        zfree(g_runtime_state.pressure_weights);
    }
    if (g_runtime_state.bw_usage_percent) {
        zfree(g_runtime_state.bw_usage_percent);
    }
    if (g_runtime_state.distance_factors) {
        zfree(g_runtime_state.distance_factors);
    }

    memset(&g_runtime_state, 0, sizeof(g_runtime_state));
    
    g_runtime_state.config.num_nodes = num_nodes;
    /* Default policy: pressure-aware weighted interleave (consistent with the
     * README / docs/new design docs; node weights are refreshed every second
     * by numa_config_update_pressure_weights()). */
    g_runtime_state.config.strategy_type = NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE;
    g_runtime_state.config.balance_threshold = 0.3;
    g_runtime_state.config.auto_rebalance = 1;
    g_runtime_state.config.enable_cxl_optimization = 1;
    g_runtime_state.config.min_allocation_size = 1024;
    g_runtime_state.config.rebalance_interval_us = 5000000; /* 5 seconds. */
    g_runtime_state.config.enabled_nodes_mask = 0;

    /* Allocate the weight arrays (weights need no atomicity; only the WEIGHTED strategy reads them under lock). */
    g_runtime_state.config.node_weights = zcalloc(num_nodes * sizeof(int));
    /* Atomic counter arrays: element types are redisAtomic int/size_t. */
    g_runtime_state.allocation_counters = zcalloc(num_nodes * sizeof(redisAtomic int));
    g_runtime_state.bytes_allocated_per_node = zcalloc(num_nodes * sizeof(redisAtomic size_t));
    g_runtime_state.pressure_weights = zcalloc(num_nodes * sizeof(redisAtomic int));
    g_runtime_state.bw_usage_percent = zcalloc(num_nodes * sizeof(redisAtomic int));

    if (!g_runtime_state.config.node_weights ||
        !g_runtime_state.allocation_counters ||
        !g_runtime_state.bytes_allocated_per_node ||
        !g_runtime_state.pressure_weights ||
        !g_runtime_state.bw_usage_percent) {
        return C_ERR;
    }
    
    /* Initialize the default weights. */
    for (int i = 0; i < num_nodes; i++) {
        g_runtime_state.config.node_weights[i] = 100; /* Default static weight 100. */
        atomicSet(g_runtime_state.pressure_weights[i], 100); /* Default pressure weight 100. */
    }

    /* Compute the NUMA distance factors: sqrt(min_dist / dist) damping.
     * Node 0 (dist=10): 1.0, Node 1 CXL (dist=50): ~0.45
     * At zero pressure DRAM gets ~69% and CXL ~31%, leaving headroom for the migration system. */
    int cpu_node = numa_node_of_cpu(sched_getcpu());
    if (cpu_node < 0) cpu_node = 0;
    g_runtime_state.cpu_node = cpu_node;

    g_runtime_state.distance_factors = zcalloc(num_nodes * sizeof(double));
    if (g_runtime_state.distance_factors) {
        int min_dist = numa_distance(cpu_node, cpu_node);
        if (min_dist <= 0) min_dist = 10;
        for (int i = 0; i < num_nodes; i++) {
            int d = numa_distance(cpu_node, i);
            if (d <= 0) d = min_dist;
            g_runtime_state.distance_factors[i] = sqrt((double)min_dist / d);
            if (g_runtime_state.distance_factors[i] < 0.01)
                g_runtime_state.distance_factors[i] = 0.01;
        }
        serverLog(LL_NOTICE,
            "[NUMA Config] Distance factors (cpu_node=%d): node0=%.2f, node1=%.2f",
            cpu_node,
            num_nodes > 0 ? g_runtime_state.distance_factors[0] : 0,
            num_nodes > 1 ? g_runtime_state.distance_factors[1] : 0);
    }

    return C_OK;
}

static int node_enabled(int node, int num_nodes) {
    uint64_t mask = g_runtime_state.config.enabled_nodes_mask;
    if (node < 0 || node >= num_nodes) return 0;
    if (mask == 0) return 1;
    return (mask & (1ULL << node)) != 0;
}

static int first_enabled_node(int num_nodes) {
    for (int i = 0; i < num_nodes && i < 64; i++) {
        if (node_enabled(i, num_nodes)) return i;
    }
    return 0;
}

static int next_enabled_node(int start, int num_nodes) {
    if (num_nodes <= 0) return 0;
    for (int i = 0; i < num_nodes && i < 64; i++) {
        int node = (start + i) % num_nodes;
        if (node_enabled(node, num_nodes)) return node;
    }
    return 0;
}

static int count_enabled_nodes(int num_nodes) {
    int count = 0;
    for (int i = 0; i < num_nodes && i < 64; i++) {
        if (node_enabled(i, num_nodes)) count++;
    }
    return count ? count : 1;
}

static int nth_enabled_node(int nth, int num_nodes) {
    int seen = 0;
    for (int i = 0; i < num_nodes && i < 64; i++) {
        if (!node_enabled(i, num_nodes)) continue;
        if (seen++ == nth) return i;
    }
    return first_enabled_node(num_nodes);
}

/* Select the best allocation node.
 * Optimization: most strategies no longer hold the g_config_mutex global lock.
 *   - LOCAL_FIRST / INTERLEAVE / ROUND_ROBIN / CXL_OPTIMIZED: fully lock-free
 *   - WEIGHTED: briefly locks to copy the weight array; scoring happens outside the lock
 *   - PRESSURE_AWARE: lock-free (reads external node utilization data)
 * Statistics counters are updated lock-free with Redis atomicIncr/atomicGet (atomicvar.h).
 * Config fields (strategy_type/num_nodes) change rarely and are read without a lock:
 * even a transient value only affects the target node of a single allocation, with no
 * correctness risk. */
static int select_best_node(size_t size) {
    int strategy_type = g_runtime_state.config.strategy_type;
    int num_nodes     = g_runtime_state.config.num_nodes;
    int selected_node = first_enabled_node(num_nodes);

    switch (strategy_type) {
        case NUMA_STRATEGY_CONFIG_LOCAL_FIRST:
            selected_node = node_enabled(0, num_nodes) ? 0 : first_enabled_node(num_nodes);
            break;

        case NUMA_STRATEGY_CONFIG_INTERLEAVE: {
            static __thread unsigned int seed = 0;
            if (seed == 0) seed = getpid() ^ pthread_self();
            selected_node = nth_enabled_node(rand_r(&seed) % count_enabled_nodes(num_nodes), num_nodes);
            break;
        }

        case NUMA_STRATEGY_CONFIG_ROUND_ROBIN: {
            static __thread int rr_index = 0;
            selected_node = next_enabled_node(rr_index, num_nodes);
            rr_index = selected_node + 1;
            break;
        }

        case NUMA_STRATEGY_CONFIG_WEIGHTED: {
            /* Only this strategy reads the weight array; briefly lock to copy it. */
            pthread_mutex_lock(&g_config_mutex);
            int total_weight = 0;
            for (int i = 0; i < num_nodes; i++) {
                if (!node_enabled(i, num_nodes)) continue;
                if (g_runtime_state.config.node_weights)
                    total_weight += g_runtime_state.config.node_weights[i];
            }

            if (total_weight > 0) {
                static __thread unsigned int seed = 0;
                if (seed == 0) seed = getpid() ^ pthread_self();
                int random_value = rand_r(&seed) % total_weight;
                int cumulative_weight = 0;
                for (int i = 0; i < num_nodes; i++) {
                    if (!node_enabled(i, num_nodes)) continue;
                    if (g_runtime_state.config.node_weights)
                        cumulative_weight += g_runtime_state.config.node_weights[i];
                    if (random_value < cumulative_weight) {
                        selected_node = i;
                        break;
                    }
                }
            }
            pthread_mutex_unlock(&g_config_mutex);
            break;
        }

        case NUMA_STRATEGY_CONFIG_PRESSURE_AWARE: {
            double min_utilization = 1.0;
            for (int i = 0; i < num_nodes; i++) {
                if (!node_enabled(i, num_nodes)) continue;
                double utilization = numa_config_get_node_utilization(i);
                if (utilization < min_utilization) {
                    min_utilization = utilization;
                    selected_node = i;
                }
            }
            break;
        }

        case NUMA_STRATEGY_CONFIG_CXL_OPTIMIZED: {
            size_t min_sz = g_runtime_state.config.min_allocation_size;
            if (size < min_sz) {
                selected_node = node_enabled(0, num_nodes) ? 0 : first_enabled_node(num_nodes);
            } else {
                selected_node = next_enabled_node(1, num_nodes);
            }
            break;
        }

        case NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE: {
            int total_weight = 0;
            int w[16];
            int n = (num_nodes <= 16) ? num_nodes : 16;
            for (int i = 0; i < n; i++) {
                if (!node_enabled(i, num_nodes)) {
                    w[i] = 0;
                    continue;
                }
                atomicGet(g_runtime_state.pressure_weights[i], w[i]);
                total_weight += w[i];
            }
            if (total_weight > 0) {
                static __thread unsigned int seed = 0;
                if (seed == 0) seed = (unsigned int)(getpid() ^ (uintptr_t)pthread_self());
                int r = rand_r(&seed) % total_weight;
                int cum = 0;
                for (int i = 0; i < n; i++) {
                    cum += w[i];
                    if (r < cum) { selected_node = i; break; }
                }
            }
            break;
        }

        case NUMA_STRATEGY_CONFIG_ADAPTIVE:
        case NUMA_STRATEGY_CONFIG_LATENCY_AWARE:
            selected_node = first_enabled_node(num_nodes);
            break;

        default:
            selected_node = first_enabled_node(num_nodes);
            break;
    }

    if (!node_enabled(selected_node, num_nodes)) {
        selected_node = first_enabled_node(num_nodes);
    }

    /* Lock-free atomic statistics updates using the Redis atomicvar.h API. */
    atomicIncr(g_runtime_state.allocation_counters[selected_node], 1);
    atomicIncr(g_runtime_state.bytes_allocated_per_node[selected_node], size);

    return selected_node;
}

/* ========== Public API implementation ========== */

/* Initialize the configurable strategy system. */
int numa_config_strategy_init(void) {
    if (g_initialized) {
        return C_OK;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    if (!g_initialized) {
        int num_nodes = numa_max_node() + 1;
        if (num_nodes <= 0) {
            num_nodes = 1; /* At least one node. */
        }
        
        if (init_runtime_state(num_nodes) == C_ERR) {
            pthread_mutex_unlock(&g_config_mutex);
            return C_ERR;
        }
        
        g_initialized = 1;
        serverLog(LL_NOTICE, "[NUMA Config] Configurable strategy system initialized (%d nodes)", num_nodes);
        serverLog(LL_NOTICE, "[NUMA Config] Default strategy: %s", 
                 get_strategy_name(g_runtime_state.config.strategy_type));
    }
    
    pthread_mutex_unlock(&g_config_mutex);
    return C_OK;
}

/* Clean up the strategy system. */
void numa_config_strategy_cleanup(void) {
    if (!g_initialized) return;
    
    pthread_mutex_lock(&g_config_mutex);
    
    if (g_runtime_state.config.node_weights) {
        zfree(g_runtime_state.config.node_weights);
    }
    if (g_runtime_state.allocation_counters) {
        zfree(g_runtime_state.allocation_counters);
    }
    if (g_runtime_state.bytes_allocated_per_node) {
        zfree(g_runtime_state.bytes_allocated_per_node);
    }
    if (g_runtime_state.pressure_weights) {
        zfree(g_runtime_state.pressure_weights);
    }
    if (g_runtime_state.bw_usage_percent) {
        zfree(g_runtime_state.bw_usage_percent);
    }
    if (g_runtime_state.distance_factors) {
        zfree(g_runtime_state.distance_factors);
    }

    memset(&g_runtime_state, 0, sizeof(g_runtime_state));
    g_initialized = 0;

    pthread_mutex_unlock(&g_config_mutex);
}

/* Load the strategy configuration from a file. */
int numa_config_load_from_file(const char *config_file) {
    if (!config_file || !g_initialized) {
        return C_ERR;
    }
    
    FILE *fp = fopen(config_file, "r");
    if (!fp) {
        serverLog(LL_WARNING, "[NUMA Config] Cannot open config file: %s", config_file);
        return C_ERR;
    }
    
    char line[256];
    numa_strategy_config_t new_config = g_runtime_state.config;
    
    while (fgets(line, sizeof(line), fp)) {
        char *key = strtok(line, "=");
        char *value = strtok(NULL, "\n");
        
        if (!key || !value) continue;
        
        /* Strip whitespace. */
        while (*key == ' ') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && *end == ' ') *end-- = '\0';
        
        if (strcmp(key, "strategy") == 0) {
            numa_config_strategy_type_t st = parse_strategy_name(value);
            if ((int)st >= 0) new_config.strategy_type = st;
            /* Keep the current value on an unknown strategy name. */
        } else if (strcmp(key, "balance_threshold") == 0) {
            new_config.balance_threshold = atof(value);
        } else if (strcmp(key, "auto_rebalance") == 0) {
            new_config.auto_rebalance = (strcasecmp(value, "yes") == 0 || atoi(value));
        } else if (strcmp(key, "rebalance_interval") == 0) {
            new_config.rebalance_interval_us = atoll(value);
        } else if (strcmp(key, "enable_cxl_optimization") == 0) {
            new_config.enable_cxl_optimization = (strcasecmp(value, "yes") == 0 || atoi(value));
        } else if (strncmp(key, "weight_", 7) == 0) {
            int node_id = atoi(key + 7);
            int weight = atoi(value);
            if (node_id >= 0 && node_id < new_config.num_nodes) {
                if (!new_config.node_weights) {
                    new_config.node_weights = zcalloc(new_config.num_nodes * sizeof(int));
                }
                new_config.node_weights[node_id] = weight;
            }
        }
    }
    
    fclose(fp);
    
    /* Apply the new configuration. */
    int result = numa_config_apply_strategy(&new_config);
    
    if (result == C_OK) {
        serverLog(LL_NOTICE, "[NUMA Config] Configuration loaded from %s", config_file);
    }
    
    return result;
}

/* Apply the strategy configuration. */
int numa_config_apply_strategy(const numa_strategy_config_t *config) {
    if (!config || !g_initialized) {
        return C_ERR;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    /* Validate the configuration. */
    if (config->num_nodes <= 0 || config->num_nodes > 64) { /* At most 64 nodes supported. */
        pthread_mutex_unlock(&g_config_mutex);
        return C_ERR;
    }
    
    /* Update the configuration. */
    g_runtime_state.config = *config;
    g_runtime_state.current_strategy = config->strategy_type;
    g_runtime_state.last_rebalance_time = 0;
    
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Strategy applied: %s", 
             get_strategy_name(config->strategy_type));
    
    return C_OK;
}

/* Get the current strategy configuration. */
const numa_strategy_config_t* numa_config_get_current(void) {
    if (!g_initialized) return NULL;
    return &g_runtime_state.config;
}

/* Set the NUMA allocation strategy. */
int numa_config_set_strategy(numa_config_strategy_type_t strategy) {
    if (!g_initialized) return C_ERR;
    
    pthread_mutex_lock(&g_config_mutex);
    g_runtime_state.config.strategy_type = strategy;
    g_runtime_state.current_strategy = strategy;
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Strategy changed to: %s", 
             get_strategy_name(strategy));
    
    return C_OK;
}

/* Set the node weights. */
int numa_config_set_node_weights(int *weights, int num_nodes) {
    if (!weights || num_nodes <= 0 || !g_initialized) {
        return C_ERR;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    if (num_nodes != g_runtime_state.config.num_nodes) {
        /* Reallocate the weight array. */
        if (g_runtime_state.config.node_weights) {
            zfree(g_runtime_state.config.node_weights);
        }
        g_runtime_state.config.node_weights = zcalloc(num_nodes * sizeof(int));
        g_runtime_state.config.num_nodes = num_nodes;
    }
    
    /* Copy the weights. */
    for (int i = 0; i < num_nodes && i < g_runtime_state.config.num_nodes; i++) {
        g_runtime_state.config.node_weights[i] = weights[i];
    }
    
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Node weights updated");
    return C_OK;
}

int numa_config_set_enabled_nodes_mask(uint64_t mask) {
    if (!g_initialized) return C_ERR;

    pthread_mutex_lock(&g_config_mutex);
    int num_nodes = g_runtime_state.config.num_nodes;
    uint64_t valid_mask = 0;
    for (int i = 0; i < num_nodes && i < 64; i++) {
        valid_mask |= (1ULL << i);
    }
    mask &= valid_mask;
    if (mask == 0) {
        mask = 0;
    }
    g_runtime_state.config.enabled_nodes_mask = mask;
    pthread_mutex_unlock(&g_config_mutex);

    serverLog(LL_NOTICE, "[NUMA Config] Enabled nodes mask set to 0x%llx", (unsigned long long)mask);
    return C_OK;
}

uint64_t numa_config_get_enabled_nodes_mask(void) {
    if (!g_initialized) return 0;
    return g_runtime_state.config.enabled_nodes_mask;
}

/* Enable/disable CXL optimization. */
int numa_config_set_cxl_optimization(int enable) {
    if (!g_initialized) return C_ERR;
    
    pthread_mutex_lock(&g_config_mutex);
    g_runtime_state.config.enable_cxl_optimization = enable;
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] CXL optimization %s", 
             enable ? "enabled" : "disabled");
    
    return C_OK;
}

/* Set the balance threshold. */
int numa_config_set_balance_threshold(double threshold) {
    if (threshold < 0.0 || threshold > 1.0 || !g_initialized) {
        return C_ERR;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    g_runtime_state.config.balance_threshold = threshold;
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Balance threshold set to %.2f", threshold);
    return C_OK;
}

/* Manually trigger a rebalance. */
int numa_config_trigger_rebalance(void) {
    if (!g_initialized) return C_ERR;
    
    pthread_mutex_lock(&g_config_mutex);
    g_runtime_state.last_rebalance_time = 0; /* Force a rebalance on the next check. */
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Manual rebalance triggered");
    return C_OK;
}

/* Smart memory allocation. */
void *numa_config_malloc(size_t size) {
    if (!g_initialized) {
        return zmalloc(size);
    }
    
    int target_node = select_best_node(size);
    return numa_zmalloc_onnode(size, target_node);
}

/* Smart zeroed allocation. */
void *numa_config_calloc(size_t nmemb, size_t size) {
    if (!g_initialized) {
        return zcalloc(nmemb * size);
    }
    
    size_t total_size = nmemb * size;
    int target_node = select_best_node(total_size);
    void *ptr = numa_zmalloc_onnode(total_size, target_node);
    
    if (ptr) {
        memset(ptr, 0, total_size);
    }
    
    return ptr;
}

/* Allocate on a given node. */
void *numa_config_malloc_onnode(size_t size, int node) {
    if (!g_initialized) {
        return numa_zmalloc_onnode(size, node);
    }
    
    pthread_mutex_lock(&g_config_mutex);
    if (node >= 0 && node < g_runtime_state.config.num_nodes) {
        atomicIncr(g_runtime_state.allocation_counters[node], 1);
        atomicIncr(g_runtime_state.bytes_allocated_per_node[node], size);
    }
    pthread_mutex_unlock(&g_config_mutex);
    
    return numa_zmalloc_onnode(size, node);
}

/* Get the strategy execution statistics. */
void numa_config_get_statistics(uint64_t *allocations_per_node, 
                               size_t *bytes_per_node,
                               int num_nodes) {
    if (!g_initialized || !allocations_per_node || !bytes_per_node) {
        return;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    int copy_nodes = (num_nodes < g_runtime_state.config.num_nodes) ? 
                     num_nodes : g_runtime_state.config.num_nodes;
    
    for (int i = 0; i < copy_nodes; i++) {
        atomicGet(g_runtime_state.allocation_counters[i], allocations_per_node[i]);
        atomicGet(g_runtime_state.bytes_allocated_per_node[i], bytes_per_node[i]);
    }
    
    pthread_mutex_unlock(&g_config_mutex);
}

/* Get the node load info. */
double numa_config_get_node_utilization(int node_id) {
    if (!g_initialized || node_id < 0 || node_id >= g_runtime_state.config.num_nodes) {
        return 0.0;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    double utilization = 0.0;
    size_t node_bytes;
    atomicGet(g_runtime_state.bytes_allocated_per_node[node_id], node_bytes);
    if (node_bytes > 0) {
        /* Simplified utilization computation. */
        utilization = (double)node_bytes /
                      (1024.0 * 1024.0 * 1024.0); /* Convert to GB as an example. */
    }
    
    pthread_mutex_unlock(&g_config_mutex);
    return utilization;
}

/* Check whether a rebalance is needed. */
int numa_config_needs_rebalance(void) {
    if (!g_initialized || !g_runtime_state.config.auto_rebalance) {
        return 0;
    }
    
    uint64_t current_time = ustime();
    if (current_time - g_runtime_state.last_rebalance_time < 
        g_runtime_state.config.rebalance_interval_us) {
        return 0;
    }
    
    /* Check for load imbalance. */
    pthread_mutex_lock(&g_config_mutex);
    
    double max_util = 0.0, min_util = 1e30;
    for (int i = 0; i < g_runtime_state.config.num_nodes; i++) {
        double util = numa_config_get_node_utilization(i);
        if (util > max_util) max_util = util;
        if (util < min_util) min_util = util;
    }
    
    int needs_rebalance = (max_util - min_util) > g_runtime_state.config.balance_threshold;
    
    pthread_mutex_unlock(&g_config_mutex);
    
    return needs_rebalance;
}

/* Get the best allocation node. */
int numa_config_get_best_node(size_t size) {
    if (!g_initialized) return 0;
    return select_best_node(size);
}

/* Handle NUMA configuration commands. */
int numa_config_handle_command(int argc, char **argv) {
    if (argc < 2) {
        numa_config_show_help();
        return C_OK;
    }
    
    if (strcasecmp(argv[1], "GET") == 0) {
        numa_config_show_status();
        return C_OK;
    }
    
    if (strcasecmp(argv[1], "SET") == 0 && argc >= 4) {
        if (strcasecmp(argv[2], "strategy") == 0) {
            numa_config_strategy_type_t strategy = parse_strategy_name(argv[3]);
            if ((int)strategy < 0) return C_ERR;
            return numa_config_set_strategy(strategy);
        } else if (strcasecmp(argv[2], "cxl_optimization") == 0) {
            int enable = (strcasecmp(argv[3], "on") == 0 || atoi(argv[3]));
            return numa_config_set_cxl_optimization(enable);
        } else if (strcasecmp(argv[2], "balance_threshold") == 0) {
            double threshold = atof(argv[3]);
            return numa_config_set_balance_threshold(threshold);
        }
        return C_ERR;
    }
    
    if (strcasecmp(argv[1], "REBALANCE") == 0) {
        return numa_config_trigger_rebalance();
    }
    
    if (strcasecmp(argv[1], "STATS") == 0) {
        /* Show the statistics. */
        serverLog(LL_NOTICE, "[NUMA Config] Allocation Statistics:");
        for (int i = 0; i < g_runtime_state.config.num_nodes; i++) {
            int alloc_count;
            size_t alloc_bytes;
            atomicGet(g_runtime_state.allocation_counters[i], alloc_count);
            atomicGet(g_runtime_state.bytes_allocated_per_node[i], alloc_bytes);
            serverLog(LL_NOTICE, "  Node %d: %d allocations, %zu bytes",
                     i, alloc_count, alloc_bytes);
        }
        return C_OK;
    }
    
    return C_ERR;
}

/* Show the current configuration state. */
void numa_config_show_status(void) {
    if (!g_initialized) {
        serverLog(LL_NOTICE, "[NUMA Config] System not initialized");
        return;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Current Status:");
    serverLog(LL_NOTICE, "  Strategy: %s", 
             get_strategy_name(g_runtime_state.config.strategy_type));
    serverLog(LL_NOTICE, "  Nodes: %d", g_runtime_state.config.num_nodes);
    serverLog(LL_NOTICE, "  Balance Threshold: %.2f", 
             g_runtime_state.config.balance_threshold);
    serverLog(LL_NOTICE, "  Auto Rebalance: %s", 
             g_runtime_state.config.auto_rebalance ? "Yes" : "No");
    serverLog(LL_NOTICE, "  CXL Optimization: %s", 
             g_runtime_state.config.enable_cxl_optimization ? "Enabled" : "Disabled");
    
    if (g_runtime_state.config.node_weights) {
        serverLog(LL_NOTICE, "  Node Weights:");
        for (int i = 0; i < g_runtime_state.config.num_nodes; i++) {
            serverLog(LL_NOTICE, "    Node %d: %d", i, g_runtime_state.config.node_weights[i]);
        }
    }
    
    pthread_mutex_unlock(&g_config_mutex);
}

/* Show help. */
void numa_config_show_help(void) {
    serverLog(LL_NOTICE, "[NUMA Config] Available Commands:");
    serverLog(LL_NOTICE, "  NUMACONFIG GET                    - Show current configuration");
    serverLog(LL_NOTICE, "  NUMACONFIG SET strategy <name>    - Set allocation strategy");
    serverLog(LL_NOTICE, "  NUMACONFIG SET cxl_optimization <on/off> - Enable/disable CXL optimization");
    serverLog(LL_NOTICE, "  NUMACONFIG SET balance_threshold <value> - Set balance threshold");
    serverLog(LL_NOTICE, "  NUMACONFIG REBALANCE              - Trigger manual rebalance");
    serverLog(LL_NOTICE, "  NUMACONFIG STATS                  - Show allocation statistics");
    serverLog(LL_NOTICE, "");
    serverLog(LL_NOTICE, "Available Strategies:");
    for (size_t i = 0; i < sizeof(strategy_names)/sizeof(strategy_names[0]); i++) {
        serverLog(LL_NOTICE, "  %s", strategy_names[i]);
    }
}

/* ========== Pressure weight updates ========== */

void numa_config_update_pressure_weights(void) {
    if (!g_initialized || !g_runtime_state.pressure_weights) return;
    int n = g_runtime_state.config.num_nodes;
    for (int i = 0; i < n; i++) {
        double p = numaGetNodePressure(i);
        double bw = numa_bw_get_usage(i);
        if (bw < 0) bw = 0;

        double df = g_runtime_state.distance_factors
                  ? g_runtime_state.distance_factors[i] : 1.0;
        int w = (int)((1.0 - p) * (1.0 - bw) * df * 100);
        if (w < 1) w = 1;
        atomicSet(g_runtime_state.pressure_weights[i], w);

        if (g_runtime_state.bw_usage_percent) {
            int bw_pct = (int)(bw * 100);
            if (bw_pct > 100) bw_pct = 100;
            atomicSet(g_runtime_state.bw_usage_percent[i], bw_pct);
        }
    }
}

int numa_config_get_cached_bw(int node) {
    if (!g_initialized || !g_runtime_state.bw_usage_percent ||
        node < 0 || node >= g_runtime_state.config.num_nodes)
        return 0;
    int val;
    atomicGet(g_runtime_state.bw_usage_percent[node], val);
    return val;
}

int numa_config_get_cached_pressure_weight(int node) {
    if (!g_initialized || !g_runtime_state.pressure_weights ||
        node < 0 || node >= g_runtime_state.config.num_nodes)
        return 100;
    int val;
    atomicGet(g_runtime_state.pressure_weights[node], val);
    return val;
}
