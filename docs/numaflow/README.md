# NUMAflow — N8N 风格的 NUMA 内存调度策略引擎

> 本模块是本次毕业设计的新增核心：把已有的缓存调度策略拆分为一组**可像 N8N 工作流
> 一样被流程化执行的原子操作**，并提供一个更优的默认策略、一个在 QEMU 不可用环境下
> 的公平评测框架、TUI/GUI 配置界面，以及一个轻量级缓存行为追踪反馈框架。
> 全部核心实现为**纯 C11**，无任何 Redis / libnuma / POSIX 依赖，在 16 GB 的 Windows
> 笔记本上即可编译、测试与评测。

## 目录

```text
numaflow/
├── include/           # 公开头文件（纯 C11）
│   ├── nf_common.h    # 公共类型 / 确定性 PRNG / 日志
│   ├── numa_shim.h    # 可移植 libnuma 仿真环境（自建 libnuma 环境）
│   ├── nf_json.h      # 极简 JSON 解析/序列化
│   ├── nf_graph.h     # DAG 图模型
│   ├── nf_ops.h       # 原子操作注册表 + 执行上下文
│   ├── nf_exec.h      # 拓扑排序数据流执行器
│   ├── nf_track.h     # 轻量缓存行为追踪框架（CMS + Doorkeeper + 反馈）
│   ├── nf_strategy.h  # 策略目录（现有策略的 DAG 分解 + 新策略）
│   └── nf_bench.h     # 公平评测框架
├── src/               # 实现（对应上述头文件）+ nf_cli.c 命令行
├── tui/nf_tui.c       # 交互式文本界面（C11 + ANSI/VT）
├── gui/               # N8N 风格 Web 编辑器（index.html）+ 后端桥接（server.py）
├── eval/report.py     # 结果可视化（纯 stdlib Python，生成 SVG/HTML 报告）
├── tests/             # 单元 + 集成测试（test_all.c / test_smoke.c）
└── Makefile           # 兼容 GNU make / mingw32-make
```

## 1. 自建 libnuma 环境（无 libnuma / 无异构内存）

真实 libnuma 只能在 Linux 上链接。本模块在 `numa_shim.c` 中用纯 C11 在普通 `malloc`
之上仿真了 Redis-NUMA 用到的 libnuma API 子集：

- 把「NUMA 节点」建模为可配置拓扑（容量 / 延迟 / 带宽 / 距离 / 压力），默认提供
  `dram0 + cxl1 + ...` 的分层模板；
- `nf_numa_alloc_onnode` / `nf_numa_free` / `nf_numa_node_of_addr` 通过一个分配注册表
  保证「节点归属」可查询；
- `nf_numa_access_cost` / `nf_numa_migrate_cost` 是**解析式访问代价模型**——
  本地访问只付延迟，远端访问按 NUMA 距离放大延迟并按带宽折算传输时间。

这样，任何机器（包括本 16 GB Windows 机）都能以**确定性**的方式建模异构内存的
快/慢分层，从而公平地评测调度策略。

## 2. 策略的原子化拆解

`nf_ops.c` 注册了 **36 个原子操作**，按类别划分，并逐一对应到已有策略：

| 类别 | 原子操作 | 对应已有策略语义 |
| --- | --- | --- |
| alloc | `alloc_local_first` / `alloc_interleave` / `alloc_round_robin` / `alloc_weighted` / `alloc_pressure_aware` / `alloc_cxl_optimized` / `alloc_weighted_interleave` / `alloc_adaptive` / `alloc_latency_aware` | 9 种分配策略 |
| score | `score_hotness` / `decay_hotness` / `cms_observe` / `cms_estimate` / `global_decay` / `score_ewma` / `score_cost_benefit` | Composite LRU 阶梯热度 / TinyLFU CMS+Doorkeeper |
| filter | `filter_hot` / `filter_freq` / `filter_cold` / `filter_remote` / `filter_local` / `filter_size_min` / `filter_size_max` / `filter_benefit` | 迁移候选筛选 |
| rank | `rank_lru` / `rank_frequency` / `rank_hotness` / `rank_cost` / `rank_ewma` / `rank_size` | 候选排序 |
| decide | `select_dest_node` / `budget_limit` | 目标节点选择 / 迁移预算 |
| emit | `emit_migrate` / `demote_cold` / `balance_nodes` | 迁移执行 / 冷数据降级 / 再平衡 |
| track | `track_access` | 访问追踪 |

