/* numa_configurable_strategy.c - 可配置NUMA分配策略实现 */

#define _GNU_SOURCE
#include "numa_configurable_strategy.h"
#include "zmalloc.h"
#include "server.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <numa.h>

/* 全局运行时状态 */
static numa_runtime_state_t g_runtime_state = {0};
static pthread_mutex_t g_config_mutex = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

/* 策略名称映射 */
static const char* strategy_names[] = {
    "local_first",
    "interleaved", 
    "round_robin",
    "weighted",
    "pressure_aware",
    "cxl_optimized"
};

/* 获取策略名称 */
const char* get_strategy_name(numa_config_strategy_type_t strategy) {
    if (strategy >= 0 && strategy < (int)(sizeof(strategy_names)/sizeof(strategy_names[0]))) {
        return strategy_names[strategy];
    }
    return "unknown";
}

/* 解析策略名称 */
numa_config_strategy_type_t parse_strategy_name(const char* name) {
    for (size_t i = 0; i < sizeof(strategy_names)/sizeof(strategy_names[0]); i++) {
        if (strcasecmp(name, strategy_names[i]) == 0) {
            return (numa_config_strategy_type_t)i;
        }
    }
    return NUMA_STRATEGY_CONFIG_LOCAL_FIRST; /* 默认策略 */
}

/* 初始化运行时状态 */
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
    
    memset(&g_runtime_state, 0, sizeof(g_runtime_state));
    
    g_runtime_state.config.num_nodes = num_nodes;
    g_runtime_state.config.strategy_type = NUMA_STRATEGY_CONFIG_INTERLEAVE; /* 默认使用交错策略 */
    g_runtime_state.config.balance_threshold = 0.3;
    g_runtime_state.config.auto_rebalance = 1;
    g_runtime_state.config.rebalance_interval_us = 5000000; /* 5秒 */
    
    /* 分配权重数组 */
    g_runtime_state.config.node_weights = zcalloc(num_nodes * sizeof(int));
    g_runtime_state.allocation_counters = zcalloc(num_nodes * sizeof(int));
    g_runtime_state.bytes_allocated_per_node = zcalloc(num_nodes * sizeof(size_t));
    
    if (!g_runtime_state.config.node_weights || 
        !g_runtime_state.allocation_counters || 
        !g_runtime_state.bytes_allocated_per_node) {
        return C_ERR;
    }
    
    /* 初始化默认权重 */
    for (int i = 0; i < num_nodes; i++) {
        g_runtime_state.config.node_weights[i] = 100; /* 默认权重100 */
    }
    
    return C_OK;
}

/* 选择最佳分配节点 */
static int select_best_node(size_t size) {
    int num_nodes = g_runtime_state.config.num_nodes;
    int selected_node = 0;
    
    pthread_mutex_lock(&g_config_mutex);
    
    switch (g_runtime_state.config.strategy_type) {
        case NUMA_STRATEGY_CONFIG_LOCAL_FIRST:
            selected_node = 0; /* 总是选择节点0 */
            break;
            
        case NUMA_STRATEGY_CONFIG_INTERLEAVE: {
            static __thread unsigned int seed = 0;
            if (seed == 0) seed = getpid() ^ pthread_self();
            selected_node = rand_r(&seed) % num_nodes;
            break;
        }
            
        case NUMA_STRATEGY_CONFIG_ROUND_ROBIN: {
            static __thread int rr_index = 0;
            selected_node = rr_index % num_nodes;
            rr_index++;
            break;
        }
            
        case NUMA_STRATEGY_CONFIG_WEIGHTED: {
            /* 加权随机选择 */
            int total_weight = 0;
            for (int i = 0; i < num_nodes; i++) {
                total_weight += g_runtime_state.config.node_weights[i];
            }
            
            if (total_weight > 0) {
                static __thread unsigned int seed = 0;
                if (seed == 0) seed = getpid() ^ pthread_self();
                int random_value = rand_r(&seed) % total_weight;
                
                int cumulative_weight = 0;
                for (int i = 0; i < num_nodes; i++) {
                    cumulative_weight += g_runtime_state.config.node_weights[i];
                    if (random_value < cumulative_weight) {
                        selected_node = i;
                        break;
                    }
                }
            }
            break;
        }
            
        case NUMA_STRATEGY_CONFIG_PRESSURE_AWARE: {
            /* 选择负载最轻的节点 */
            double min_utilization = 1.0;
            for (int i = 0; i < num_nodes; i++) {
                double utilization = numa_config_get_node_utilization(i);
                if (utilization < min_utilization) {
                    min_utilization = utilization;
                    selected_node = i;
                }
            }
            break;
        }
            
        case NUMA_STRATEGY_CONFIG_CXL_OPTIMIZED:
            /* CXL优化：小对象本地分配，大对象远端分配 */
            if (size < g_runtime_state.config.min_allocation_size) {
                selected_node = 0; /* 小对象分配到本地 */
            } else {
                /* 大对象分配到CXL节点 */
                selected_node = (num_nodes > 1) ? 1 : 0;
            }
            break;
            
        default:
            selected_node = 0;
            break;
    }
    
    /* 更新统计 */
    g_runtime_state.allocation_counters[selected_node]++;
    g_runtime_state.bytes_allocated_per_node[selected_node] += size;
    
    pthread_mutex_unlock(&g_config_mutex);
    
    return selected_node;
}

