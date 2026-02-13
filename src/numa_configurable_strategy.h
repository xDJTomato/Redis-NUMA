/* numa_configurable_strategy.h - 可配置NUMA分配策略接口
 *
 * 提供运行时可配置的NUMA内存分配策略，支持通过配置文件和命令行指令动态调整
 */

#ifndef NUMA_CONFIGURABLE_STRATEGY_H
#define NUMA_CONFIGURABLE_STRATEGY_H

#include <stddef.h>
#include <stdint.h>
#include "zmalloc.h"
#include <stddef.h>

/* 可配置的NUMA策略类型 */
typedef enum {
    NUMA_STRATEGY_CONFIG_LOCAL_FIRST = 0,    /* 本地优先策略 */
    NUMA_STRATEGY_CONFIG_INTERLEAVE,         /* 交错分配策略 */
    NUMA_STRATEGY_CONFIG_ROUND_ROBIN,        /* 轮询分配策略 */
    NUMA_STRATEGY_CONFIG_WEIGHTED,           /* 加权分配策略 */
    NUMA_STRATEGY_CONFIG_PRESSURE_AWARE,     /* 压力感知策略 */
    NUMA_STRATEGY_CONFIG_CXL_OPTIMIZED       /* CXL优化策略 */
} numa_config_strategy_type_t;

/* 策略配置参数 */
typedef struct {
    numa_config_strategy_type_t strategy_type;  /* 策略类型 */
    int *node_weights;                          /* 各节点权重数组 */
    int num_nodes;                              /* 节点数量 */
    double balance_threshold;                   /* 平衡阈值 */
    int enable_cxl_optimization;                /* 是否启用CXL优化 */
    size_t min_allocation_size;                 /* 最小分配大小 */
    int auto_rebalance;                         /* 是否自动重新平衡 */
    uint64_t rebalance_interval_us;             /* 重新平衡间隔 */
} numa_strategy_config_t;

/* 运行时策略状态 */
typedef struct {
    numa_strategy_config_t config;
    int current_strategy;                       /* 当前使用的策略 */
    uint64_t last_rebalance_time;               /* 上次重新平衡时间 */
    int *allocation_counters;                   /* 各节点分配计数器 */
    size_t *bytes_allocated_per_node;           /* 各节点已分配字节数 */
} numa_runtime_state_t;

/* ========== 配置管理API ========== */

/* 初始化可配置策略系统 */
int numa_config_strategy_init(void);

/* 清理策略系统 */
void numa_config_strategy_cleanup(void);

/* 从配置文件加载策略配置 */
int numa_config_load_from_file(const char *config_file);

/* 应用策略配置 */
int numa_config_apply_strategy(const numa_strategy_config_t *config);

/* 获取当前策略配置 */
const numa_strategy_config_t* numa_config_get_current(void);

/* ========== 运行时控制API ========== */

/* 设置NUMA分配策略 */
int numa_config_set_strategy(numa_config_strategy_type_t strategy);

/* 设置节点权重 */
int numa_config_set_node_weights(int *weights, int num_nodes);

/* 启用/禁用CXL优化 */
int numa_config_set_cxl_optimization(int enable);

/* 设置平衡阈值 */
int numa_config_set_balance_threshold(double threshold);

/* 手动触发重新平衡 */
int numa_config_trigger_rebalance(void);

/* ========== 内存分配API ========== */

/* 智能内存分配 - 根据当前配置选择最优策略 */
void *numa_config_malloc(size_t size);

/* 智能清零分配 */
void *numa_config_calloc(size_t nmemb, size_t size);

/* 在指定节点分配 */
void *numa_config_malloc_onnode(size_t size, int node);

/* ========== 查询和统计API ========== */

/* 获取策略执行统计 */
void numa_config_get_statistics(uint64_t *allocations_per_node, 
                               size_t *bytes_per_node,
                               int num_nodes);

/* 获取节点负载信息 */
double numa_config_get_node_utilization(int node_id);

/* 检查是否需要重新平衡 */
int numa_config_needs_rebalance(void);

/* 获取最佳分配节点 */
int numa_config_get_best_node(size_t size);

/* ========== 命令行接口 ========== */

/* 处理NUMA配置相关命令 */
int numa_config_handle_command(int argc, char **argv);

/* 显示当前配置状态 */
void numa_config_show_status(void);

/* 显示帮助信息 */
void numa_config_show_help(void);

#endif /* NUMA_CONFIGURABLE_STRATEGY_H */