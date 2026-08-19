# Redis-NUMA

在 Redis 7.2.6 之上扩展了 NUMA 感知的内存分配与 CXL（Compute Express Link）内存
分层能力：透明的、以 NUMA 节点为粒度的分配、按 key 的热度追踪，以及跨节点的冷热
数据迁移——同时完整保留 Redis 原有的 API 兼容性。

另有一个完全独立的纯 C11 子系统 **NUMAflow**（`numaflow/`），把每一种 NUMA 调度
策略都拆解成 36 个可组合的原子操作，以类似 N8N 的 DAG 工作流方式执行，并新增了
一个默认策略（CAAT）、一套无需 QEMU 的公平评测框架、一个 TUI、一个 Web GUI，以
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

### 一条命令完成全部验证

```bash
./run_full_validation.sh --quick     # 编译 + 单元测试 + NUMAflow 基准
./run_full_validation.sh             # 以上，再加上 QEMU 虚拟机冒烟测试 + CXLMemSim
```

会生成 `results/full_report_<timestamp>/index.html`——一份单文件、零依赖的 HTML
报告，内嵌 SVG 图表，汇总每一个步骤的结果。任何在当前环境跑不了的步骤（比如没
有 JDK 导致 YCSB 跑不了、没有 `/dev/kvm` 导致 QEMU 跑不了、CXLMemSim 没编译）都
会被标记为**已跳过（skipped）**并注明原因——绝不伪造一个“通过”的假象。

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

在 Redis 内核之上叠加了十个模块，全部由 `#ifdef HAVE_NUMA` 保护（完整拆解见
[`ARCHITECTURE.md`](ARCHITECTURE.md)）：

- **numa_pool** — 自定义分配器：33 个尺寸类，基于原子位图管理的两级 Slab 分配
  （小/大 slab），配合 Thread-Local Cache 实现无锁快速路径。
- **numa_migrate** / **numa_key_migrate** — 块级和 key 级的跨节点迁移，对
  STRING/HASH/LIST/SET/ZSET 全部类型都有完整的适配器。
- **numa_strategy_slots** + **numa_composite_lru** + **numa_tinylfu** — 一套
  16 槎位的可插拔迁移策略框架；Composite LRU 是默认策略（槎位 1），TinyLFU 可用
  但默认关闭（槎位 2）。
- **numa_configurable_strategy** — `zmalloc` 层的 9 种分配策略（LOCAL_FIRST、
  INTERLEAVE、ROUND_ROBIN、WEIGHTED、PRESSURE_AWARE、CXL_OPTIMIZED、
  WEIGHTED_INTERLEAVE、ADAPTIVE、LATENCY_AWARE）。
- **numa_command** — 统一的 `NUMA` 命令（`MIGRATE`/`CONFIG`/`STRATEGY`）。
- **numa_bw_monitor** — 实时的按节点带宽监控。
- **evict_numa** — NUMA 感知的淘汰逻辑：淘汰一个 key 之前先尝试把它降级。
- **numa_flow.c** — 把 Redis 和 NUMAflow 子系统的策略目录连接起来，通过
  `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT` 暴露出来。

## 配置

- `redis.conf` 第 1184–1208 行：`numa-demote-*` 系列配置。
- `redis.conf` 第 2342–2354 行：`numa-enabled` 和 `numa-migrate-config`
  （指向 `composite_lru.json`）。

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