/* ========== 公共API实现 ========== */

/* 初始化可配置策略系统 */
int numa_config_strategy_init(void) {
    if (g_initialized) {
        return C_OK;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    if (!g_initialized) {
        int num_nodes = numa_max_node() + 1;
        if (num_nodes <= 0) {
            num_nodes = 1; /* 至少有一个节点 */
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

/* 清理策略系统 */
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
    
    memset(&g_runtime_state, 0, sizeof(g_runtime_state));
    g_initialized = 0;
    
    pthread_mutex_unlock(&g_config_mutex);
}

/* 从配置文件加载策略配置 */
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
        
        /* 去除空白字符 */
        while (*key == ' ') key++;
        char *end = key + strlen(key) - 1;
        while (end > key && *end == ' ') *end-- = '\0';
        
        if (strcmp(key, "strategy") == 0) {
            new_config.strategy_type = parse_strategy_name(value);
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
    
    /* 应用新配置 */
    int result = numa_config_apply_strategy(&new_config);
    
    if (result == C_OK) {
        serverLog(LL_NOTICE, "[NUMA Config] Configuration loaded from %s", config_file);
    }
    
    return result;
}

/* 应用策略配置 */
int numa_config_apply_strategy(const numa_strategy_config_t *config) {
    if (!config || !g_initialized) {
        return C_ERR;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    /* 验证配置 */
    if (config->num_nodes <= 0 || config->num_nodes > 64) { /* 最多支持64个节点 */
        pthread_mutex_unlock(&g_config_mutex);
        return C_ERR;
    }
    
    /* 更新配置 */
    g_runtime_state.config = *config;
    g_runtime_state.current_strategy = config->strategy_type;
    g_runtime_state.last_rebalance_time = 0;
    
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Strategy applied: %s", 
             get_strategy_name(config->strategy_type));
    
    return C_OK;
}

/* 获取当前策略配置 */
const numa_strategy_config_t* numa_config_get_current(void) {
    if (!g_initialized) return NULL;
    return &g_runtime_state.config;
}

/* 设置NUMA分配策略 */
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

/* 设置节点权重 */
int numa_config_set_node_weights(int *weights, int num_nodes) {
    if (!weights || num_nodes <= 0 || !g_initialized) {
        return C_ERR;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    if (num_nodes != g_runtime_state.config.num_nodes) {
        /* 重新分配权重数组 */
        if (g_runtime_state.config.node_weights) {
            zfree(g_runtime_state.config.node_weights);
        }
        g_runtime_state.config.node_weights = zcalloc(num_nodes * sizeof(int));
        g_runtime_state.config.num_nodes = num_nodes;
    }
    
    /* 复制权重 */
    for (int i = 0; i < num_nodes && i < g_runtime_state.config.num_nodes; i++) {
        g_runtime_state.config.node_weights[i] = weights[i];
    }
    
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Node weights updated");
    return C_OK;
}

/* 启用/禁用CXL优化 */
int numa_config_set_cxl_optimization(int enable) {
    if (!g_initialized) return C_ERR;
    
    pthread_mutex_lock(&g_config_mutex);
    g_runtime_state.config.enable_cxl_optimization = enable;
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] CXL optimization %s", 
             enable ? "enabled" : "disabled");
    
    return C_OK;
}

/* 设置平衡阈值 */
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

/* 手动触发重新平衡 */
int numa_config_trigger_rebalance(void) {
    if (!g_initialized) return C_ERR;
    
    pthread_mutex_lock(&g_config_mutex);
    g_runtime_state.last_rebalance_time = 0; /* 强制下次检查时重新平衡 */
    pthread_mutex_unlock(&g_config_mutex);
    
    serverLog(LL_NOTICE, "[NUMA Config] Manual rebalance triggered");
    return C_OK;
}

/* 智能内存分配 */
void *numa_config_malloc(size_t size) {
    if (!g_initialized) {
        return zmalloc(size);
    }
    
    int target_node = select_best_node(size);
    return numa_zmalloc_onnode(size, target_node);
}

/* 智能清零分配 */
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

/* 在指定节点分配 */
void *numa_config_malloc_onnode(size_t size, int node) {
    if (!g_initialized) {
        return numa_zmalloc_onnode(size, node);
    }
    
    pthread_mutex_lock(&g_config_mutex);
    if (node >= 0 && node < g_runtime_state.config.num_nodes) {
        g_runtime_state.allocation_counters[node]++;
        g_runtime_state.bytes_allocated_per_node[node] += size;
    }
    pthread_mutex_unlock(&g_config_mutex);
    
    return numa_zmalloc_onnode(size, node);
}

/* 获取策略执行统计 */
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
        allocations_per_node[i] = g_runtime_state.allocation_counters[i];
        bytes_per_node[i] = g_runtime_state.bytes_allocated_per_node[i];
    }
    
    pthread_mutex_unlock(&g_config_mutex);
}

