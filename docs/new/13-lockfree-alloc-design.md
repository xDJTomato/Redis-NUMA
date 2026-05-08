# 13 - 分配路径无锁设计

## 背景

每次 `zmalloc()` 在 NUMA 模式下都经历以下调用链：

```
zmalloc() → numa_alloc_with_size() → numa_config_get_best_node()
    → select_best_node() → [无锁选择目标 NUMA 节点]
    → numa_slab_alloc() / numa_alloc_onnode() → [实际分配]
```

在 YCSB 多线程负载下，`select_best_node()` 中的 `pthread_mutex_lock(&g_config_mutex)` 成为瓶颈：64 线程每秒数万次分配全部争用同一把锁。Phase 1 填充阶段，CXL 版本吞吐仅为 vanilla 的 53%。

## Redis Atomicvar API

Redis 在 `src/atomicvar.h` 中提供了一套跨平台的原子操作抽象，支持三级后端自动切换：

| 后端 | 宏展开 | 条件 |
|------|--------|------|
| C11 `_Atomic` | `atomic_fetch_add_explicit(..., memory_order_relaxed)` | `__STDC_VERSION__ >= 201112L` |
| `__atomic_*` | `__atomic_add_fetch(...)` | GCC/Clang 内置 |
| `__sync_*` | `__sync_add_and_fetch(...)` | 兜底兼容 |

### API 一览

```c
#include "atomicvar.h"

// ── 变量声明 ──
redisAtomic int counter;           // → _Atomic int（C11）或 int（__sync 回退）

// ── 原子操作宏 ──
atomicIncr(var, count)             // var += count
atomicGetIncr(var, old, count)     // old = var; var += count
atomicDecr(var, count)             // var -= count
atomicGet(var, dst)                // dst = var
atomicSet(var, value)              // var = value
atomicGetWithSync(var, dst)        // dst = var（含完整内存屏障，跨线程 happens-before）
atomicSetWithSync(var, value)      // var = value（含完整内存屏障）
```

### Redis 核心中的典型用法

```c
// server.h — 声明
redisAtomic unsigned int lruclock;
redisAtomic long long stat_net_input_bytes;

// server.c — 写入（单写者，relaxed 即可）
atomicSet(server.lruclock, lruclock);

// networking.c — 多线程递增
atomicIncr(server.stat_net_input_bytes, nread);

// evict.c — 读取
atomicGet(server.lruclock, lruclock);

// networking.c — 多线程生产者-消费者（需同步屏障）
atomicGetWithSync(io_threads_pending[i], count);
atomicSetWithSync(io_threads_pending[i], count);
```

## 设计：分配路径无锁化

### 核心原则

**读写分离**：热路径（分配）只读不写配置；冷路径（管理命令）持锁修改配置。统计计数器用 `atomicIncr` 无锁更新。

### 策略分类与锁策略

| 策略 | 锁状态 | 原因 |
|------|--------|------|
| `LOCAL_FIRST` | **无锁** | 固定返回 node 0 |
| `INTERLEAVE` | **无锁** | 每线程独立 `rand_r` seed，无共享状态 |
| `ROUND_ROBIN` | **无锁** | 每线程独立 `rr_index`，无共享状态 |
| `CXL_OPTIMIZED` | **无锁** | 仅读取 `min_allocation_size` 和 `num_nodes` |
| `PRESSURE_AWARE` | **无锁** | 读取外部节点利用率数据（由 `numa_bw_monitor` 独立维护） |
| `WEIGHTED` | **短锁** | 需读取权重数组，短暂持锁复制后立即释放，计算在锁外 |
| `WEIGHTED_INTERLEAVE` | **无锁** | `atomicGet` 读压力权重，加权随机选择（**默认策略**） |
| `ADAPTIVE` | **无锁** | 待实现，fallback 返回 node 0 |
| `LATENCY_AWARE` | **无锁** | 待实现，fallback 返回 node 0 |

### 数据结构

```c
// numa_configurable_strategy.h
typedef struct {
    ...
    redisAtomic int    *allocation_counters;        // 原子计数器数组
    redisAtomic size_t *bytes_allocated_per_node;   // 原子字节计数器数组
    redisAtomic int    *pressure_weights;           // 压力权重数组（WEIGHTED_INTERLEAVE 策略）
} numa_runtime_state_t;
```

数组分配时使用正确的原子类型大小：
```c
g_runtime_state.allocation_counters      = zcalloc(num_nodes * sizeof(redisAtomic int));
g_runtime_state.bytes_allocated_per_node = zcalloc(num_nodes * sizeof(redisAtomic size_t));
g_runtime_state.pressure_weights         = zcalloc(num_nodes * sizeof(redisAtomic int));
// 初始权重 100（所有节点等概率分配）
for (int i = 0; i < num_nodes; i++) atomicSet(g_runtime_state.pressure_weights[i], 100);
```

