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

### 2.1 已有策略的 DAG 分解

**Composite LRU（slot 1）**：

```text
score_hotness → filter_hot → rank_hotness → budget_limit → select_dest_node → emit_migrate
```

**TinyLFU（slot 2）**（每访问的 `cms_observe` 在热路径完成，批处理只读估计）：

```text
cms_estimate → filter_freq → rank_frequency → budget_limit → select_dest_node → emit_migrate
```

### 2.2 新的默认策略：CAAT（Cost-Aware Adaptive Tiering）

现有策略要么只看热度（Composite LRU），要么只看频率（TinyLFU），且都**只升不降**：
一旦 DRAM 写满就停止晋升，无法回收。CAAT 是一个完整的**晋升 + 降级**流水线，在
两个阶段各自发生任何变更**之前**先按原始驻留位置分叉（`filter_local`/
`filter_remote`），避免旧版单链设计里"降级已执行但未通过晋升阶段过滤条件从而
永远不到达终止节点、导致桥接层看不到这次迁移"的 bug（见
[ADR-09](../new/09-architecture-decisions.md)）：

```text
filter_local  → score_cost_benefit → demote_cold → emit_migrate      （DRAM 驻留项：只走降级子链）
filter_remote → cms_estimate → filter_freq → filter_benefit → rank_cost
              → budget_limit → select_dest_node → emit_migrate       （非 DRAM 驻留项：只走晋升子链）
```

其核心是 `score_cost_benefit`：

```text
benefit = (cost(当前节点) - cost(目标节点)) × 访问率 - 迁移代价
```

只有**净收益为正**的 key 才会被晋升，且按收益排序、受容量与预算双重约束；同时把
DRAM 上不再热的 key 降级到 CXL，让 DRAM 始终保持最优驻留。

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

