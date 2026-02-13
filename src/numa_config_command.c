/* numa_config_command.c - NUMACONFIG Redis命令实现 */

#include "server.h"
#include "numa_configurable_strategy.h"

/* 函数声明 */
extern const char* get_strategy_name(numa_config_strategy_type_t strategy);
extern numa_config_strategy_type_t parse_strategy_name(const char* name);

/* 外部Redis函数声明 */
extern void addReply(client *c, robj *obj);
extern void addReplyError(client *c, const char *err);
extern void addReplyErrorFormat(client *c, const char *fmt, ...);
extern void addReplyLongLong(client *c, long long ll);
extern void addReplyBulkCString(client *c, const char *s);
extern void addReplyArrayLen(client *c, long length);
extern void addReplyStatus(client *c, const char *status);
extern void addReplyArrayLen(client *c, long length);

/*
 * NUMACONFIG command implementation
 * 
 * Usage:
 *   NUMACONFIG GET                           - Show current configuration
 *   NUMACONFIG SET strategy <strategy_name>  - Set allocation strategy
 *   NUMACONFIG SET weight <node> <weight>    - Set node weight
 *   NUMACONFIG SET cxl_optimization <on/off> - Enable/disable CXL optimization
 *   NUMACONFIG SET balance_threshold <value> - Set balance threshold
 *   NUMACONFIG REBALANCE                     - Trigger manual rebalance
 *   NUMACONFIG STATS                         - Show allocation statistics
 *   NUMACONFIG HELP                          - Show help information
 */