### 2.1 数据从哪来：`nf_item_t` 各字段的真实来源

四个迁移预设都读写同一个 `nf_item_t`（`include/nf_common.h`），字段语义和赋值方式
决定了每个预设实际"看到"什么信号。在 Redis 桥接（`src/numa_flow.c` 的
`numa_flow_enumerate()`）里，这些字段并不是凭空计算的合成值，而是分别接到两条完全
独立的真实数据管道：

| 字段 | 谁写入 | 写入时机 |
| --- | --- | --- |
| `current_node` | `numa_get_node_id(sample)` | 每次枚举时读 zmalloc 分配前缀 |
| `access_count` / `recency` / `hotness` | zmalloc 分配前缀（`numa_alloc_prefix_t`） | `numa_key_migrate_touch()`——**每次真实的"触达"访问**都无条件更新，不管当前有没有迁移策略在跑（见 `src/db.c`） |
| `freq_est` | NUMAflow 自己的 CMS + Doorkeeper 追踪器（`nf_track.c`），只在 `cms_estimate` 算子里按需查询 | `numa_flow_observe_access()`——与上面**同一处**真实访问路径调用，喂给 CMS，和 zmalloc 前缀的热度计数是两套完全独立的统计（ADR-09 修复前，这条线路完全没有人调用，`freq_est` 永远是 0，TinyLFU/CAAT 因此从不迁移任何数据） |
| `cost_benefit` | `op_score_cost_benefit`（只有 CAAT 用它） | 每次 DAG 执行时按当前 `access_count`/`freq_est` 现算，不持久化 |

也就是说：Composite LRU（用 `hotness`）和 TinyLFU（用 `freq_est`）看的是**两套互不
相通的热度信号**，即使面对完全一样的访问模式，两者对"这个 key 有多热"的判断也
不会逐 bit 一致——这不是 bug，是刻意保留下来的、对应它们各自原始设计（阶梯式 LRU
衰减 vs. Count-Min Sketch 频率估计）的实现差异。

### 2.2 `noop`——空图基线

```c
static int build_noop(nf_graph_t *g) { (void)g; return NF_OK; }
```

不添加任何节点/边。`nf_exec_run()` 对空图的行为是"什么都不做，原样返回输入"，
所以 `noop` 下 `migrations` 恒为 0——这不是"总是不满足迁移条件"，是图里根本没有
任何算子会检查条件。作为吞吐/延迟对比的对照组，以及验证"NUMA 分配层单独的开销"
时的基线。

### 2.3 Composite LRU——阶梯式热度衰减，只晋升不降级

```text
score_hotness → filter_hot(threshold=5) → rank_hotness → budget_limit(budget=512)
              → select_dest_node → emit_migrate
```

逐步展开：

1. **`score_hotness`**：`hotness = min(access_count, 7) - staircase_decay(idle)`，
   `idle = ctx.tick - recency`。`staircase_decay()` 是四级阶梯：空闲 <10s 不衰减，
   [10s,60s) 衰减 1，[60s,300s) 衰减 2，[300s,1800s) 衰减 3，≥1800s（30 分钟）
   直接清零。这是"阶梯式惰性衰减"——不需要后台定时任务逐 key 扫描衰减，只在
   每次真正被打分的时候按空闲时长一次性算出当前该衰减多少。
2. **`filter_hot(threshold=5)`**：只留下 `hotness >= 5` 的候选——`access_count`
   上限是 7，所以实际能通过这个门槛的，是最近访问过、访问次数够多、且没有空闲
   太久的 key。
3. **`rank_hotness`**：按 `hotness` 降序稳定排序。
4. **`budget_limit(budget=512)`**：只保留排名前 512 个候选，其余计入
   `ctx.stats.migrations_skipped`（不是真的被拒绝迁移，是"这一轮预算不够，下一轮
   再评估"——因为热度信号是持久化在 zmalloc 前缀里的，没通过预算的候选不会丢失
   状态）。
