# 可配置策略框架

## 模块概述

`numa_configurable_strategy.c/h` 提供运行时可配置的 NUMA 内存分配策略。支持通过配置文件和命令行指令动态调整分配策略，无需重启服务。

## 支持的策略类型

```c
typedef enum {
    NUMA_STRATEGY_CONFIG_LOCAL_FIRST = 0,    // 本地节点优先
    NUMA_STRATEGY_CONFIG_INTERLEAVE,         // 交错分配（负载均衡）
    NUMA_STRATEGY_CONFIG_ROUND_ROBIN,        // 轮询分配
    NUMA_STRATEGY_CONFIG_WEIGHTED,           // 加权分配
    NUMA_STRATEGY_CONFIG_PRESSURE_AWARE,     // 压力感知
    NUMA_STRATEGY_CONFIG_CXL_OPTIMIZED       // CXL 优化
} numa_config_strategy_type_t;
```

### 策略说明

| 策略 | 适用场景 | 行为 |
|------|---------|------|
| **Local First** | 通用场景 | 优先在当前 CPU 所在节点分配 |
| **Interleave** | 负载均衡 | 轮流在各节点分配，均匀分布 |
| **Round Robin** | 均匀分布 | 严格轮询，不感知 CPU 位置 |
| **Weighted** | 异构系统 | 按权重分配（节点内存大小不同） |
| **Pressure Aware** | 高负载 | 根据节点内存压力动态调整 |
| **CXL Optimized** | CXL 环境 | 热数据 DRAM，冷数据 CXL |

## 核心数据结构

### 策略配置

```c
typedef struct {
    numa_config_strategy_type_t strategy_type;  // 策略类型
    int *node_weights;                          // 各节点权重数组
    int num_nodes;                              // 节点数量
    double balance_threshold;                   // 平衡阈值
    int enable_cxl_optimization;                // 是否启用 CXL 优化
    size_t min_allocation_size;                 // 最小分配大小
    int auto_rebalance;                         // 是否自动重新平衡
    uint64_t rebalance_interval_us;             // 重新平衡间隔（微秒）
} numa_strategy_config_t;
```

### 运行时状态

```c
typedef struct {
    numa_strategy_config_t config;              // 当前配置
    int current_strategy;                       // 当前使用的策略
    uint64_t last_rebalance_time;               // 上次重新平衡时间
    int *allocation_counters;                   // 各节点分配计数器
    size_t *bytes_allocated_per_node;           // 各节点已分配字节数
} numa_runtime_state_t;
```

## 配置管理 API

### 初始化

```c
int numa_config_strategy_init(void);
void numa_config_strategy_cleanup(void);
```

### 从文件加载

```c
int numa_config_load_from_file(const char *config_file);
```

支持 JSON 格式配置文件。

### 应用配置

```c
int numa_config_apply_strategy(const numa_strategy_config_t *config);
```

### 获取当前配置

```c
const numa_strategy_config_t* numa_config_get_current(void);
```

## 运行时控制 API

### 设置策略

```c
int numa_config_set_strategy(numa_config_strategy_type_t strategy);
```

立即生效，后续分配使用新策略。

### 设置节点权重

```c
int numa_config_set_node_weights(int *weights, int num_nodes);
```

用于 Weighted 策略。

### CXL 优化开关

```c
int numa_config_set_cxl_optimization(int enable);
```

### 设置平衡阈值

```c
int numa_config_set_balance_threshold(double threshold);
```

当节点间内存差异超过阈值时触发重新平衡。

### 手动触发重新平衡

```c
int numa_config_trigger_rebalance(void);
```

## 内存分配 API

### 智能分配

```c
void *numa_config_malloc(size_t size);
void *numa_config_calloc(size_t nmemb, size_t size);
```

根据当前配置自动选择最优节点。

### 指定节点分配

```c
void *numa_config_malloc_onnode(size_t size, int node);
```

## 查询和统计 API

### 获取分配统计

```c
void numa_config_get_statistics(uint64_t *allocations_per_node,
                               size_t *bytes_per_node,
                               int num_nodes);
```

### 获取节点利用率

```c
double numa_config_get_node_utilization(int node_id);
```

### 检查是否需要重新平衡

```c
int numa_config_needs_rebalance(void);
```

### 获取最佳分配节点

```c
int numa_config_get_best_node(size_t size);
```

## 策略实现细节

### Local First

```c
int numa_config_get_best_node(size_t size) {
    // 1. 获取当前线程绑定的 CPU
    int cpu = sched_getcpu();

    // 2. 查找 CPU 所属的 NUMA 节点
    int node = cpu_to_node(cpu);

    // 3. 检查节点是否可用
    if (is_node_available(node)) {
        return node;
    }

    // 4. 回退：选择负载最低的节点
    return find_least_loaded_node();
}
```

### Interleave

```c
static __thread int interleave_index = 0;

int numa_config_get_best_node(size_t size) {
    int node = interleave_index % num_nodes;
    interleave_index++;
    return node;
}
```

### Weighted

```c
int numa_config_get_best_node(size_t size) {
    // 1. 计算总权重
    int total_weight = 0;
    for (int i = 0; i < num_nodes; i++) {
        total_weight += node_weights[i];
    }

    // 2. 根据权重选择节点
    int target = random() % total_weight;
    int cumulative = 0;
    for (int i = 0; i < num_nodes; i++) {
        cumulative += node_weights[i];
        if (target < cumulative) return i;
    }
    return 0;
}
```

