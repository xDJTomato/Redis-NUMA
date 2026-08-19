# CLAUDE.md（中文版）

> 本文件是 [`CLAUDE.md`](CLAUDE.md) 的中文翻译，供人阅读参考。**权威版本是英文原文
> `CLAUDE.md`**——Claude Code 实际读取并用来配置自己在本仓库中行为的是那份英文文件；
> 这份中文版不会被工具读取，只是方便中文读者理解内容，如果两者出现不一致，以英文
> 原文为准。

本文件为 Claude Code（claude.ai/code）在本仓库中工作时提供指引。

## 项目概览

Redis 7.2.6 扩展了 NUMA 感知的内存分配与 CXL（Compute Express Link）内存分层能
力。项目新增了透明的、以 NUMA 节点为粒度的分配、按 key 的热度追踪，以及跨节点的
冷/热数据迁移，同时完整保留 Redis 的 API 兼容性。

另有一个独立的**纯 C11** 子系统 **NUMAflow**（`numaflow/`），把每一种 NUMA 调度
策略都拆解成 **36 个可组合的原子操作**，可以像 N8N 风格的 DAG 工作流那样执行；此
外还新增了：一个新的默认策略（CAAT）、一个不依赖 QEMU 的公平评测框架、一个 TUI
和一个 Web GUI，以及一个轻量级的缓存行为追踪反馈回路。Redis 6.2.21 → 7.2.6 的核
心迁移是一次真实的三方合并（不是纸面计划），记录在 `docs/redis7-migration.md`
中，包含合并过程中静默引入的每一个 bug 以及它们是如何被发现的。核心代码在
Linux + libnuma 环境下可以编译通过，并通过完整的 `make test` 套件。

## 构建命令

```bash
cd src
make clean && make -j$(nproc)
```

在 Linux 上，构建会**强制使用 `MALLOC=libc`** 并链接 `-lnuma`（`src/Makefile` 第
133–140 行）。jemalloc 与 NUMA 分配器不兼容。需要 `libnuma-dev`（Debian/Ubuntu）
或 `numactl-devel`（CentOS/RHEL）。

构建全部目标：在 `src/` 下运行 `make` 会产出 redis-server、redis-cli、
redis-benchmark、redis-sentinel、redis-check-rdb、redis-check-aof。

## 运行测试

```bash
# 单一入口：编译 + make test + NUMAflow 基准测试（可选 YCSB/QEMU/CXLMemSim）
./run_full_validation.sh --quick   # 快速模式：跳过 YCSB/QEMU/CXLMemSim
./run_full_validation.sh           # 完整模式：聚合 HTML 报告写入 results/full_report_<ts>/

# 标准 Redis 测试套件（基于 Tcl）
cd src && make test

# NUMA 专项功能测试
cd tests/ycsb && ./run_bw_benchmark.sh    # 主基准测试（三阶段：Fill→Hotspot→Sustain）
cd tests/ycsb && ./run_ycsb.sh            # YCSB baseline/stress 模式

# 快速 NUMA 环境检查
./utils/numa/check_numa_config.sh
./utils/numa/diagnose_numa.sh

# NUMAflow 子系统测试（纯 C11，这台 Windows 主机上也能跑）
cd numaflow && make test

# 可选：QEMU 多 NUMA 节点冒烟测试与 CXLMemSim 设备级链路校验
./tests/vm/boot_numa_vm.sh
./tests/cxl/run_cxlmemsim.sh
```

测试结构：
- `tests/unit/*.tcl` — 标准 Redis 单元测试
- `tests/ycsb/` — YCSB 性能基准测试（主测试框架）
- `tests/ycsb/workloads/` — 工作负载定义（baseline、stress、bw_saturate、numa_migration）
- `tests/legacy/numa/` — 归档的 NUMA 功能测试（C/bash）
- `tests/ycsb/scripts/` — 辅助脚本（安装、评测、报告生成）
- `tests/vm/` — QEMU 多 NUMA 节点冒烟测试（TCG，没有 `/dev/kvm` 时会优雅跳过）
- `tests/cxl/` — CXLMemSim 设备仿真链路校验（`external/CXLMemSim`）
- `tests/report/` — `run_full_validation.sh` 使用的 HTML 报告生成器

每一层测试的完整说明见 `TESTING.md`。

## 架构

### NUMA 模块层（叠加在 Redis 核心之上）