5. **`select_dest_node`**：对每个候选调用 `nf_best_node()`——遍历所有节点，选
   `nf_numa_access_cost() × (1 + 2×pressure)` 最小、且容量够放的那个；
   `migrate = (dst != current_node)`，**不检查 `cost_benefit` 的正负**（Composite
   LRU 没有设 `require_benefit`）——只要不在最优节点上，且通过了热度门槛，就迁移。
6. **`emit_migrate`**：真正执行——目标节点容量不够就跳过（计入
   `migrations_skipped`），够就记账（`nf_numa_account_free`/`_alloc`）、累加
   `total_cost_ns`、把 `current_node` 改成 `selected_node`。

**结构性限制**：整条链里没有任何算子会把已经在 DRAM 上的 key 移出去——一个
key 一旦被判定为"不够热"，**不会**被这条链主动降级，只是单纯不再被选中；它
会一直占着 DRAM 直到某个更好的候选把预算用满、或者它自己的 `hotness` 掉到 5
以下又刚好被别的路径处理掉。这也是 composite_lru 和 tinylfu 在 ADR-11 的真实
双节点测试里 `cold_off_ratio` 提升不了的根本原因：预设本身就没有降级机制，不是
参数没调好。

### 2.4 TinyLFU——Count-Min Sketch 频率估计，同样只晋升不降级

```text
cms_estimate → filter_freq(threshold=2) → rank_frequency → budget_limit(budget=512)
             → select_dest_node → emit_migrate
```

结构和 Composite LRU 几乎镜像，只是打分方式换成频率估计而不是阶梯热度：

1. **`cms_estimate`**：向 `nf_track.c` 的追踪器查询这个 key 的频率估计
   （`nf_tracker_freq()`——4 行 Count-Min Sketch，每行用不同的哈希种子映射到
   4096 个 4-bit 计数器格子，取 4 行里的**最小值**作为估计，这是 CMS 用多行取
   min 抑制哈希碰撞高估的标准做法）。**关键细节：CMS 写入受 Doorkeeper 布隆
   过滤器把关**——`nf_tracker_observe()` 每次访问先查两位布隆过滤器，第一次
   见到某个 key 只在布隆过滤器里打标记、**不**增加 CMS 计数；只有第二次及以后
   才真正 `cms_inc()`。这意味着**只被访问过一次的 key，`freq_est` 永远是 0**，
   不管它的 zmalloc 前缀 `access_count` 是多少。
2. **`filter_freq(threshold=2)`**：只留下 `freq_est >= 2` 的候选——按上面的
   Doorkeeper 语义，这实际上要求"被真正计入 CMS 的访问≥2 次"，即"至少被访问
   过 3 次"（第 1 次被布隆过滤器吃掉，第 2、3 次才让 CMS 从 0 涨到 2）。
3. **`rank_frequency`**：按 `freq_est` 降序排序。
4. **`budget_limit(budget=512)` → `select_dest_node` → `emit_migrate`**：和
   Composite LRU 完全同一套实现（同一批算子函数），差异只在上游打分/过滤用的
   字段。

**全局衰减**：`nf_tracker_t.reset_interval`（Redis 桥接里固定为 100000 次观测）
到点后触发 `nf_tracker_decay()`——把全部 CMS 计数器右移一位（相当于减半）、清空
Doorkeeper 布隆过滤器。观测量不到这个阈值时（例如一次性小规模基准测试），衰减
完全不会触发，`freq_est` 只会单调上升，不会因为"过了一段时间没访问"就自动降低——
这与 Composite LRU 的 `hotness`（会随空闲时间主动衰减）是两种不同的"冷却"哲学：
TinyLFU 的冷却只跟"总访问量"挂钩，不跟"墙钟时间"挂钩。

同样**没有降级机制**——`caat`/`tinylfu`/`composite_lru` 三者里，只有 CAAT 会主动
把 DRAM 上的 key 挪走。

### 2.5 CAAT（Cost-Aware Adaptive Tiering）——唯一同时晋升 + 降级的预设

