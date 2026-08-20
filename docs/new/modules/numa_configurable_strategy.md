# numa_configurable_strategy — 可配置分配策略框架

**源文件**：`src/numa_configurable_strategy.c` / `src/numa_configurable_strategy.h`

## 1. 职责（Responsibility）

在 `zmalloc` 分配路径的最前端回答一个问题："这次分配应该放到哪个 NUMA 节点？"
它不管理内存本身（那是 `numa_pool` 的事），只负责**选节点**：提供 9 个可运行时
切换的策略名，覆盖从"完全不看拓扑"（`LOCAL_FIRST`）到"按实时内存压力动态加
权"（`WEIGHTED_INTERLEAVE`，当前默认）的整个谱系，并把节点选择这件高频操作做成
无锁或近似无锁的热路径。9 个策略名背后是 **7 套独立实现**：`WEIGHTED`/
`WEIGHTED_INTERLEAVE` 现在共享同一个 `select_weighted_node()` 加权随机辅助函数
（原来是两份逐字重复的循环，见 3.2 节），`ADAPTIVE`/`LATENCY_AWARE` 本就共享同
一段内核占位 fallback（见第 5 节）——两两合并之后，剩下 7 种彼此独立的行为。

## 2. 接口（Interface）

| 函数 | 作用 | 锁依赖 |
|---|---|---|
| `numa_config_strategy_init()` | 初始化策略系统，分配各节点计数器/权重数组 | 全局锁（仅初始化一次） |
| `numa_config_strategy_cleanup()` | 清理策略系统，释放内存 | 全局锁 |
| `numa_config_set_strategy(type)` | 切换当前分配策略 | 全局锁 |
| `numa_config_get_best_node(size)` | **热路径入口**：按当前策略返回目标节点 | 视策略而定，见第 3.3 节 |
| `numa_config_update_pressure_weights()` | 按各节点内存压力重新计算权重（`serverCron` 每秒调用一次） | 无锁 |
| `numa_config_set_node_weights(w, n)` | 手动设置静态权重（供 `WEIGHTED` 策略使用） | 全局锁 |
| `numa_config_get_statistics(...)` | 读取各节点分配计数/字节数统计 | 无锁（`atomicGet`） |
| `numa_config_load_from_file(path)` | 从 `key=value` 文件加载配置 | 全局锁 |

对外通过 `NUMA CONFIG` 命令暴露（`src/numa_command.c`）：

```
NUMA CONFIG SET strategy weighted_interleave
NUMA CONFIG GET
NUMA CONFIG HELP
```

## 3. 内部结构与关键路径（Internal Structure & Key Paths）

### 3.1 九个策略名，七套实现

```text
               numa_config_get_best_node(size) 节点决策入口
                                     │
      ┌──────────────────────────────┼──────────────────────────────┐
      ▼                              ▼                              ▼
  [local_first]               [round_robin]                  [interleaved]
  固定返回 Node 0             TLS 计数器轮询取模             TLS 种子伪随机取模
      │                              │                              │
      ▼                              ▼                              ▼
  [cxl_optimized]             [pressure_aware]               [weighted_interleave] (默认)
  size < 阈值 ? Node 0 : 1    选节点压力最低者               按节点压力动态加权随机
                                     │                              │
                                     ▼                              ▼
                              [weighted] (静态)              [adaptive / latency_aware]
                              持短锁读静态权重加权随机       内核占位 -> 推荐 NUMAflow DAG 编排
```

```c
typedef enum {
    NUMA_STRATEGY_CONFIG_LOCAL_FIRST = 0,      /* 本地优先：固定返回 node 0 */
    NUMA_STRATEGY_CONFIG_INTERLEAVE,           /* 交错分配：rand_r 随机选择 */
    NUMA_STRATEGY_CONFIG_ROUND_ROBIN,          /* 轮询分配：thread-local 计数器 */
    NUMA_STRATEGY_CONFIG_WEIGHTED,             /* 静态加权：持锁读权重数组 */
    NUMA_STRATEGY_CONFIG_PRESSURE_AWARE,       /* 压力感知：选择利用率最低的节点 */
    NUMA_STRATEGY_CONFIG_CXL_OPTIMIZED,        /* CXL 优化：小对象本地、大对象远端 */
    NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE,  /* 压力感知权重交错（当前默认策略） */
    NUMA_STRATEGY_CONFIG_ADAPTIVE,             /* 内核侧为占位实现，见第 5 节 */
    NUMA_STRATEGY_CONFIG_LATENCY_AWARE,        /* 内核侧为占位实现，见第 5 节 */
    NUMA_STRATEGY_CONFIG_COUNT                 /* 策略总数哨兵 */
} numa_config_strategy_type_t;
```

