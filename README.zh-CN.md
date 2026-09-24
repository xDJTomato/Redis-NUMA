# Redis-NUMA

在 Redis 7.2.6 之上扩展了 NUMA 感知的内存分配与 CXL（Compute Express Link）内存
分层能力：透明的、以 NUMA 节点为粒度的分配、按 key 的热度追踪，以及跨节点的冷热
数据迁移——同时完整保留 Redis 原有的 API 兼容性。

另有一个完全独立的纯 C11 子系统 **NUMAflow**（`numaflow/`），把每一种 NUMA 调度
策略以 7 个可配置动作组合成类似 N8N 的 DAG 工作流（旧操作 ID 仍兼容已有模板），
并新增了一个默认策略（CAAT）、一套无需 QEMU 的公平评测框架、一个 TUI、一个 Web GUI，以
及一个轻量级的缓存行为反馈回路。

## 当前状态

Redis 内核经历了一次从 6.2.21 到 7.2.6 的**真实三方合并**（不是纸面计划）——具体
过程、合并中破坏了什么、又是如何逐一发现并修复的，见
[`docs/redis7-migration.md`](docs/redis7-migration.md)。`make -j$(nproc)` 编译
干净通过，`make test` 跑通完整的 Tcl 测试套件。模块布局见
[`ARCHITECTURE.md`](ARCHITECTURE.md)，如何跑通全部测试（包括可选的 QEMU 多
NUMA 节点和 CXLMemSim 校验步骤）见 [`TESTING.md`](TESTING.md)。

## 快速开始

```bash
# 编译（需要 Linux + libnuma；会强制使用 MALLOC=libc，见下面的“编译”一节）
cd src && make clean && make -j$(nproc)

# 用启用了 NUMA 的标准配置启动（参见 redis.conf:2342-2354）
./redis-server ../redis.conf

# 连上去试一试
./redis-cli set foo bar
./redis-cli numa config get
./redis-cli numa strategy list
```

### NUMAflow 完整基准矩阵（2026 年 9 月 24 日）

在 **8 vCPU、15 GiB 内存、仅 1 个真实 NUMA 节点**的 VMware 虚拟机上，
完成了 4 种合成负载 × 2 组建模分层参数的全部测试。每组对 **4 个迁移策略及
9 个分配策略**重放同一条 20 万次访问轨迹（2 万个键、epoch 5000、迁移预算 64、
固定种子 20240517、**模拟的**两个 NUMA 节点）。下表为**建模总净代价**，
单位百万纳秒（访问 + 迁移，**越低越好**），并非真实 Redis 吞吐量。
每格负载名称链接到包含完整命中率、迁移次数、反馈和节点放置的 JSON。

| 模型 | 负载（原始 JSON） | Noop | Composite LRU | TinyLFU | CAAT | CAAT 本地命中 | CAAT 迁移次数 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 默认拓扑 | [zipf](docs/benchmarks/2026-09-24/bench_zipf_default.json) | 504.9 | 238.3 | 228.1 | 183.7 | 84.5% | 13,521 |
| 默认拓扑 | [uniform](docs/benchmarks/2026-09-24/bench_uniform_default.json) | 467.7 | 408.8 | 424.4 | 388.5 | 18.0% | 4,117 |
| 默认拓扑 | [hotspot](docs/benchmarks/2026-09-24/bench_hotspot_default.json) | 456.1 | 250.6 | 326.9 | 185.4 | 76.1% | 10,321 |
| 默认拓扑 | [temporal](docs/benchmarks/2026-09-24/bench_temporal_default.json) | 480.5 | 394.0 | 407.4 | 307.8 | 48.9% | 9,613 |
| CXL 参数情景 | [zipf](docs/benchmarks/2026-09-24/bench_zipf_cxlcal.json) | 190.6 | 126.2 | 124.0 | 121.6 | 79.5% | 15,426 |
| CXL 参数情景 | [uniform](docs/benchmarks/2026-09-24/bench_uniform_cxlcal.json) | 178.7 | 166.8 | 168.9 | 146.2 | 46.9% | 18,125 |
| CXL 参数情景 | [hotspot](docs/benchmarks/2026-09-24/bench_hotspot_cxlcal.json) | 174.9 | 120.9 | 139.8 | 111.5 | 74.6% | 16,570 |
| CXL 参数情景 | [temporal](docs/benchmarks/2026-09-24/bench_temporal_cxlcal.json) | 182.7 | 161.9 | 166.8 | 132.1 | 63.7% | 17,620 |