```text
                     ┌── cms_estimate → score_cost_benefit ──┐
                     │         （对全部候选统一打分一次）        │
                     └──────────────────┬─────────────────────┘
                                        │  按当前驻留位置分叉
                    ┌───────────────────┴───────────────────┐
                    ▼                                       ▼
        filter_local(node=0)                     filter_remote(node=0)
        （DRAM 驻留项：降级子链）                  （非 DRAM 驻留项：晋升子链）
                    │                                       │
        demote_cold(threshold=1)              filter_freq(threshold=1)
                    │                                       │
             emit_migrate                       filter_benefit(threshold=0)
        （终止节点 A，唯一一次变更）                          │
                                                        rank_cost
                                                             │
                                              budget_limit(budget=512)
                                                             │
                                        select_dest_node(require_benefit=1)
                                                             │
                                                       emit_migrate
                                              （终止节点 B，唯一一次变更）
```

按分叉前后拆开看：

**打分阶段（分叉前，对全部候选统一执行一次）**：
- `cms_estimate`：和 TinyLFU 用同一个算子，读同一个 Doorkeeper+CMS 追踪器，
  语义完全一致（含"只访问过一次算 0"的门槛）。
- `score_cost_benefit`：先用 `nf_best_node()` 算出这个 key 理论上的最优落点
  `dst`（遍历所有节点选 `access_cost × (1+2×pressure)` 最小且容量够放的一个），
  再用 `nf_benefit()` 算净收益：
  ```text
  rate   = freq_est（若 CMS 有估计）否则 log2(1 + access_count)
  gain   = (access_cost(当前节点) - access_cost(目标节点)) × rate
  cost   = migrate_cost(当前节点 → 目标节点)     // 一次性搬迁代价：固定 1000ns
                                                  // + 按源/目节点带宽折算的
                                                  // 读出+写入传输时间
  benefit = gain - cost
  ```
  `benefit` 写入 `it.cost_benefit`，供后面两条子链使用。**注意**：`rate` 优先取
  `freq_est`，只有 CMS 没有估计（`freq_est==0`）时才退化到 `access_count` 的对数——
  这意味着"只访问过一次"的 key 在这里的 `rate` 不是 0，是 `log2(2)=1`，和上面
  `filter_freq`/`demote_cold` 用的"纯 `freq_est`"门槛不是一回事，容易被误读成同一
  套信号。

  ##### 算例说明（CAAT 晋升净收益具体计算）：
  假设某 Key（大小 1KB）当前驻留在 Node 1 (CXL 内存，访问延迟 250ns)，目标晋升节点为 Node 0 (DRAM，访问延迟 80ns)。
  - **访问频次**：该 Key 处于高频活跃状态，CMS 估算访问率 `freq_est = 20`。
  - **访问收益（Gain）**：
    $$\text{Gain} = (\text{Cost}_{\text{CXL}} - \text{Cost}_{\text{DRAM}}) \times \text{rate} = (250\text{ns} - 80\text{ns}) \times 20 = 170\text{ns} \times 20 = \mathbf{3400\text{ns}}$$
  - **一次性迁移代价（Cost）**：
    $$\text{Cost} = \text{BaseCost} + \text{TransferCost} = 1000\text{ns} + 100\text{ns} = \mathbf{1100\text{ns}}$$
  - **净收益（Benefit）**：
    $$\text{Benefit} = \text{Gain} - \text{Cost} = 3400\text{ns} - 1100\text{ns} = +\mathbf{2300\text{ns}} > 0$$
  - **决策**：净收益为正（$+2300\text{ns}$），通过 `filter_benefit(threshold=0)` 门槛，准予加入晋升队列；若另一低频 Key 仅被访问 2 次（$\text{Gain} = 340\text{ns} < 1100\text{ns}$，$\text{Benefit} = -760\text{ns}$），则被直接拒绝晋升，从而避免盲目迁移打满总线带宽。