| 策略 | 实现状态 | 锁依赖 | 选择逻辑 |
|---|---|---|---|
| `local_first` | 完整 | 无锁 | 固定返回 node 0 |
| `interleaved` | 完整 | 无锁 | `rand_r(&seed) % num_nodes` |
| `round_robin` | 完整 | 无锁 | thread-local 计数器递增取模 |
| `weighted` | 完整 | **短锁** | `select_weighted_node(n, 0)`：持锁复制静态权重数组，锁外计算加权随机 |
| `pressure_aware` | 完整 | 无锁 | 遍历节点，选择 `numa_bw_get_node_pressure()` 最低的 |
| `cxl_optimized` | 完整 | 无锁 | `size < threshold` → node 0，否则 node 1 |
| `weighted_interleave` | 完整（**默认**） | 无锁 | `select_weighted_node(n, 1)`：`atomicGet` 读压力权重，加权随机 |
| `adaptive` | **内核侧占位** | — | fallback 返回 node 0 |
| `latency_aware` | **内核侧占位** | — | fallback 返回 node 0 |

`weighted`/`weighted_interleave` 现在共用同一个 `select_weighted_node(num_nodes,
use_pressure_weights)` 辅助函数（`src/numa_configurable_strategy.c`）——两者过去
是两份逐字重复的"加权随机选择"循环，唯一区别只是权重的来源（管理员静态设置的
数组 vs. `serverCron` 按压力更新的数组），现在合并成一份实现，用一个 bool 参数
切换权重来源；锁行为不变（`weighted` 走短锁复制、`weighted_interleave` 走
`atomicGet`，见 3.2/3.3 节）。

（源码 `src/numa_configurable_strategy.c` 中 `case NUMA_STRATEGY_CONFIG_ADAPTIVE` 与
`case NUMA_STRATEGY_CONFIG_LATENCY_AWARE` 目前是同一段 fallback 逻辑，本文档对此
如实记录，不夸大其完成度——完整实现在别处，见第 5 节。）

### 3.2 WEIGHTED_INTERLEAVE：默认策略详解

权重更新（慢，`serverCron` 每秒一次）与分配路径（热，每次 `zmalloc`）完全解耦：

