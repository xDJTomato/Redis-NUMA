# 4. 解决方案总览（Solution Strategy）

> arc42 第 4 章：不重复第 5 章「构建块视图」的实现细节，只回答一个问题——
> **面对第 1 章的目标，本项目在最高层面选择了怎样的技术路线，为什么这条路线能
> 达成那些目标。** 每个模块的完整设计见 [`modules/`](modules/) 下的对应文档。

## 4.1 核心思路：一句话概括

标准 Redis 的分配器（jemalloc/libc malloc）和淘汰逻辑完全不知道 NUMA 拓扑，也
不追踪单个 key 的访问热度。本项目的解法是在不改变 Redis 对外 API 的前提下，把
"内存在哪个 NUMA 节点"这件事变成一等公民：**分配时按策略选节点，运行时按热度
迁移节点，淘汰前先尝试降级而不是直接丢弃**。CXL 内存不被当成一种全新的概念，而
是被当成"又慢又大的一个 NUMA 节点"接入同一套机制。

## 4.2 技术路线与目标的对应关系

| 目标（第 1 章） | 采用的策略 | 对应模块/文档 |
|---|---|---|
| 保留 100% Redis API 兼容性 | 只在 `zmalloc`/`server.c`/`evict.c` 三个接触点插入钩子，不改变任何命令的对外行为 | [`modules/zmalloc_numa.md`](modules/zmalloc_numa.md) |
| 新分配的数据尽量落在低延迟节点 | `numa_pool` 自定义分配器 + `numa_configurable_strategy` 的 7 种独立分配行为（默认 `weighted_interleave`：压力权重交错，无锁读路径；`WEIGHTED`/`WEIGHTED_INTERLEAVE` 共享同一个 `select_weighted_node()` 实现，仅权重来源不同） | [`modules/numa_pool.md`](modules/numa_pool.md)、[`modules/numa_configurable_strategy.md`](modules/numa_configurable_strategy.md) |
| 已分配的热数据能被发现并搬到快节点 | 16 字节 PREFIX 内联热度追踪（O(1) 访问，无需额外字典查询）+ 阶梯式惰性衰减（不为每个 key 单独跑衰减定时器）；`numa_key_migrate_touch()` 无条件写入这个信号，是 NUMAflow `enumerate()` 读取的唯一真实来源 | [`modules/zmalloc_numa.md`](modules/zmalloc_numa.md) |
| 迁移决策可插拔、可替换、可关闭 | 三个迁移策略（`caat`/`composite_lru`/`tinylfu`，外加 `noop`）统一收敛为 NUMAflow 的原子操作 DAG 预设，通过 `NUMA FLOW DEFAULT <name>` 在内核默认路径上切换（见 ADR-08） | [`09-architecture-decisions.md`](09-architecture-decisions.md) |
| 迁移决策既能兼顾"够快"又能"不遗漏冷门热点" | Composite LRU 预设的双通道设计：热候选环形缓冲区（快路径，毫秒级响应）+ 渐进式字典扫描（慢路径，每轮只扫一小段，不阻塞事件循环）——现在只作为 NUMAflow 的原子操作 DAG 存在，通过 `NUMA FLOW DEFAULT composite_lru` 使用 | [`docs/new/modules/numa_composite_lru.md`](modules/numa_composite_lru.md) |
| 内存吃紧时优先降级而不是直接淘汰 | `evict_numa` 在原生淘汰逻辑前插入一步：按"距离 40% + 压力 30% + 带宽 30%"加权评分选目标节点，尝试降级 | [`modules/evict_numa.md`](modules/evict_numa.md) |
| 迁移/分配决策需要感知真实的节点带宽压力，而不是只看容量 | `numa_bw_monitor` 实时按节点采集带宽（resctrl/numastat/manual 三级降级），并提供统一的 `numa_bw_get_node_pressure()`，供 `PRESSURE_AWARE`/`evict_numa` 共用同一套评分依据 | [`modules/numa_bw_monitor.md`](modules/numa_bw_monitor.md) |
| 所有 5 种 Redis 数据类型、所有内部编码都要被正确迁移，不能有"部分实现" | `numa_key_migrate` 为 STRING/HASH/LIST/SET/ZSET 的每一种内部编码（listpack/ziplist/hashtable/quicklist/skiplist/intset）都实现了对应适配器 | [`modules/numa_key_migrate.md`](modules/numa_key_migrate.md) |
| 用户/运维需要统一、可脚本化的操作入口 | 单一 `NUMA` 命令族（`MIGRATE`/`CONFIG`/`FLOW`/`HELP`），走 Redis 7 的声明式命令自省系统注册 | [`modules/numa_command.md`](modules/numa_command.md) |
| 慢策略不能拖慢 `serverCron`、拖累主事件循环尾延迟 | `numa_flow_cron()` 按 `interval_sec` 判断是否该跑 NUMAflow 工作流，单线程 `serverCron` 驱动；原先"每个策略槽位可选 `servercron`/`ae` 两种调度模式"的设计已随槽位框架一起退役（见 ADR-07/ADR-08），不再有 AE time event 变体 | [`09-architecture-decisions.md`](09-architecture-decisions.md)、[`modules/ae_strategy_scheduler.md`](modules/ae_strategy_scheduler.md)（历史设计记录） |
| 策略研究/对比不应该被"一次只能跑一个策略"限制，也不应该只能靠改内核代码试新想法 | 独立于 Redis/libnuma 的纯 C11 子系统 **NUMAflow**：把上述策略拆成 36 个可组合的原子操作，允许在不改内核的前提下用 DAG 拼出新策略，并在此基础上设计了新默认策略 **CAAT**（晋升+降级，见 4.3） | [`docs/numaflow/README.md`](../numaflow/README.md) |