`src/` 下的十个模块，全部由 `#ifdef HAVE_NUMA` 保护：

1. **numa_pool** — 自定义内存分配器。33 个尺寸类（8B–64KB），基于原子位图管理的
   两级 Slab 分配（小/大 slab），配合 Thread-Local Cache 实现无锁快速路径。
2. **numa_migrate** — 通过 `numa_zmalloc_onnode` + memcpy 实现 NUMA 节点间的底层
   块迁移（注：实际的按 key 迁移由下面的 numa_key_migrate 独立实现，并不调用这
   个模块的迁移函数）。
3. **numa_key_migrate** — 按 key 迁移（以 robj 为单位）。集成 LRU 式热度追踪，带
   惰性阶梯衰减。为全部 5 种 Redis 类型提供完整的类型适配器：STRING
   (RAW/EMBSTR)、HASH (listpack/ziplist/hashtable)、LIST (quicklist，含
   LZF/raw 以及 PLAIN/PACKED 容器子路径)、SET (intset/hashtable)、ZSET
   (listpack/ziplist/skiplist)。
4. **numa_strategy_slots** — 基于 vtable 多态的 16 槽位可插拔策略框架。槎位 0 =
   空操作，槎位 1 = Composite LRU，槎位 2 = TinyLFU（默认关闭）。通过
   `serverCron` 每秒运行一次。
5. **numa_composite_lru** — 默认迁移策略（槎位 1）。双通道：热候选环形缓冲区
   （快路径）+ 渐进式字典扫描（慢路径）。可通过 JSON 配置。
6. **numa_tinylfu** — 频率驱动的迁移策略（槎位 2，默认关闭）。Count-Min Sketch
   (4×16384，4-bit) + Doorkeeper 布隆过滤器。固定约 40KB 内存占用，O(1) 发现热
   数据。需手动启用，以避免与 Composite LRU 冲突。
7. **numa_configurable_strategy** — `zmalloc` 层的 9 种分配策略（LOCAL_FIRST、
   INTERLEAVE、ROUND_ROBIN、WEIGHTED、PRESSURE_AWARE、CXL_OPTIMIZED、
   WEIGHTED_INTERLEAVE、ADAPTIVE、LATENCY_AWARE）。
8. **numa_command** — 统一的 `NUMA` Redis 命令：`NUMA MIGRATE`、`NUMA CONFIG`、
   `NUMA STRATEGY`。
9. **numa_bw_monitor** — 实时的每节点带宽监控（resctrl/numastat/manual 三种
   backend）。
10. **evict_numa** — NUMA 感知的驱逐：淘汰前先把 key 降级到压力较小的节点。加权
    评分：距离(40%) + 压力(30%) + 带宽(30%)。

### 与 Redis 核心的关键接触点

- **zmalloc.c/h** — 所有 `zmalloc/zfree/zrealloc` 在 NUMA 可用时都会经过 NUMA 分
  配器。每次分配都带一个 16 字节的 `numa_alloc_prefix_t` 前缀，记录大小、节点、
  热度、访问元数据。强制开启 `NO_MALLOC_USABLE_SIZE`。
- **server.h** — `redisServer` 结构体中的 NUMA 统计计数器与配置字段。NUMA 头文
  件在 `#ifdef HAVE_NUMA` 下被包含。
- **server.c** — `numa_init()` 在 `main()` 中、`initServer()` 之前调用。策略/按
  键迁移/带宽监控的初始化在 `initServer()` 之后进行。周期性的策略执行在
  `serverCron` 中完成。
- **evict.h** — 淘汰逻辑里插入了一次无状态的降级尝试调用（`evictionTryNumaDemote`），
  在真正淘汰一个 key 之前先看能不能把它迁移到压力更小的节点；`evictionPoolEntry`
  结构体本身未被修改。

### 模块依赖顺序（从底层到顶层）

libnuma → numa_pool → numa_migrate → numa_key_migrate → numa_composite_lru /
numa_tinylfu / numa_strategy_slots → numa_command → evict_numa → server.c

### NUMAflow 子系统（`numaflow/`，纯 C11，无 Redis/libnuma 依赖）

在任何带 C11 编译器的平台上都能构建和测试（`make` 或 `mingw32-make`）：