```
serverCron（每秒）                    分配路径（每次 zmalloc）
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

权重计算（`numa_config_update_pressure_weights()`）：

```c
void numa_config_update_pressure_weights(void) {
    for (int i = 0; i < num_nodes; i++) {
        double p = numaGetNodePressure(i);  /* 薄封装，转发到 numa_bw_monitor.c
                                                的 numa_bw_get_node_pressure()：
                                                evict_numa 与本模块共享的同一
                                                个压力信号，见 numa_bw_monitor.md */
        int w = (int)((1.0 - p) * 100);
        if (w < 1) w = 1;  /* 最低权重 1，保证所有节点都有机会被选中 */
        atomicSet(g_runtime_state.pressure_weights[i], w);
    }
}
```

分配路径（无锁，`numa_config_get_best_node()` 内部）：

```c
case NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE: {
    int total_weight = 0, w[16];
    int n = (num_nodes <= 16) ? num_nodes : 16;
    for (int i = 0; i < n; i++) {
        atomicGet(g_runtime_state.pressure_weights[i], w[i]);
        total_weight += w[i];
    }
    if (total_weight > 0) {
        static __thread unsigned int seed = 0;
        if (seed == 0) seed = (unsigned int)(getpid() ^ (uintptr_t)pthread_self());
        int r = rand_r(&seed) % total_weight, cum = 0;
        for (int i = 0; i < n; i++) {
            cum += w[i];
            if (r < cum) { selected_node = i; break; }
        }
    }
    break;
}
```

示例（双节点 QEMU 环境下权重如何随压力变化）：

| 场景 | Node 0 压力 | Node 1 压力 | Node 0 权重 | Node 1 权重 | Node 0 分配概率 |
|---|---|---|---|---|---|
| 初始状态 | 0% | 0% | 100 | 100 | 50% |
| 数据填充中 | 60% | 30% | 40 | 70 | 36% |
| Node 0 过载 | 90% | 40% | 10 | 60 | 14% |
| 均衡状态 | 50% | 50% | 50 | 50 | 50% |

### 3.3 无锁分配路径设计

**核心原则：读写分离**——热路径（`zmalloc` 分配时选节点）只读不写配置；冷路径
（管理命令 `NUMA CONFIG SET`）才持锁修改配置。统计计数器用 `atomicIncr` 无锁更新。

| 策略 | 锁状态 | 原因 |
|---|---|---|
| `LOCAL_FIRST` | 无锁 | 固定返回 node 0 |
| `INTERLEAVE` | 无锁 | 每线程独立 `rand_r` seed |
| `ROUND_ROBIN` | 无锁 | 每线程独立 `rr_index` |
| `CXL_OPTIMIZED` | 无锁 | 仅读取 `min_allocation_size` 与 `num_nodes` |
| `PRESSURE_AWARE` | 无锁 | 读取外部节点利用率 |
| `WEIGHTED` | **短锁** | 持锁复制权重数组，锁外计算 |
| `WEIGHTED_INTERLEAVE` | 无锁 | `atomicGet` 读压力权重（**默认策略**） |

原子操作走 Redis 自己的 `src/atomicvar.h` 抽象（三级后端自动切换：C11 `_Atomic` →
`__atomic_*` → `__sync_*`），全部使用 `memory_order_relaxed` 语义：

```c
redisAtomic int counter;
atomicIncr(var, count)    /* var += count（relaxed） */
atomicGet(var, dst)       /* dst = var（relaxed） */
atomicSet(var, value)     /* var = value（relaxed） */
```

安全性论证——配置字段（`strategy_type`、`num_nodes`）可以不持锁读取，因为：
1. 写入只发生在管理命令中（极低频），写入时仍持有 `g_config_mutex`；
2. 热路径读到过渡值的最坏后果是一次分配落在非最优节点，不影响正确性；
3. `int` 在 x86_64 上天然对齐，不存在 torn read。

### 3.4 运行时状态与配置

```c
typedef struct {
    numa_strategy_config_t config;
    int current_strategy;
    uint64_t last_rebalance_time;
    redisAtomic int *allocation_counters;        /* 各节点分配计数器 */
    redisAtomic size_t *bytes_allocated_per_node; /* 各节点已分配字节数 */
    redisAtomic int *pressure_weights;            /* 压力权重（WEIGHTED_INTERLEAVE 用） */
    redisAtomic int *bw_usage_percent;            /* 各节点带宽利用率百分比 */
    double *distance_factors;                     /* NUMA 距离因子 sqrt(min_dist/d) */
} numa_runtime_state_t;
```

`config.enabled_nodes_mask` 是一个 64 位掩码，控制参与分配的节点集合（默认 0 =
使用全部可用节点，可通过 `numa_config_set_enabled_nodes_mask(mask)` 修改）。
`distance_factors[i] = sqrt(min_dist / numa_distance(current_node, i))` 在初始化
时按 NUMA 拓扑自动计算，距离越远的节点因子越小，辅助 `WEIGHTED_INTERLEAVE` 的权
重计算。

默认配置：

```c
/* init_runtime_state() 中的初始值 */
config.strategy_type = NUMA_STRATEGY_CONFIG_LOCAL_FIRST;
config.balance_threshold = 0.3;
config.auto_rebalance = 1;
config.rebalance_interval_us = 5000000;  /* 5 秒 */