void numaconfigCommand(client *c) {
    /* 初始化可配置策略系统（如果尚未初始化） */
    if (numa_config_strategy_init() != C_OK) {
        addReplyError(c, "Failed to initialize NUMA configurable strategy system");
        return;
    }
    
    if (c->argc < 2) {
        addReplyError(c, "wrong number of arguments for 'NUMACONFIG' command");
        return;
    }
    
    const char *subcmd = c->argv[1]->ptr;
    
    /* NUMACONFIG GET - 显示当前配置 */
    if (!strcasecmp(subcmd, "GET")) {
        const numa_strategy_config_t *config = numa_config_get_current();
        if (!config) {
            addReplyError(c, "NUMA configuration not available");
            return;
        }
        
        addReplyArrayLen(c, 10);
        
        addReplyBulkCString(c, "strategy");
        addReplyBulkCString(c, get_strategy_name(config->strategy_type));
        
        addReplyBulkCString(c, "nodes");
        addReplyLongLong(c, config->num_nodes);
        
        addReplyBulkCString(c, "balance_threshold");
        addReplyLongLong(c, (long long)(config->balance_threshold * 100)); /* 以百分比显示 */
        
        addReplyBulkCString(c, "auto_rebalance");
        addReplyBulkCString(c, config->auto_rebalance ? "yes" : "no");
        
        addReplyBulkCString(c, "cxl_optimization");
        addReplyBulkCString(c, config->enable_cxl_optimization ? "enabled" : "disabled");
        
        addReplyBulkCString(c, "rebalance_interval");
        addReplyLongLong(c, config->rebalance_interval_us);
        
        addReplyBulkCString(c, "min_allocation_size");
        addReplyLongLong(c, config->min_allocation_size);
        
        /* 显示节点权重 */
        addReplyBulkCString(c, "node_weights");
        addReplyArrayLen(c, config->num_nodes);
        for (int i = 0; i < config->num_nodes; i++) {
            addReplyArrayLen(c, 2);
            addReplyLongLong(c, i);
            addReplyLongLong(c, config->node_weights ? config->node_weights[i] : 100);
        }
        
        return;
    }
    
    /* NUMACONFIG SET - 设置配置参数 */
    if (!strcasecmp(subcmd, "SET")) {
        if (c->argc < 4) {
            addReplyError(c, "Usage: NUMACONFIG SET <parameter> <value>");
            return;
        }
        
        const char *param = c->argv[2]->ptr;
        const char *value = c->argv[3]->ptr;
        
        /* 设置分配策略 */
        if (!strcasecmp(param, "strategy")) {
            numa_config_strategy_type_t strategy = parse_strategy_name(value);
            if (numa_config_set_strategy(strategy) == C_OK) {
                addReplyStatus(c, "OK");
            } else {
                addReplyError(c, "Failed to set strategy");
            }
            return;
        }
        
        /* 设置CXL优化 */
        if (!strcasecmp(param, "cxl_optimization")) {
            int enable = (!strcasecmp(value, "on") || !strcasecmp(value, "yes") || atoi(value));
            if (numa_config_set_cxl_optimization(enable) == C_OK) {
                addReplyStatus(c, "OK");
            } else {
                addReplyError(c, "Failed to set CXL optimization");
            }
            return;
        }
        
        /* 设置平衡阈值 */
        if (!strcasecmp(param, "balance_threshold")) {
            double threshold = atof(value) / 100.0; /* 输入为百分比 */
            if (threshold >= 0.0 && threshold <= 1.0) {
                if (numa_config_set_balance_threshold(threshold) == C_OK) {
                    addReplyStatus(c, "OK");
                } else {
                    addReplyError(c, "Failed to set balance threshold");
                }
            } else {
                addReplyError(c, "Balance threshold must be between 0 and 100");
            }
            return;
        }
        
        /* 设置节点权重 */
        if (!strcasecmp(param, "weight") && c->argc >= 5) {
            long node_id, weight;
            if (getLongFromObjectOrReply(c, c->argv[3], &node_id, "Invalid node ID") != C_OK ||
                getLongFromObjectOrReply(c, c->argv[4], &weight, "Invalid weight") != C_OK) {
                return;
            }
            
            const numa_strategy_config_t *current_config = numa_config_get_current();
            if (!current_config || node_id < 0 || node_id >= current_config->num_nodes) {
                addReplyErrorFormat(c, "Node ID %ld out of range", node_id);
                return;
            }
            
            if (weight < 0 || weight > 1000) {
                addReplyError(c, "Weight must be between 0 and 1000");
                return;
            }
            
            /* 创建新的权重数组 */
            int *new_weights = zmalloc(current_config->num_nodes * sizeof(int));
            if (!new_weights) {
                addReplyError(c, "Memory allocation failed");
                return;
            }
            
            /* 复制现有权重 */
            if (current_config->node_weights) {
                memcpy(new_weights, current_config->node_weights, 
                       current_config->num_nodes * sizeof(int));
            } else {
                /* 初始化默认权重 */
                for (int i = 0; i < current_config->num_nodes; i++) {
                    new_weights[i] = 100;
                }
            }
            
            /* 更新指定节点权重 */
            new_weights[node_id] = weight;
            
            int result = numa_config_set_node_weights(new_weights, current_config->num_nodes);
            zfree(new_weights);
            
            if (result == C_OK) {
                addReplyStatus(c, "OK");
            } else {
                addReplyError(c, "Failed to set node weight");
            }
            return;
        }
        
        addReplyErrorFormat(c, "Unknown parameter: %s", param);
        return;
    }
    
    /* NUMACONFIG REBALANCE - 手动触发重新平衡 */
    if (!strcasecmp(subcmd, "REBALANCE")) {
        if (numa_config_trigger_rebalance() == C_OK) {
            addReplyStatus(c, "OK");
        } else {
            addReplyError(c, "Failed to trigger rebalance");
        }
        return;
    }
    
    /* NUMACONFIG STATS - 显示统计信息 */
    if (!strcasecmp(subcmd, "STATS")) {
        const numa_strategy_config_t *config = numa_config_get_current();
        if (!config) {
            addReplyError(c, "NUMA configuration not available");
            return;
        }
        
        /* 准备统计数组 */
        uint64_t *allocations = zcalloc(config->num_nodes * sizeof(uint64_t));
        size_t *bytes = zcalloc(config->num_nodes * sizeof(size_t));
        
        if (!allocations || !bytes) {
            if (allocations) zfree(allocations);
            if (bytes) zfree(bytes);
            addReplyError(c, "Memory allocation failed");
            return;
        }
        
        /* 获取统计信息 */
        numa_config_get_statistics(allocations, bytes, config->num_nodes);
        
        /* 返回统计结果 */
        addReplyArrayLen(c, config->num_nodes * 2);
        for (int i = 0; i < config->num_nodes; i++) {
            addReplyArrayLen(c, 2);
            addReplyBulkCString(c, "node");
            addReplyLongLong(c, i);
            
            addReplyArrayLen(c, 4);
            addReplyBulkCString(c, "allocations");
            addReplyLongLong(c, allocations[i]);
            addReplyBulkCString(c, "bytes");
            addReplyLongLong(c, bytes[i]);
        }
        
        zfree(allocations);
        zfree(bytes);
        return;
    }
    
    /* NUMACONFIG HELP - 显示帮助信息 */
    if (!strcasecmp(subcmd, "HELP")) {
        addReplyArrayLen(c, 8);
        addReplyBulkCString(c, "NUMACONFIG GET");
        addReplyBulkCString(c, "NUMACONFIG SET strategy <name>");
        addReplyBulkCString(c, "NUMACONFIG SET weight <node> <weight>");
        addReplyBulkCString(c, "NUMACONFIG SET cxl_optimization <on/off>");
        addReplyBulkCString(c, "NUMACONFIG SET balance_threshold <percent>");
        addReplyBulkCString(c, "NUMACONFIG REBALANCE");
        addReplyBulkCString(c, "NUMACONFIG STATS");
        addReplyBulkCString(c, "NUMACONFIG HELP");
        return;
    }
    
    /* 未知子命令 */
    addReplyErrorFormat(c, "Unknown subcommand: %s", subcmd);
}