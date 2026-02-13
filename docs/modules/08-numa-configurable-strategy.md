# 08-NUMA可配置分配策略

## 模块概述

NUMA可配置分配策略模块提供运行时动态配置的NUMA内存分配机制，允许用户通过配置文件或命令行指令灵活调整内存分配策略，以适应不同的工作负载和硬件环境。

## 设计目标

- **灵活性**：支持多种NUMA分配策略的动态切换
- **可配置性**：通过配置文件和运行时命令调整策略参数
- **性能优化**：针对CXL环境和多NUMA节点场景优化
- **监控能力**：提供详细的分配统计和负载信息

## 核心功能

### 1. 多种分配策略

| 策略类型 | 描述 | 适用场景 |
|---------|------|----------|
| LOCAL_FIRST | 本地节点优先分配 | 单节点优化，最小延迟 |
| INTERLEAVE | 交错分配到所有节点 | 负载均衡，多节点环境 |
| ROUND_ROBIN | 轮询分配 | 简单均衡，均匀分布 |
| WEIGHTED | 加权分配 | 按节点性能差异化分配 |
| PRESSURE_AWARE | 压力感知分配 | 动态负载均衡 |
| CXL_OPTIMIZED | CXL优化分配 | CXL内存架构优化 |

### 2. 配置管理

#### 配置文件格式 (numa.conf)
```ini
[numa_strategy]
strategy = interleaved
balance_threshold = 0.3
auto_rebalance = yes
rebalance_interval = 5000000

[node_weights]
node_0 = 100
node_1 = 80
node_2 = 60
node_3 = 40

[cxl_optimization]
enable = yes
min_allocation_size = 1024
```

#### 运行时命令
```bash
# 查看当前配置
NUMACONFIG GET

# 设置分配策略
NUMACONFIG SET strategy weighted

# 调整节点权重
NUMACONFIG SET weight 0 100
NUMACONFIG SET weight 1 80

# 启用CXL优化
NUMACONFIG SET cxl_optimization on

# 手动触发重新平衡
NUMACONFIG REBALANCE

# 查看统计信息
NUMACONFIG STATS
```

### 3. 监控和统计

#### 分配统计
- 各节点分配次数统计
- 各节点内存使用量跟踪
- 策略切换历史记录
- 性能指标收集

#### 负载监控
- 节点内存利用率计算
- 负载不均衡检测
- 自动重新平衡触发

## API接口

### 配置管理API
```c
// 初始化策略系统
int numa_config_strategy_init(void);

// 从配置文件加载
int numa_config_load_from_file(const char *config_file);

// 应用策略配置
int numa_config_apply_strategy(const numa_strategy_config_t *config);

// 获取当前配置
const numa_strategy_config_t* numa_config_get_current(void);
```

### 运行时控制API
```c
// 设置分配策略
int numa_config_set_strategy(numa_config_strategy_type_t strategy);

// 设置节点权重
int numa_config_set_node_weights(int *weights, int num_nodes);

// 启用CXL优化
int numa_config_set_cxl_optimization(int enable);
```

### 内存分配API
```c
// 智能内存分配
void *numa_config_malloc(size_t size);

// 智能清零分配
void *numa_config_calloc(size_t nmemb, size_t size);

// 指定节点分配
void *numa_config_malloc_onnode(size_t size, int node);
```

### 查询统计API
```c
// 获取分配统计
void numa_config_get_statistics(uint64_t *allocations_per_node, 
                               size_t *bytes_per_node,
                               int num_nodes);

// 获取节点利用率
double numa_config_get_node_utilization(int node_id);

// 检查是否需要重新平衡
int numa_config_needs_rebalance(void);
```

## 实现架构

### 核心组件

1. **配置管理层** - 处理配置文件解析和运行时配置更新
2. **策略执行层** - 实现各种NUMA分配策略算法
3. **监控统计层** - 收集分配统计和负载信息
4. **命令接口层** - 提供Redis命令行接口

### 数据结构

```c
// 策略配置
typedef struct {
    numa_config_strategy_type_t strategy_type;
    int *node_weights;
    int num_nodes;
    double balance_threshold;
    int enable_cxl_optimization;
    // ... 其他配置参数
} numa_strategy_config_t;

// 运行时状态
typedef struct {
    numa_strategy_config_t config;
    int current_strategy;
    uint64_t last_rebalance_time;
    int *allocation_counters;
    size_t *bytes_allocated_per_node;
} numa_runtime_state_t;
```

## 性能考虑

### 优化策略

1. **线程局部存储** - 使用TLS减少锁竞争
2. **批量分配** - 减少系统调用频率
3. **缓存友好** - 优化数据结构布局
4. **延迟计算** - 按需计算统计信息

### 负载均衡算法

```c
// 加权轮询算法示例
static int weighted_round_robin_select(void) {
    static __thread int current_weight_index = 0;
    int selected_node = 0;
    int max_weight = 0;
    
    // 根据权重选择节点
    for (int i = 0; i < num_nodes; i++) {
        if (node_weights[i] > max_weight) {
            max_weight = node_weights[i];
            selected_node = i;
        }
    }
    
    return selected_node;
}
```

## 测试方案

### 单元测试
- 各策略算法正确性验证
- 配置解析和应用测试
- 边界条件和错误处理测试

### 集成测试
- 与Redis核心功能集成测试
- 多线程并发分配测试
- CXL环境兼容性测试

### 性能测试
- 不同策略的性能对比
- 负载均衡效果验证
- 内存使用效率评估

## 部署指南

### 编译配置
```makefile
# Makefile中添加
FINAL_LIBS += -lnuma
FINAL_CFLAGS += -DHAVE_NUMA_CONFIGURABLE
```

### 配置文件部署
```bash
# 复制默认配置文件
cp configs/numa.conf.example /etc/redis/numa.conf

# 设置适当权限
chown redis:redis /etc/redis/numa.conf
chmod 644 /etc/redis/numa.conf
```

### 启动参数
```bash
redis-server --numa-config /etc/redis/numa.conf
```

## 故障排除

### 常见问题

1. **配置文件解析失败**
   - 检查文件格式和权限
   - 验证配置项有效性

2. **策略切换不生效**
   - 确认策略类型支持
   - 检查运行时权限

3. **性能不如预期**
   - 分析负载模式是否匹配策略
   - 调整配置参数

### 监控命令
```bash
# 查看详细状态
NUMACONFIG STATUS

# 获取分配统计
NUMACONFIG STATS

# 实时监控
NUMACONFIG MONITOR
```

## 版本历史

### v1.0 (当前版本)
- 基础可配置策略框架
- 6种分配策略实现
- 配置文件和命令行接口
- 基本监控统计功能

### 规划功能
- 更智能的自适应策略
- 机器学习辅助优化
- 更丰富的监控指标
- 图形化配置界面

---
*本文档遵循Redis NUMA项目文档规范*