/* server.c 启动流程中覆盖为实际生效的默认值 */
numa_config_set_strategy(NUMA_STRATEGY_CONFIG_WEIGHTED_INTERLEAVE);
```

## 4. 质量与性能特性（Quality & Performance Characteristics）

- **热路径无锁化**：7/9 策略在选节点时完全不持锁（仅 `WEIGHTED` 持一次短锁复制
  权重数组，锁外计算），代价是权重更新（慢速路径）与实际生效之间存在至多 1 秒
  的滞后——这是刻意的设计取舍：用"权重稍微过期"换"热路径零锁竞争"。
- **确定性优先于精确性**：压力权重允许读到过渡值（见 3.3 节安全性论证），因为
  错误后果只是一次非最优分配，而不是正确性问题。
- **策略切换代价**：`numa_config_set_strategy()` 持全局锁，属于管理操作，不在
  性能敏感路径上，切换频率预期极低。

## 5. 与其他模块的关系（Relations to Other Modules）

- **`zmalloc.c`**：每次分配调用 `numa_config_get_best_node(size)` 决定目标节点
  ——这是本模块存在的最直接原因。
- **`server.c`**：启动时调用 `numa_config_strategy_init()`，并将默认策略覆盖为
  `WEIGHTED_INTERLEAVE`；`serverCron` 每秒调用一次 `numa_config_update_pressure_weights()`。
- **`numa_bw_monitor.c`**：`PRESSURE_AWARE` 和 `WEIGHTED_INTERLEAVE` 的权重更新都
  通过 `numaGetNodePressure()`（`evict_numa.c` 里的一个薄封装）读取同一个
  `numa_bw_get_node_pressure()`——这也是 `evict_numa` 降级评分用的压力信号，两个
  模块共享同一份数据，不会对"这个节点有多忙"给出互相矛盾的答案（此前两者各自
  维护一套独立公式，已在 ADR-08 一并合并，见 [`numa_bw_monitor.md`](numa_bw_monitor.md)）。
- **NUMAflow 子系统**（`numaflow/`）：`ADAPTIVE` 与 `LATENCY_AWARE` 在**本模块
  内**只是 fallback 到 node 0 的占位实现，**真正可用的完整版本不在这里**，而是
  NUMAflow 的两个原子操作——`alloc_adaptive`（DRAM 优先，压力超过阈值后溢出到压
  力最小的节点）与 `alloc_latency_aware`（选择建模访问代价最低的节点），定义在
  `numaflow/src/nf_ops.c`（`op_alloc_adaptive`/`op_alloc_latency_aware`），通过
  `NUMA FLOW LOAD/RUN` 以工作流形式桥接进 Redis（`src/numa_flow.c`）。这是本项
  目一个刻意的分层设计（[ADR-05](../09-architecture-decisions.md)）：内核里的策
  略保持简单可预测，复杂的自适应逻辑放在独立子系统里迭代，不直接耦合进
  `zmalloc` 的关键路径。ADR-08 之后这一分层本身没有变化，但代码现在会**自己说
  出来**：`numa_config_set_strategy()` 切到这两个策略时打一条 `LL_WARNING`
  （见 `src/numa_configurable_strategy.c`），`NUMA CONFIG GET` 的回复也新增了
  `strategy_note` 字段，同样的话在两处都写明"这是占位，完整实现在 NUMAflow"——
  过去这个分层只写在 arc42 文档里，现在运行时也会主动告知。详见
  [NUMAflow 子系统文档](../../numaflow/README.md)。

## 6. 未解决问题与已知限制（Open Issues & Known Limitations）

- **`ADAPTIVE`/`LATENCY_AWARE` 在内核中不是完整实现**——如果只看
  `numa_configurable_strategy.c`，这两种策略实际效果等同于 `LOCAL_FIRST`
  （fallback 到 node 0）。此前任何只读内核代码、不知道 NUMAflow 桥接存在的人都
  可能误以为这两种策略"坏了"或"未完成"；ADR-08 之后这个风险已经部分缓解——切
  到这两种策略会打一条 `LL_WARNING`，`NUMA CONFIG GET` 的 `strategy_note` 字段
  也会说明"占位，完整实现见 NUMAflow"，但本模块本身仍不打算在内核里补完它们，
  这是设计决策而非待办事项（见第 5 节、`docs/README.md` 的事实核对表）。
- **压力权重最长 1 秒滞后**：`WEIGHTED_INTERLEAVE` 的权重更新在 `serverCron` 里
  按秒粒度进行，突发的压力变化在被下一次 `serverCron` 采样之前不会反映到分配决
  策上。
- **`enabled_nodes_mask` 限制为 64 位**：超过 64 个 NUMA 节点的拓扑无法完整表
  示，目前没有实际硬件场景触发这个限制，但值得记录。