/* 获取节点负载信息 */
double numa_config_get_node_utilization(int node_id) {
    if (!g_initialized || node_id < 0 || node_id >= g_runtime_state.config.num_nodes) {
        return 0.0;
    }
    
    pthread_mutex_lock(&g_config_mutex);
    
    double utilization = 0.0;
    if (g_runtime_state.bytes_allocated_per_node[node_id] > 0) {
        /* 简化的利用率计算 */
        utilization = (double)g_runtime_state.bytes_allocated_per_node[node_id] / 
                      (1024.0 * 1024.0 * 1024.0); /* 转换为GB作为示例 */
    }
    
    pthread_mutex_unlock(&g_config_mutex);
    return utilization;
}

/* 检查是否需要重新平衡 */
int numa_config_needs_rebalance(void) {
    if (!g_initialized || !g_runtime_state.config.auto_rebalance) {
        return 0;
    }
    
    uint64_t current_time = ustime();
    if (current_time - g_runtime_state.last_rebalance_time < 
        g_runtime_state.config.rebalance_interval_us) {
        return 0;
    }
    
    /* 检查负载不均衡 */
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

/* 获取最佳分配节点 */
int numa_config_get_best_node(size_t size) {
    if (!g_initialized) return 0;
    return select_best_node(size);
}

/* 处理NUMA配置相关命令 */
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
        /* 显示统计信息 */
        serverLog(LL_NOTICE, "[NUMA Config] Allocation Statistics:");
        for (int i = 0; i < g_runtime_state.config.num_nodes; i++) {
            serverLog(LL_NOTICE, "  Node %d: %llu allocations, %zu bytes", 
                     i, 
                     (unsigned long long)g_runtime_state.allocation_counters[i],
                     g_runtime_state.bytes_allocated_per_node[i]);
        }
        return C_OK;
    }
    
    return C_ERR;
}

/* 显示当前配置状态 */
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

/* 显示帮助信息 */
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