**分叉**（`filter_local`/`filter_remote` 按 `current_node==0` 与否二分）：这一步
是 ADR-09 修的关键点——旧版单链设计把降级的 `emit_migrate` 直接接到晋升阶段的
过滤器上，一个刚被降级、但没通过晋升侧 `filter_freq`/`filter_benefit` 门槛的 key
就会从图的终止节点输出里彻底消失，桥接层的"结果 vs 入队原始状态"diff 永远看不到
它，`apply()`（真正执行迁移）不会被调用——**它的 `current_node` 在内存里已经改了，
但 Redis 侧从未真的把数据搬过去**。现在的写法保证每个 item 只经过其中一条子链、
只被变更一次、必然到达唯一一个属于自己的终止节点。

**降级子链**（DRAM 驻留项）：`demote_cold(threshold=1)`——**只看 `freq_est`，不看
`cost_benefit`**：`current_node==dram_node && freq_est < 1` 就标记降级到 CXL。
threshold 默认是 1，即 `freq_est==0`（前面提到的"只访问过一次"或"从未真正被
CMS 计数过"）就会被判定为该降级——这是一个**纯频率门槛**，跟这个 key 之前打的
`cost_benefit` 分数完全无关（分数算出来了，但降级子链根本没读它）。这解释了本次
会话在真实 VM 上观测到的现象：CAAT 会把"只在建库时 `SET` 过一次、从未被真正
`GET` 过"的 key 全部挪出 DRAM——这是它按设计应该做的事，只是当 DRAM 容量本来就
绰绰有余时，这些迁移不会换回任何访问收益，纯粹是迁移代价的净支出（同一批 key
换到 Composite LRU/TinyLFU 上则会因为压根没有降级机制而原地不动，若这些 key 恰好
最初就分配在本地节点，反而会让 `local_hit_ratio` 这个只看"访问是否命中本地"的
指标显得更高——这不代表它们的放置质量更好，只是分母里混进了大量几乎不产生任何
真实访问权重的冷 key）。

**晋升子链**（非 DRAM 驻留项）：`filter_freq(threshold=1)` 先过一道频率下限（比
Composite LRU/TinyLFU 的默认阈值更松），`filter_benefit(threshold=0)` 再过一道
净收益必须为正，`rank_cost` 按 `cost_benefit` 降序排序，`budget_limit(budget=512)`
截断，`select_dest_node(require_benefit=1)` 重新确认目标节点和收益为正（双重
保险——`filter_benefit` 已经筛过一次，这里 `require_benefit=1` 保证即使排序/截断
之间数据有出入也不会晋升净收益为负的候选）。

**与 Composite LRU/TinyLFU 的核心差异不是"更聪明的打分公式"，是拓扑本身**：
后两者的图里根本没有任何路径能把 `current_node` 从 DRAM 改成别的值；CAAT 的图
显式分出了一条独立的降级子链。这是"晋升+降级"这句话在实现层面唯一的含义，也是
[ADR-04](../new/09-architecture-decisions.md)/[ADR-11](../new/09-architecture-decisions.md)
反复强调"CAAT 是三者中唯一同时执行 promote 和 demote 的策略"这句话的具体出处。

### 2.6 九种分配预设（`alloc_*`，决定新数据落在哪个节点，不涉及迁移）

这 9 个原子操作对应的是 `numa_configurable_strategy.c` 里 zmalloc 层的分配策略
（见 [ADR-08](../new/09-architecture-decisions.md)：内核里仍有 7 种独立行为，
`WEIGHTED`/`WEIGHTED_INTERLEAVE` 共享同一份实现，`ADAPTIVE`/`LATENCY_AWARE` 是
占位）。NUMAflow 里把全部 9 个都实现成了真正独立的原子操作，可以在 DAG 里替换
内核占位的那两个：