### 热路径实现

```c
static int select_best_node(size_t size) {
    // 配置字段：plain read。策略类型 / 节点数仅在管理命令中变更，
    // 读到过渡值只影响瞬时分配的节点选择，无正确性风险。
    int strategy_type = g_runtime_state.config.strategy_type;
    int num_nodes     = g_runtime_state.config.num_nodes;
    int selected_node = 0;

    switch (strategy_type) {
    case NUMA_STRATEGY_CONFIG_LOCAL_FIRST:
        selected_node = 0;  // 无锁，固定值
        break;
    case NUMA_STRATEGY_CONFIG_INTERLEAVE:
        // 无锁：每线程独立 seed
        selected_node = rand_r(&seed) % num_nodes;
        break;
    case NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE: {
        // 无锁：atomicGet 读取压力权重，加权随机
        int total_weight = 0;
        int w[16];
        int n = (num_nodes <= 16) ? num_nodes : 16;
        for (int i = 0; i < n; i++) {
            atomicGet(g_runtime_state.pressure_weights[i], w[i]);
            total_weight += w[i];
        }
        if (total_weight > 0) {
            int r = rand_r(&seed) % total_weight;
            int cum = 0;
            for (int i = 0; i < n; i++) {
                cum += w[i];
                if (r < cum) { selected_node = i; break; }
            }
        }
        break;
    }
    ...
    }

    // 无锁原子递增：使用 Redis atomicvar.h API
    atomicIncr(g_runtime_state.allocation_counters[selected_node], 1);
    atomicIncr(g_runtime_state.bytes_allocated_per_node[selected_node], size);
    return selected_node;
}
```

### 读取计数器

```c
// 统计查询（NUMA CONFIG STATS）— 无锁读取
int alloc_count;
size_t alloc_bytes;
atomicGet(g_runtime_state.allocation_counters[i], alloc_count);
atomicGet(g_runtime_state.bytes_allocated_per_node[i], alloc_bytes);
```

### memory_order 选择

全部使用 `memory_order_relaxed`（`atomicIncr` / `atomicGet` 宏的默认语义），因为：
- 计数器只增不减，没有与其他变量的 happens-before 依赖
- 读取者不要求看到精确的瞬时值
- 这避免了不必要的内存屏障开销

如果需要跨线程同步（如配置变更通知），使用 `atomicGetWithSync` / `atomicSetWithSync`（`memory_order_seq_cst`）。

## 安全性论证

### 为什么配置字段可以不持锁读取

`strategy_type`、`num_nodes`、`min_allocation_size` 等字段：
1. **只在管理命令中写入**（`NUMA CONFIG SET`、`NUMA CONFIG LOAD`），写入频率极低
2. **写入时仍持 `g_config_mutex`**（管理命令路径不变）
3. **热路径读到过渡值的最坏情况**：一次分配的节点选择偏差 → 该次分配落在非最优节点 → 对系统无正确性影响
4. **`int` 类型在 x86_64 上是天然原子对齐的**，不存在 torn read

### `_Atomic` 类型与数组

C11 中 `_Atomic int *p` 是指向原子整数的指针，`p[i]` 是 `_Atomic int`。分配时使用 `sizeof(redisAtomic int)` 确保每个元素有正确的对齐和大小（在 x86_64 上 `sizeof(redisAtomic int) == sizeof(int)`，但代码不依赖此假设）。

## 与 Slab 分配器锁的关系

分配路径涉及两层独立的锁/无锁机制：

| 层级 | 锁 | 作用 |
|------|-----|------|
| 节点选择 | ~~`g_config_mutex`~~（已消除） | 保护分配目标节点决策 → 现为无锁 plain read + atomicGet |
| Slab 分配 | 原子 CAS（无锁） | Slab 位图的原子 CAS 操作，无需 pthread_mutex |

两层之间无依赖关系。`select_best_node()` 只决定目标节点号，实际的 Slab 内存分配由 `numa_slab_alloc()` 内部的原子 CAS 操作完成，完全无锁。

> **Pool 路径已移除**：原 Pool 分配器使用 `pthread_mutex_lock(&pool->lock)` 保护 chunk bump-pointer 和 free_list，已随 Pool 路径一起移除。当前 Slab 分配器（64KB slab + 3072bit 位图 + 原子 CAS）完全无锁。

## 验证

1. 编译：`make clean && make -j$(nproc)` 无新增警告
2. `redis-server` 正常启动，日志显示 `weighted_interleave` 策略
3. YCSB Phase 1 吞吐：8,528 → 10,626 ops/s（+24.6%）
4. `NUMA CONFIG STATS` 命令读取计数器正常
5. `NUMA CONFIG GET` 显示默认策略为 `weighted-interleave`
