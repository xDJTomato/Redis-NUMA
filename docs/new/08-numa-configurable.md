# 可配置策略框架

## 模块概述

`numa_configurable_strategy.c/h` 提供运行时可配置的 NUMA 内存分配策略框架。支持 10 种分配策略，通过原子操作实现无锁分配路径，每秒由 serverCron 更新压力权重。

**版本**：v3.0（WEIGHTED_INTERLEAVE 策略，无锁分配）

## 策略枚举

```c
typedef enum {
    NUMA_STRATEGY_CONFIG_LOCAL_FIRST = 0,      // 本地优先：固定返回 node 0
    NUMA_STRATEGY_CONFIG_INTERLEAVE,           // 交错分配：rand_r 随机选择
    NUMA_STRATEGY_CONFIG_ROUND_ROBIN,          // 轮询分配：thread-local 计数器
    NUMA_STRATEGY_CONFIG_WEIGHTED,             // 静态加权：持锁读权重数组
    NUMA_STRATEGY_CONFIG_PRESSURE_AWARE,       // 压力感知：选择利用率最低的节点
    NUMA_STRATEGY_CONFIG_CXL_OPTIMIZED,        // CXL 优化：小对象本地、大对象远端
    NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE,  // 压力感知权重交错（默认策略）
    NUMA_STRATEGY_CONFIG_ADAPTIVE,             // 自适应策略（待实现，fallback node 0）
    NUMA_STRATEGY_CONFIG_LATENCY_AWARE,        // 延迟感知策略（待实现，fallback node 0）
    NUMA_STRATEGY_CONFIG_COUNT                 // 策略总数哨兵
} numa_config_strategy_type_t;
```

| 策略 | 实现状态 | 锁依赖 | 选择逻辑 |
|------|---------|--------|---------|
| `local_first` | 完整 | 无锁 | 固定返回 node 0 |
| `interleaved` | 完整 | 无锁 | `rand_r(&seed) % num_nodes` |
| `round_robin` | 完整 | 无锁 | thread-local 计数器递增取模 |
| `weighted` | 完整 | **短锁** | 持锁复制权重数组，锁外计算加权随机 |
| `pressure_aware` | 完整 | 无锁 | 遍历节点，选择利用率最低的 |
| `cxl_optimized` | 完整 | 无锁 | size < threshold → node 0，否则 node 1 |
| `weighted_interleave` | 完整（默认） | 无锁 | `atomicGet` 读压力权重，加权随机 |
| `adaptive` | 待实现 | — | fallback 返回 node 0 |
| `latency_aware` | 待实现 | — | fallback 返回 node 0 |

## WEIGHTED_INTERLEAVE 策略详解

### 核心设计

压力越大的节点分配概率越低，权重更新与分配路径完全解耦：

```
serverCron (每秒)                    分配路径 (每次 zmalloc)
        │                                    │
        ▼                                    ▼
读取 numaGetNodePressure()          atomicGet(pressure_weights[i])
        │                                    │
        ▼                                    ▼
weight = (1 - pressure) * 100       加权随机选择节点
        │
        ▼
atomicSet(pressure_weights[i], w)
```

### 权重计算公式

```c
// 每秒由 serverCron 调用
void numa_config_update_pressure_weights(void) {
    for (int i = 0; i < num_nodes; i++) {
        double p = numaGetNodePressure(i);  // 读取 /sys/devices/system/node/nodeX/meminfo
        int w = (int)((1.0 - p) * 100);
        if (w < 1) w = 1;  // 最低权重 1，保证所有节点都有分配机会
        atomicSet(g_runtime_state.pressure_weights[i], w);
    }
}
```

### 分配路径（无锁）

```c
case NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE: {
    int total_weight = 0;
    int w[16];
    int n = (num_nodes <= 16) ? num_nodes : 16;
    for (int i = 0; i < n; i++) {
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
```

### 示例：双节点 QEMU 环境

| 场景 | Node 0 压力 | Node 1 压力 | Node 0 权重 | Node 1 权重 | Node 0 分配概率 |
|------|------------|------------|------------|------------|---------------|
| 初始状态 | 0% | 0% | 100 | 100 | 50% |
| 数据填充中 | 60% | 30% | 40 | 70 | 36% |
| Node 0 过载 | 90% | 40% | 10 | 60 | 14% |
| 均衡状态 | 50% | 50% | 50 | 50 | 50% |

## 运行时状态

```c
typedef struct {
    numa_strategy_config_t config;
    int current_strategy;
    uint64_t last_rebalance_time;
    redisAtomic int *allocation_counters;        // 各节点分配计数器（原子）
    redisAtomic size_t *bytes_allocated_per_node; // 各节点已分配字节数（原子）
    redisAtomic int *pressure_weights;            // 压力权重数组（WEIGHTED_INTERLEAVE 策略）
    redisAtomic int *bw_usage_percent;            // 各节点带宽利用率百分比（原子）
    double *distance_factors;                     // NUMA 距离因子（sqrt(min_dist/d)）
} numa_runtime_state_t;
```