```bash
cd numaflow && make && make test && make report
./build/numaflow ops          # 列出 36 个原子操作
./build/numaflow strategies   # 列出 13 个内置策略（CAAT 为默认）
python gui/server.py          # N8N 风格的 DAG 编辑器，http://127.0.0.1:8090
```

组成：`include/` + `src/`（引擎）、`tui/nf_tui.c`（交互式 TUI）、`gui/`（Web 编
辑器 + Python 桥接）、`eval/report.py`（SVG/HTML 可视化）、`tests/`（单元 + 桥接/
自适应 + 冒烟测试）。关键文件：`nf_ops.c`（36 个原子操作）、`nf_strategy.c`（策
略目录，含 CAAT）、`nf_bench.c`（公平评测器）、`nf_track.c`（CMS + Doorkeeper +
EWMA 反馈）、`nf_bridge.c`（存储无关的桥接契约 + 迁移执行）、`nf_adapt.c`（自适
应 DAG：参数爬山 + 结构选择）、`numa_shim.c`（可移植的 libnuma 仿真）。

Redis 侧适配器 `src/numa_flow.c`（只在 `HAVE_NUMA` 下编译）实现了两个桥接回调，
并暴露 `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT`；`serverCron` 按各自的时间
间隔运行已加载的工作流。`nf_adapt_tune()` 汇总每次运行的 DRAM 驻留反馈，可以在
conservative/balanced/aggressive 三种模板间切换 DAG 结构，并对其参数做爬山调优。

## 配置

- `redis.conf` 第 1184–1208 行：`numa-demote-*` 相关设置（enable、min-size、
  max-migrate、pressure-threshold、weights）
- `redis.conf` 第 2342–2354 行：`numa-enabled` 以及指向 `composite_lru.json` 的
  `numa-migrate-config` 路径
- `composite_lru.json`：每节点带宽基线与迁移调优参数

## 文档

- `README.md` — 项目概览、快速上手、`run_full_validation.sh` 用法
- `ARCHITECTURE.md` — 模块布局、依赖顺序、与 Redis 核心的集成点
- `docs/new/` — arc42 风格的架构文档：12 个顶层章节（`01-introduction-and-goals.md`
  到 `12-glossary.md`）、每个组件一份 `modules/` 详情表（包括此前没有专门文档的
  `numa_bw_monitor` 和 `evict_numa`），以及一个 `appendix/`（调用链完整参考、相关
  工作对比）
- `docs/numaflow/` — NUMAflow 子系统的设计与用法
- `docs/redis7-migration.md` — Redis 6.2.21 → 7.2.6 的真实合并记录：工具链、发
  现并修复的每一个 bug、执行过的验证
- `docs/test/` — 测试组织方式、基准测试结果与使用指南（benchmark_results.txt、
  EXECUTIVE_SUMMARY.txt、DIAGNOSIS_USAGE.txt）
- `docs/devlog/` — 开发日志与设计笔记（例如 zmalloc-goals.txt）
- `TESTING.md` — 每一层测试，包括可选的 QEMU/CXLMemSim 步骤
- `CONTRIBUTING.md` — 新增 NUMA 模块的规范
- `CHANGELOG.md` — 版本历史（Keep a Changelog 格式）

## 开发规范

新增一个 NUMA 模块时：
1. 先建 `.h`（接口/结构体），再建 `.c`（实现）
2. 把 `numa_xxx.o` 加入 `src/Makefile` 的 `REDIS_SERVER_OBJ`
3. 在 `#ifdef HAVE_NUMA` 下把头文件包含进 `server.h`
4. 在 `server.c` 里 `initServer()` 之后调用初始化函数
5. 使用 `extern void _serverLog(...)`——不要直接用 `serverLog()`（Redis 内部约
   定）
6. NUMA 的 `.o` 文件必须排在 Makefile 链接顺序中 `server.o` 之后

## 关键注意事项

- **绝不要用 jemalloc** — 构建强制使用 libc，但如果你改动了 Makefile 的编译选
  项，NUMA 会被破坏
- **初始化顺序很重要** — `initServer()` 必须先完整执行完，才能调用任何
  `numa_*_init()`
- **serverLog 不能直接用** — 在 NUMA 模块里要用
  `extern void _serverLog(int level, const char *fmt, ...)`
- **全部 5 种数据类型的迁移适配器都已完整实现** — STRING、HASH、LIST、SET、
  ZSET，都正确处理了各自的编码（listpack/ziplist/hashtable/quicklist/skiplist）
</content>