## 4.3 为什么需要 NUMAflow，而不是止步于（曾经的）内核原生 Composite LRU/TinyLFU

Composite LRU 和 TinyLFU（当年内核原生实现，现已随 ADR-08 一并删除，只作为
NUMAflow 的原子操作预设保留）都是**只升不降**的策略：一旦 DRAM 写满就无法继续
晋升新的热 key，因为二者都没有"主动把 DRAM 上不再热的数据搬走，腾位置给真正的
热点"这一步。要在内核代码里补上这一步，意味着要再写一个新的、同样复杂的策略
模块，且难以在改动前用真实数据验证它是否真的更好。

NUMAflow 把这个问题拆成了两半来解：

1. **先把"策略"这件事本身原子化**——36 个可组合的原子操作（打分/过滤/排序/
   决策/执行/追踪六类），任何既有策略都能被表示成一串原子操作的 DAG，任何新想法
   也能用同一套积木拼出来，不需要改一行 Redis 内核代码；
2. **在这套积木上设计出 CAAT（Cost-Aware Adaptive Tiering）**——一条完整的
   "晋升 + 降级"流水线，核心是净收益公式
   `benefit = (cost(当前节点) - cost(目标节点)) × 访问率 - 迁移代价`，只有收益
   为正的 key 才被晋升，同时主动把 DRAM 上不再热的 key 降级到 CXL 节点，从而
   让 DRAM 始终留给"当下最值得占用它"的数据。

实测（zipf 分布 / 3000 key / 120000 次访问 / DRAM 容量约束在工作集的 50%）：CAAT
本地命中率约 91%，净代价比表现最好的既有基线（TinyLFU）低约 20%，比 Composite
LRU 低约 37%。**这组数字用四个基准工作负载复测后确认是工作负载依赖的**（见
[ADR-09](09-architecture-decisions.md) 的复测记录）：在 zipf/hotspot 这类有明显
冷热分层的负载上 CAAT 确实全面更优，与上面的数字一致；但在 uniform（访问均匀、
没有真正"冷"数据）上，`demote_cold` 主动搬走"冷"数据反而变成纯浪费开销，净代价
比 Composite LRU **高约 31%**；temporal 负载上两者基本打平。ADR-08 选 CAAT 作
默认这个决策本身不需要因此撤销（真实 Redis 场景的访问分布通常偏态、接近
zipf/hotspot），但引用"CAAT 全面更优"时应注明这是有工作负载前提的。该评测框架
的设计仍放在 [`modules/`](modules/) 之外的
[`docs/numaflow/README.md`](../numaflow/README.md) 中单独说明，因为它不是 Redis
内核的一部分。

## 4.4 为什么把 ADAPTIVE / LATENCY_AWARE 两种分配策略的"真身"放在 NUMAflow 而不是内核