### enabled_nodes_mask

`config.enabled_nodes_mask` 是一个 64 位掩码，控制哪些 NUMA 节点参与分配。在初始化时默认为 0（表示使用所有可用节点）。可通过 `numa_config_set_enabled_nodes_mask(mask)` 设置。

### distance_factors

`distance_factors` 数组在初始化时自动根据 NUMA 拓扑计算。对于每个节点 i：

```c
distance_factors[i] = sqrt((double)min_dist / numa_distance(current_node, i));
```

距离越远的节点因子越小，用于 WEIGHTED_INTERLEAVE 策略中辅助权重计算。

## 默认配置

```c
// init_runtime_state() 中设置
config.strategy_type = NUMA_STRATEGY_CONFIG_LOCAL_FIRST;  // 初始默认
config.balance_threshold = 0.3;
config.auto_rebalance = 1;
config.rebalance_interval_us = 5000000;  // 5秒

// server.c 中覆盖为 WEIGHTED_INTERLEAVE
numa_config_set_strategy(NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE);
```

## 核心 API

| 函数 | 功能 | 锁依赖 |
|------|------|--------|
| `numa_config_strategy_init()` | 初始化策略系统，分配计数器数组 | 全局锁（仅初始化） |
| `numa_config_strategy_cleanup()` | 清理策略系统，释放内存 | 全局锁 |
| `numa_config_set_strategy(type)` | 设置当前分配策略 | 全局锁 |
| `numa_config_get_best_node(size)` | 根据当前策略选择最优节点 | 视策略而定 |
| `numa_config_update_pressure_weights()` | 更新压力权重（serverCron 调用） | 无锁 |
| `numa_config_set_node_weights(w, n)` | 设置静态权重（WEIGHTED 策略） | 全局锁 |
| `numa_config_get_statistics(...)` | 获取各节点分配统计 | 无锁（atomicGet） |
| `numa_config_load_from_file(path)` | 从 key=value 文件加载配置 | 全局锁 |

## 命令行接口

通过 `NUMA CONFIG` 命令（在 `numa_command.c` 中实现）：

```
NUMA CONFIG SET strategy weighted_interleave
NUMA CONFIG GET
NUMA CONFIG HELP
```

## 与其他模块的关系

- **zmalloc.c**：调用 `numa_config_get_best_node(size)` 确定分配目标节点
- **server.c**：初始化时调用 `numa_config_strategy_init()`，每秒调用 `numa_config_update_pressure_weights()`
- **numa_composite_lru.c**：通过 `numaGetNodePressure()` 读取节点压力，影响迁移决策

## 无锁分配路径设计

### 核心原则

**读写分离**：热路径（zmalloc 分配）只读不写配置；冷路径（管理命令 `NUMA CONFIG SET`）持锁修改配置。统计计数器用 `atomicIncr` 无锁更新。

### 策略锁依赖分类

| 策略 | 锁状态 | 原因 |
|------|--------|------|
| `LOCAL_FIRST` | 无锁 | 固定返回 node 0 |
| `INTERLEAVE` | 无锁 | 每线程独立 `rand_r` seed |
| `ROUND_ROBIN` | 无锁 | 每线程独立 `rr_index` |
| `CXL_OPTIMIZED` | 无锁 | 仅读取 `min_allocation_size` 和 `num_nodes` |
| `PRESSURE_AWARE` | 无锁 | 读取外部节点利用率 |
| `WEIGHTED` | **短锁** | 持锁复制权重数组，锁外计算 |
| `WEIGHTED_INTERLEAVE` | 无锁 | `atomicGet` 读压力权重（**默认策略**） |

### Redis Atomicvar API

分配路径使用 Redis `src/atomicvar.h` 原子操作抽象，支持三级后端自动切换（C11 `_Atomic` → `__atomic_*` → `__sync_*`）：

```c
redisAtomic int counter;
atomicIncr(var, count)    // var += count（relaxed）
atomicGet(var, dst)       // dst = var（relaxed）
atomicSet(var, value)     // var = value（relaxed）
```

全部使用 `memory_order_relaxed` 语义：计数器只增不减，读取者不要求精确瞬时值。

### 安全性论证

配置字段（`strategy_type`、`num_nodes`）可不持锁读取，因为：
1. 写入仅在管理命令中（极低频），写入时仍持 `g_config_mutex`
2. 热路径读到过渡值的最坏情况：一次分配落在非最优节点，无正确性影响
3. `int` 类型在 x86_64 上天然原子对齐，不存在 torn read