| 预设 | 决策规则 |
| --- | --- |
| `alloc_local_first` | 固定分配到参数指定的节点（默认节点 0） |
| `alloc_interleave` | 每次请求独立均匀随机选节点 |
| `alloc_round_robin` | 按请求序号对节点数取模，逐个轮询 |
| `alloc_weighted` | 按各节点静态 `weight` 加权随机（权重和归一化后按累积区间落点选择） |
| `alloc_pressure_aware` | 选当前 `pressure` 最低的节点 |
| `alloc_cxl_optimized` | 按 value 大小分层：小于 `min_size`（默认 1024B）进节点 0，其余进节点 1 |
| `alloc_weighted_interleave` | 加权随机，但权重 = `静态 weight × (1 - pressure)`，与 `alloc_weighted` 是同一套加权随机循环，只是权重来源多乘了一个压力因子 |
| `alloc_adaptive` | 节点 0 压力 < 阈值（默认 0.8）就直接选节点 0，否则退化成 `pressure_aware`——这是内核里 `ADAPTIVE` 占位背后**真正**的实现 |
| `alloc_latency_aware` | 对每个候选节点调用 `nf_best_node()`（与 CAAT 打分用的同一个函数）选建模访问代价最低的节点——这是内核里 `LATENCY_AWARE` 占位背后**真正**的实现 |



## 3. 公平评测框架（QEMU 不可用）

`nf_bench.c` 在仿真拓扑上复现同一条访问轨迹，保证公平性：

1. **相同拓扑**：DRAM 容量被约束为工作集约 50%，逼真地迫使策略做「取舍」而非全量晋升；
2. **相同工作负载**：`zipf` / `uniform` / `hotspot` / `temporal` 四种合成负载，同一种子；
3. **相同轨迹**：先确定性生成访问序列，每个策略重放同一条序列；
4. **相同预算与容量**：迁移预算、节点容量在策略间一致，`emit_migrate` 强制容量上限；
5. **确定性执行**：xorshift64* 随机源，单线程，结果可比特级复现。

指标：本地命中率、访问代价、迁移次数、迁移代价、净代价（访问 + 迁移）与追踪反馈分。

```bash
cd numaflow && make && make report   # 生成 results/report.html
```

实测（zipf / 3000 keys / 120000 访问 / DRAM 50% 容量）：

| 策略 | 本地命中率 | 净代价 | 迁移次数 |
| --- | --- | --- | --- |
| Baseline (noop) | 0.0% | 305.0M | 0 |
| Composite LRU | 81.2% | 134.5M | 2187 |
| TinyLFU | 79.3% | 105.9M | 832 |
| **CAAT（新）** | **91.1%** | **84.9M** | 2422 |

CAAT 在净代价上比最好的基线（TinyLFU）降低约 **20%**，比 Composite LRU 降低约 **37%**。

> **已复测：zipf 上的这组数字基本经得起验证，但"CAAT 全面更优"不是无条件结论**。
> `nf_bench.c`（本表数字的来源）和 `build_caat` 一样，同样从 `nf_exec_run()` 只读
> 终止节点输出并集的 `ex.result`，所以 [ADR-09](../new/09-architecture-decisions.md)
> 描述的那个拓扑 bug 也会让 `nf_bench.c` 把已执行但没通过晋升过滤的降级丢出统计。
> 这个 bug 对 CAAT 自身的绝对数字影响是真实且不小的——用仓库固定基准参数
> （3000 key / 120000 访问 / seed 20240517）修复前后对比，zipf 命中率从 84.6% 涨到
> 91.0%、净代价降低约 54%，其他三个标准工作负载（uniform/hotspot/temporal）同样
> 有 50%+ 量级的净代价改善，不是噪声。修复后重新按 ADR-04 的 zipf 口径测，命中率
> 91.03%、净代价比 composite_lru 低 36.9%、比 tinylfu 低 19.9%，和上表原始数字
> 一致。但换成 uniform（无冷热分层的均匀访问）重跑同一组对比，CAAT 净代价反而比
> composite_lru **高 31.1%**（`demote_cold` 在没有真正冷数据时纯粹是浪费的迁移
> 开销）；temporal 上两者基本打平（CAAT 净代价高 3.3%）。结论：上表的具体数字在
> zipf/hotspot 这类有明显冷热分层的负载下是可信的，但把"CAAT 全面更优"当作不区分
> 工作负载的结论去引用是不对的——它在访问接近均匀分布时反而更差。详见 ADR-09
> 的"遗留事项"小节。

### 3.1 相对性能基准：真实放置轨迹 + 标定代价模型（ADR-12）