`numa_configurable_strategy` 里的 9 个策略枚举值中，独立行为只有 7 种——
`ADAPTIVE` 和 `LATENCY_AWARE`
在内核里只是占位实现。这不是遗漏，而是延续 4.3 的同一个设计取舍：**内核里的分配
路径要保持简单、可预测、在 `zmalloc` 这条高频调用路径上零意外开销**；而"根据
运行时反馈自适应调整"这种复杂度本身会带来分支和状态，更适合放在独立、可以慢慢
迭代、出了问题也不影响 Redis 主进程稳定性的 NUMAflow 里（`alloc_adaptive`/
`alloc_latency_aware` 两个原子操作），通过 `NUMA FLOW` 桥接进 Redis。完整的决策
记录见 [`09-architecture-decisions.md`](09-architecture-decisions.md)。

## 4.5 一个 Key 的全生命周期（Life of a Key）

为了直观展现各模块如何协同工作，以下是单个 Redis Key 从创建、访问、调度迁移到承压降级的端到端全生命周期：

```text
┌───────────────────────────────────────────────────────────────────────────┐
│ 1. [创建与初始分配] 客户端执行 SET mykey "hello"                             │
│    ├─ zmalloc() 拦截请求                                                  │
│    ├─ numa_configurable_strategy: 根据各节点实时压力加权随机，选定 Node 0 (DRAM) │
│    └─ numa_pool: 分配内存并在头部写入 16B PREFIX (node_id=0, hotness=0)   │
└─────────────────────────────────────┬─────────────────────────────────────┘
                                      │
                                      ▼
┌───────────────────────────────────────────────────────────────────────────┐
│ 2. [高频读写与热度追踪] 客户端持续执行 GET mykey                             │
│    ├─ db.c (lookupKey): 命中该 Key                                        │
│    ├─ numa_key_migrate_touch(): O(1) 递增 PREFIX 中的 access_count 与 hotness│
│    └─ numa_flow_observe_access(): 记录至 NUMAflow 频率追踪器 (CMS/Doorkeeper)│
└─────────────────────────────────────┬─────────────────────────────────────┘
                                      │
                                      ▼
┌───────────────────────────────────────────────────────────────────────────┐
│ 3. [后台策略编排与动态迁移] serverCron 周期触发 numa_flow_cron              │
│    ├─ NUMAflow 执行 CAAT 工作流 DAG：                                     │
│    │  benefit = (远端访问代价 - 本地访问代价) × 访问频次 - 迁移代价            │
│    ├─ 若 mykey 变冷且 Node 0 承压：产生降级决策 (Node 0 DRAM -> Node 1 CXL) │
│    └─ numa_key_migrate: 分配 Node 1 空间 -> 拷贝数据 -> 原子切指针 -> 释放旧内存│
└─────────────────────────────────────┬─────────────────────────────────────┘
                                      │
                                      ▼
┌───────────────────────────────────────────────────────────────────────────┐
│ 4. [内存承压与淘汰前拦截] 系统达到 maxmemory 上限，触发 performEvictions   │
│    ├─ evict_numa (evictionTryNumaDemote): 拦截原生淘汰流程                │
│    ├─ 综合评分选目标节点：Score = 距离(40%) + 压力(30%) + 带宽(30%)         │
│    └─ 若存在更充裕的 Node 2 (CXL)：优先迁往 Node 2 降级，避免直接丢弃数据      │
└───────────────────────────────────────────────────────────────────────────┘
```

## 4.6 小结：分层决策，而不是单点优化

把上述机制连起来看，本项目的解决方案是一条层次分明、各司其职的完整决策链：

```
新分配 → numa_configurable_strategy（选节点）
            ↓
已分配的数据 → PREFIX 热度追踪（惰性衰减）
            ↓
numa_flow_cron 定时检查 → NUMAflow 默认工作流：CAAT / Composite LRU / TinyLFU（决定迁移谁、迁去哪）
            ↓
内存吃紧时 → evict_numa（先降级，降级不了才真淘汰）
            ↓
统一操作入口 → NUMA 命令族 / NUMA FLOW
```

每一层都可以独立替换或关闭（`numa-enabled no` 只保留分配优化、槽位可插拔、
NUMAflow 是完全独立的子系统），这正是为什么第 9 章的多条决策记录都在反复回答
同一个问题："这一层的复杂度，值得放进内核，还是应该留在可替换的外围？"