- 默认拓扑：建模 DRAM/CXL 延迟 **60/300 ns**、带宽 **20,000/8,000 MB/s**；
  CXL 参数情景：模拟 CXL 延迟 **125 ns**、带宽 **25,000 MB/s**。
  **本次没有实测 CXL 设备**，后一组数值只是模型输入。
- 分配策略是初始放置，迁移策略是从所有对象位于慢层开始；**不能直接比较两类
  策略的净代价**。分配模型可将所有对象放在 DRAM，而迁移模型限制 DRAM 容量
  约为工作集的一半。
- CAAT 在这八组模型配置下净代价最低，但**不能据此宣称普遍优于旧算法**。
  同日按仓库标准的 **3,000 键 / 120,000 访问**复测：*uniform* 上 Composite LRU
  为 **127.0M** 建模纳秒，CAAT 为 **166.5M**（多 31.1%）；*temporal* 上分别为
  **148.7M** 和 **153.6M**。旧算法依然重要。查看[标准配置图表](docs/benchmarks/2026-09-24/reference/report.html)
  和 [zipf](docs/benchmarks/2026-09-24/reference/bench_zipf.json) ·
  [uniform](docs/benchmarks/2026-09-24/reference/bench_uniform.json) ·
  [hotspot](docs/benchmarks/2026-09-24/reference/bench_hotspot.json) ·
  [temporal](docs/benchmarks/2026-09-24/reference/bench_temporal.json) 原始数据。

[完整图表](docs/benchmarks/2026-09-24/report.html) ·
[复现脚本](docs/benchmarks/2026-09-24/run.sh) ·
[数据校验](docs/benchmarks/2026-09-24/validate.py) ·
[运行环境记录](docs/benchmarks/2026-09-24/manifest.json)。

编译工具链后执行 `bash docs/benchmarks/2026-09-24/run.sh` 可复现。完整 Redis
编译、96 个 Redis 测试文件、NUMAflow 单元测试及浏览器交互测试均通过。本虚拟机
**未实测**真实多节点/CXL 设备或 YCSB 吞吐量；`run_full_validation.sh --quick`
按设计跳过这些阶段。

### 一条命令完成全部验证

```bash
./run_full_validation.sh --quick     # 编译 + 单元测试 + NUMAflow 基准
./run_full_validation.sh             # 以上，再加上 QEMU 虚拟机冒烟测试 + CXLMemSim
```

会生成 `results/full_report_<timestamp>/index.html`——一份单文件、零依赖的 HTML
报告，内嵌 SVG 图表，汇总每一个步骤的结果。任何在当前环境跑不了的步骤（比如没
有 JDK 导致 YCSB 跑不了、没有 `/dev/kvm` 导致 QEMU 跑不了、CXLMemSim 没编译）都
会被标记为**已跳过（skipped）**并注明原因——绝不伪造一个“通过”的假象。

### NUMAflow 工作流工作台

```bash
make -C numaflow
python3 numaflow/gui/server.py   # 打开 http://127.0.0.1:8090/
```

在右上角选择 **English / 简体中文**；点击节点可查看作用、输入、输出、各模式说明
及编排建议。展开「经典算法预设」可将 36 个原版算法作为新动作的固定模式节点使用；
如需改动，点击「解锁并自定义」。旧版降级/平衡节点的“仅标记”行为不会被改变。
运行按钮使用合成对象模拟测试，不会操作实机 Redis。

## 编译

```bash
cd src
make clean && make -j$(nproc)
```

编译过程会**强制使用 `MALLOC=libc`**，并在 Linux 上链接 `-lnuma`
（`src/Makefile` 第 133–140 行）——这不是随意的选择：jemalloc 和本项目的 NUMA
分配器互不兼容。你需要提前装好 `libnuma-dev`（Debian/Ubuntu）或
`numactl-devel`（CentOS/RHEL）。