`nf_bench.c` 的公平评测复现的是**合成**访问轨迹（zipf/uniform/hotspot/temporal
四种，内部生成）。用户如果手上没有真实的多路 NUMA/CXL 硬件（近期常见情况），
想知道"某个策略在真实工作负载下的相对表现"，`nf_bench.c` 这条路给不出——它测的
是策略在合成负载上的相对差异，不是真实系统里实际发生的放置决策。

`numaflow replay --trace <name>=<file.json> ...` 补上这一环：读入一份或多份
**真实**放置轨迹（每条记录 `{key,size,access_count,origin_node,final_node}`，
通常来自 `tests/vm/collect_relative_trace.sh` 在真实双 NUMA 节点 QEMU guest 上
的采集结果），对每条记录调用与 `eval` 完全相同的纯函数代价模型
（`nf_numa_access_cost`/`nf_numa_migrate_cost`，`numa_shim.c`），支持同一套
`--cxl-latency-ns`/`--cxl-bandwidth-mbps` 标定，输出和 `eval` 的
`bench_<workload>.json` 里 `migration` 数组同构的 JSON——`eval/report.py` 不用
改一行代码就能多画一张对比面板。

```bash
./build/numaflow replay \
    --trace noop=trace_noop.json --trace caat=trace_caat.json \
    --trace composite_lru=trace_composite_lru.json --trace tinylfu=trace_tinylfu.json \
    --nodes 2 --cxl-latency-ns 125 --cxl-bandwidth-mbps 25000 \
    --out results/bench_relative_perf_cxlcal.json
```

配套的 `tests/vm/relative_perf_bench.sh`（开发机上运行）编排整条链路：起停
guest 内四个策略、取回四份轨迹、跑两次 `replay`（标定/不标定各一次）。**这是
建模投影，不是实测延迟**——标定常数本身来自一次 CXLMemSim 简化设备模型的检查，
不是硅片实测；详见 ADR-12（`docs/new/09-architecture-decisions.md`）。

## 4. TUI / GUI 与定时任务

- **TUI**（`make` 后运行 `./build/nf_tui`）：列出原子操作/策略、以原子操作组合自定义
  工作流、保存/加载 JSON、运行评测、创建**周期性的内存调度任务**并逐 tick 打印追踪
  反馈分。
- **GUI**（`python gui/server.py` 后打开 http://127.0.0.1:8090）：N8N 风格可视化编辑器，
  拖拽原子操作节点、连线成 DAG、编辑参数、导入/导出/运行工作流；后端通过 HTTP 调用
  编译好的 C11 `numaflow` 二进制执行。

## 5. 轻量缓存行为追踪框架

`nf_track.c` 用固定内存实现：

- **Count-Min Sketch**（4 行、4-bit 计数器）+ **Doorkeeper 布隆过滤器**（TinyLFU 同款），
  O(1) 发现热 key；
- **全局衰减**（计数器减半）；
- **滑动窗口反馈**：EWMA 命中率 + 归一化代价 → 单一 `feedback_score`，供自适应策略
  在线调参（`nf_tracker_update_feedback`）。

## 6. 构建与测试

```bash
cd numaflow
make            # 构建 numaflow CLI + TUI（GNU make 或 mingw32-make 均可）
make test       # 编译并运行单元 + 集成测试
make report     # 生成评测 JSON + results/report.html
./build/numaflow ops        # 列出 36 个原子操作
./build/numaflow strategies # 列出 13 个内置策略
./build/numaflow eval --workload zipf --cxl-latency-ns 125 --cxl-bandwidth-mbps 25000
                             # 合成轨迹公平评测，可选标定（见 3 节）
./build/numaflow replay --trace caat=trace_caat.json [--trace ...] \
                         --cxl-latency-ns 125 --cxl-bandwidth-mbps 25000
                             # 真实放置轨迹 + 标定代价模型（见 3.1 节，ADR-12）
```