### Pressure Aware

```c
int numa_config_get_best_node(size_t size) {
    double min_pressure = 1.0;
    int best_node = 0;

    for (int i = 0; i < num_nodes; i++) {
        double pressure = calculate_node_pressure(i);
        if (pressure < min_pressure) {
            min_pressure = pressure;
            best_node = i;
        }
    }
    return best_node;
}
```

## 重新平衡机制

### 触发条件

```c
int numa_config_needs_rebalance(void) {
    if (!auto_rebalance) return 0;

    // 1. 检查间隔
    uint64_t now = get_time_us();
    if (now - last_rebalance_time < rebalance_interval_us) return 0;

    // 2. 检查节点间差异
    double max_diff = 0;
    for (int i = 0; i < num_nodes; i++) {
        for (int j = i + 1; j < num_nodes; j++) {
            double diff = abs(bytes_allocated[i] - bytes_allocated[j]);
            double ratio = diff / max(bytes_allocated[i], bytes_allocated[j]);
            if (ratio > balance_threshold) return 1;
        }
    }
    return 0;
}
```

### 重新平衡执行

```c
int numa_config_trigger_rebalance(void) {
    // 1. 找出负载最高的节点
    int source = find_most_loaded_node();
    int target = find_least_loaded_node();

    // 2. 计算需要迁移的量
    size_t to_migrate = calculate_migration_size(source, target);

    // 3. 执行迁移
    migrate_keys(source, target, to_migrate);

    // 4. 更新统计
    last_rebalance_time = get_time_us();
    return 0;
}
```

## 命令行接口

### 处理命令

```c
int numa_config_handle_command(int argc, char **argv);
```

支持的子命令：
- `GET`: 查询当前配置
- `SET strategy <name>`: 设置策略
- `SET weight <node> <value>`: 设置节点权重
- `SET cxl_optimization <on|off>`: CXL 优化开关
- `SET balance_threshold <value>`: 平衡阈值
- `REBALANCE`: 手动触发重新平衡
- `STATS`: 显示统计信息

### 显示状态

```c
void numa_config_show_status(void);
```

输出示例：
```
Current Strategy: local-first
Nodes: 2
Balance Threshold: 0.3
Auto Rebalance: enabled
CXL Optimization: disabled
Rebalance Interval: 60000000 us
Min Allocation Size: 16 bytes
Node Weights: [1, 1]
```

## 与其他模块的关系

### 被统一命令接口调用

```
numa_command.c
    │
    ├── NUMA CONFIG GET ──► numa_config_get_current()
    ├── NUMA CONFIG SET ──► numa_config_set_*()
    ├── NUMA CONFIG LOAD ──► numa_config_load_from_file()
    └── NUMA CONFIG REBALANCE ──► numa_config_trigger_rebalance()
```

### 与内存池的关系

分配策略影响 `numa_pool_alloc()` 的节点选择：

```c
void *numa_pool_alloc(size_t size, int node, size_t *total_size) {
    // node 参数由配置策略决定
    int target_node = numa_config_get_best_node(size);
    // ...
}
```

### 与 Composite LRU 的关系

Composite LRU 的 JSON 配置通过此模块加载：

```c
// 启动时
if (server.numa_migrate_config_file) {
    composite_lru_config_t cfg;
    composite_lru_load_config(server.numa_migrate_config_file, &cfg);
    composite_lru_apply_config(strategy, &cfg);
}
```

## JSON 配置文件

### 格式示例

```json
{
    "strategy_type": "local-first",
    "node_weights": [1, 1],
    "balance_threshold": 0.3,
    "auto_rebalance": true,
    "cxl_optimization": false,
    "rebalance_interval_us": 60000000,
    "min_allocation_size": 16
}
```

### 加载流程

```
numa_config_load_from_file(path)
    │
    ├── 打开 JSON 文件
    ├── 逐行解析键值对
    ├── 验证参数范围
    ├── 构建 numa_strategy_config_t
    └── 调用 numa_config_apply_strategy()
```

## 错误处理

所有 API 返回整数状态码：
- `0`: 成功
- `-1`: 一般错误
- `-2`: 参数无效
- `-3`: 内存不足

## 统计信息

```c
typedef struct {
    uint64_t *allocations_per_node;     // 各节点分配次数
    size_t *bytes_allocated_per_node;   // 各节点分配字节数
    uint64_t total_rebalances;          // 总重新平衡次数
    uint64_t total_migrations;          // 总迁移次数
} numa_config_stats_t;
```

查询：
```c
void numa_config_get_statistics(allocations, bytes, num_nodes);
```

## 使用场景

### 场景 1：双路 NUMA 服务器

```bash
# 设置本地优先策略
redis-cli NUMA CONFIG SET strategy local-first

# 设置权重（节点 0 内存更多）
redis-cli NUMA CONFIG SET weight 0 2
redis-cli NUMA CONFIG SET weight 1 1
```

### 场景 2：CXL 内存扩展

```bash
# 启用 CXL 优化
redis-cli NUMA CONFIG SET cxl_optimization on

# 热数据阈值（热数据保留在 DRAM）
redis-cli NUMA CONFIG SET balance_threshold 0.5
```

### 场景 3：负载均衡测试

```bash
# 切换到交错分配
redis-cli NUMA CONFIG SET strategy interleave

# 观察各节点内存分布
redis-cli NUMA CONFIG STATS
```
