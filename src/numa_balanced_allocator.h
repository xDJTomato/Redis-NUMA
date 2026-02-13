/* numa_balanced_allocator.h - 平衡NUMA内存分配器
 *
 * 解决CXL环境下内存分配不平衡问题，实现跨NUMA节点的智能负载均衡
 */

#ifndef NUMA_BALANCED_ALLOCATOR_H
#define NUMA_BALANCED_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

/* 负载均衡策略 */
#define BALANCE_STRATEGY_ROUND_ROBIN    0  /* 轮询分配 */
#define BALANCE_STRATEGY_WEIGHTED       1  /* 加权分配 */
#define BALANCE_STRATEGY_PRESSURE_BASED 2  /* 压力感知分配 */

/* 节点权重配置 */
typedef struct {
    int node_id;
    int weight;              /* 分配权重 (1-100) */
    size_t reserved_memory;  /* 保留内存大小 */
    int cxl_distance;        /* CXL访问延迟等级 */
} node_weight_config_t;

/* 负载均衡配置 */
typedef struct {
    int strategy;                    /* 分配策略 */
    node_weight_config_t *weights;   /* 节点权重数组 */
    int num_weights;                 /* 权重配置数量 */
    double balance_threshold;        /* 负载不平衡阈值 */
    uint64_t rebalance_interval_us;  /* 重新平衡间隔 */
    int enable_cxl_optimization;     /* 是否启用CXL优化 */
} balance_config_t;

/* 节点负载统计 */
typedef struct {
    int node_id;
    size_t total_memory;
    size_t used_memory;
    size_t free_memory;
    double utilization_rate;
    uint64_t allocation_count;
    uint64_t bytes_allocated;
    int cxl_latency_class;  /* CXL延迟等级 */
} node_load_stats_t;

/* 平衡分配器上下文 */
typedef struct {
    balance_config_t config;
    node_load_stats_t *node_stats;
    int num_nodes;
    int initialized;
    int current_rr_index;    /* 轮询索引 */
    uint64_t last_rebalance;
} balanced_allocator_t;

/* ========== Public API ========== */

/* 初始化平衡分配器 */
int numa_balanced_init(const balance_config_t *config);

/* 清理平衡分配器 */
void numa_balanced_cleanup(void);

/* 智能分配内存 - 根据负载情况选择最优节点 */
void *numa_balanced_malloc(size_t size);

/* 智能分配清零内存 */
void *numa_balanced_calloc(size_t nmemb, size_t size);

/* 在指定节点分配内存 */
void *numa_balanced_malloc_onnode(size_t size, int node);

/* 获取最适合分配的节点 */
int numa_balanced_get_best_node(size_t size);

/* 更新节点负载统计 */
int numa_balanced_update_stats(void);

/* 检查是否需要重新平衡 */
int numa_balanced_need_rebalance(void);

/* 执行负载重新平衡 */
int numa_balanced_rebalance(void);

/* 获取节点负载信息 */
const node_load_stats_t* numa_balanced_get_node_stats(int node_id);

/* 获取所有节点负载信息 */
const node_load_stats_t* numa_balanced_get_all_stats(int *num_nodes);

/* 动态调整权重 */
int numa_balanced_adjust_weight(int node_id, int new_weight);

/* 设置CXL优化参数 */
int numa_balanced_set_cxl_params(int enable_optimization, int latency_threshold_ms);

#endif /* NUMA_BALANCED_ALLOCATOR_H */