在 Linux + 真实 libnuma 环境下，本子系统同样可编译运行（Makefile 自动选择后缀）；
它也是 Redis 7 里三个迁移策略唯一的实现载体，通过 `NUMA` 命令调用（见
`src/numa_flow.c` 与 `NUMA FLOW DEFAULT/LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT`），
默认策略在启动时自动加载，不需要显式 `NUMA FLOW LOAD`。
## 6.5 独立内存分配器（`nf_alloc`）

针对原 `numa_pool`（libnuma 临时方案）的性能与碎片率优化，详见
[allocator.md](allocator.md)：无 per-object header（metamap 反查）+ free-list + tcache，
单线程吞吐约为系统 malloc 的 **1.85×**，内部碎片 3.82%，并去掉 16B PREFIX 开销。

## 6.8 新手模板库

提供 **23 个开箱即用的模板**，按 5 类分组（tiering / allocation / cost / adaptive /
special），每个带用途与适用场景，CLI `numaflow templates` 或 GUI 下拉框即可选用。详见
[templates.md](templates.md)。

## 7. Redis 桥接适配器（`NUMA FLOW` 命令）

`numaflow/src/nf_bridge.c`（纯 C11，可独立测试）定义了引擎与任意键值存储之间的契约：
宿主只需实现两个回调——`enumerate`（把 keyspace 逐条产出为 `nf_item_t`）和 `apply`
（把某个 key 物理迁移到目标节点）。`src/numa_flow.c` 就是 Redis 侧的薄胶水，且是
三个迁移策略（`caat`/`composite_lru`/`tinylfu`，外加 `noop`）**唯一**的实现载体——
内核不再有任何原生实现。启动时（`numa_flow_init()` 之后，且 `numa-enabled` 不为
`no` 时）会自动用 `numa-flow-default-strategy`（默认 `caat`）构建并加载为
`default` 工作流条目，随 `serverCron` 一起跑——不再需要手动 `NUMA FLOW LOAD` 才能
获得迁移行为：

```text
NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>         # 运行时切换默认策略
NUMA FLOW LOAD <name> <path.json> [interval_sec] [ADAPT]   # 加载 GUI 导出的工作流
NUMA FLOW RUN  [name]                                     # 立即执行（或全部）
NUMA FLOW LIST / STATUS <name>                            # 查看运行状态/反馈
NUMA FLOW UNLOAD <name>                                   # 卸载
NUMA FLOW ADAPT <name> <ON|OFF>                           # 开关自适应
```

桥接语义：`enumerate` 用 `numa_get_key_current_node` / `numa_get_key_metadata` 填充
item，`apply` 调用 `numa_migrate_key_by_name`；`emit_migrate` 的决策（`current_node` 变化）
被翻译成真实的 key 迁移。加载的工作流由 `serverCron` 按 `interval_sec` 周期执行。
CMS 频率信号（`cms_estimate` 读取的部分，TinyLFU/CAAT 都依赖）需要有人调用
`nf_tracker_observe()` 才不会永远是 0——`src/numa_flow.c` 新增的
`numa_flow_observe_access()` 从 `src/db.c` 的真实访问路径（与
`numa_key_migrate_touch()` 完全同一处）调用它，是 Redis 桥接里唯一喂这个信号的
地方（见 [ADR-09](../new/09-architecture-decisions.md)：这个调用在此之前完全缺失，
TinyLFU/CAAT 通过桥接实际跑起来时是不会迁移任何数据的）。

## 8. 自适应 DAG（`nf_adapt.c`）

根据每次运行的反馈自动调整 DAG 的**参数**甚至**结构**：

- **参数爬山**：每个可调参数（如 `filter_benefit.threshold`、`demote_cold.threshold`、
  `budget_limit.budget`）轮询微调——反馈变好则保持方向，变差则反向；
- **结构选择**：根据「DRAM 驻留率 + 迁移 churn 率」在三个模板间切换：
  `conservative`（只晋升高收益，抑制抖动）/ `balanced`（CAAT，晋升+降级）/
  `aggressive`（晋升+降级+再平衡，低驻留时激进）；
- 运行中 `nf_adapt_tune()` 返回建议模式，结构变化时自动重建 workflow 并写回参数。

测试：`make test` 包含 `tests/test_adapt.c`（桥接迁移决策 + 自适应结构/参数切换）。