在 `src/` 目录下执行 `make` 会生成 `redis-server`、`redis-cli`、
`redis-benchmark`、`redis-sentinel`、`redis-check-rdb`、`redis-check-aof`
六个二进制文件。

## 这个分支（fork）里有什么

在 Redis 内核之上叠加了八个模块，全部由 `#ifdef HAVE_NUMA` 保护，外加 NUMAflow
原子操作引擎（`numaflow/`，现在独家承载全部迁移策略逻辑）（完整拆解见
[`ARCHITECTURE.md`](ARCHITECTURE.md)）：

- **numa_pool** — 自定义分配器：33 个尺寸类，基于原子位图管理的两级 Slab 分配
  （小/大 slab），配合 Thread-Local Cache 实现无锁快速路径。
- **numa_migrate** / **numa_key_migrate** — 块级和 key 级的跨节点迁移，对
  STRING/HASH/LIST/SET/ZSET 全部类型都有完整的适配器。
- **numa_configurable_strategy** — `zmalloc` 层的 7 种独立分配策略
  （LOCAL_FIRST、INTERLEAVE、ROUND_ROBIN、WEIGHTED/WEIGHTED_INTERLEAVE 共用同一套
  加权随机实现、PRESSURE_AWARE、CXL_OPTIMIZED）。ADAPTIVE/LATENCY_AWARE 仍是内核
  侧占位，真正实现放在 NUMAflow。
- **numa_command** — 统一的 `NUMA` 命令（`MIGRATE`/`CONFIG`/`FLOW`）。
- **numa_bw_monitor** — 实时的按节点带宽监控，并提供与 `evict_numa` 共用的
  节点压力取值函数。
- **evict_numa** — NUMA 感知的淘汰逻辑：淘汰一个 key 之前先尝试把它降级。
- **numa_flow.c** — Redis 侧连接 NUMAflow 原子操作引擎的桥接层。迁移策略
  （`caat`/`composite_lru`/`tinylfu`/`noop`）*只*在这里以 NUMAflow DAG 预设的形式
  实现——内核不再有任何原生实现。启动时自动加载 `numa-flow-default-strategy`
  （默认 `caat`）；运行期可用 `NUMA FLOW DEFAULT <name>` 切换，或用
  `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT` 加载自定义工作流。

旧的 16 槽位 vtable 策略框架（`numa_strategy_slots`）及其原生 Composite LRU /
TinyLFU 实现已退役——详见 `docs/new/09-architecture-decisions.md` 的 ADR-08。

## 配置

- `redis.conf` 第 1184–1208 行：`numa-demote-*` 系列配置。
- `redis.conf` NUMA 迁移配置区：`numa-enabled`、`numa-flow-default-strategy`
  （默认 `caat`，也接受 `composite_lru`/`tinylfu`/`noop`）、
  `numa-flow-interval-sec`。`composite_lru.json` 现在只作为字段参考保留，供
  手写自定义 NUMAflow 工作流 JSON 使用，内核不再读取它。

## 文档地图

| 文档 | 内容 |
|---|---|
| [`docs/GUIDE.zh-CN.md`](docs/GUIDE.zh-CN.md) | 面向学生的中文学习指南——NUMA/CXL 背景知识、逐模块讲解、把 6.2.21→7.2.6 迁移当作案例研究、完整测试体系、推荐阅读顺序 |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | 模块布局、依赖顺序、与 Redis 核心的接触点 |
| [`docs/redis7-migration.md`](docs/redis7-migration.md) | 6.2.21 → 7.2.6 这次合并到底做了什么，逐个 bug 记录 |
| [`TESTING.md`](TESTING.md) | 如何跑通每一层测试，包括 QEMU/CXLMemSim |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | 新增一个 NUMA 模块的约定 |
| [`CHANGELOG.md`](CHANGELOG.md) | 版本历史 |
| `docs/new/` | arc42 风格的架构文档：12 个顶层章节 + 每个组件一份 `modules/` 详情表 + `appendix/` |
| `docs/numaflow/` | NUMAflow 子系统的设计与用法 |
| `docs/README.md` | 完整的文档索引，附带事实核对状态表 |

---

*本文档是 [`README.md`](README.md) 的中文版本；英文原文是权威版本，两者并存维护。*
