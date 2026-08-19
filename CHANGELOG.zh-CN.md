# 更新日志

本 fork 的所有重要变更都记录在这里，格式参考
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

> 本文档是 [`CHANGELOG.md`](CHANGELOG.md) 的中文版本；英文版是权威原文，两者并存。

## [未发布] — NUMA 迁移策略收敛（ADR-08）

### 变更（破坏性）

- **三个迁移策略（`caat`/`composite_lru`/`tinylfu`）现在唯一存在于 NUMAflow
  原子操作引擎中**（`numaflow/src/nf_strategy.c`）——内核原生实现
  （`src/numa_strategy_slots.{c,h}`、`src/numa_composite_lru.{c,h}`、
  `src/numa_tinylfu.{c,h}`）已被删除。详见
  [`docs/new/09-architecture-decisions.md`](docs/new/09-architecture-decisions.md)
  的 ADR-08。
- `NUMA STRATEGY` 命令整体移除（slot 的 insert/enable/disable/schedule/
  status/list）。改用 `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT` 以及新增的
  `NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>`。
- 移除 `numa-migrate-config` / `NUMA CONFIG LOAD`（composite-lru JSON 热重载）。
  替换为 `numa-flow-default-strategy`（默认 `caat`）与
  `numa-flow-interval-sec`，启动时自动作为 NUMAflow 的 `default` 工作流条目
  加载——现在不需要手动 `NUMA FLOW LOAD` 就能得到迁移行为。`composite_lru.json`
  仍保留在仓库中，但只作为字段名参考；内核不再读取它。
- `NUMA CONFIG GET`/`NUMA MIGRATE STATS` 的响应不再包含
  `access_tracking_enabled`/`locality_stats_enabled`/`debug_logging_enabled`/
  `composite_*`/`accesses_local`/`accesses_remote`/`tinylfu_*` 字段（它们的数据
  来源已不存在）。`NUMA MIGRATE SCAN` 现在改为执行一次 NUMAflow 的 `default`
  条目，而不是一次 composite-lru 扫描；`COUNT` 参数为兼容旧 CLI 而保留，但会被
  忽略。
- 默认迁移行为从 Composite LRU 改为 CAAT（Cost-Aware Adaptive Tiering）——
  ADR-04 自己的评测显示 CAAT 的净代价比 Composite LRU 低约 37%。

### 修复

- **TinyLFU 和 CAAT 通过 Redis 集成（`src/numa_flow.c`）实际上从未真正迁移过
  任何数据**，这个问题在本次收敛之前就已经存在——`cms_estimate`（Count-Min
  Sketch 的频率读取）永远返回 0，因为桥接层里没有任何地方调用
  `nf_tracker_observe()`（CMS 的写入入口）；只有 numaflow 自己的独立基准测试
  harness（`numaflow/src/nf_bench.c`）会调用它，这也是为什么 `ADR-04` 的评测
  数字是真实的，但真实的桥接路径却一直静默失效。修复方式是让追踪器从
  `numa_key_migrate_touch()` 同一条真实访问路径喂数据：新增
  `numa_flow_observe_access()`（`src/numa_flow.c`/`.h`），在 `src/db.c` 里每次
  真实 key 访问时调用。
- **CAAT 还会丢失每一个被降级、但没有同时满足晋升条件的 item**
  （`numaflow/src/nf_strategy.c` 的 `build_caat`，以及 `numaflow/src/nf_adapt.c`
  里等价的 `NF_ADAPT_AGGRESSIVE` 模板）。`nf_exec_run()` 的结果只是图中所有
  *终止（sink）节点*输出的并集；旧版是一条线性链，降级阶段的变更直接喂给晋升
  阶段的过滤器，于是一个被降级但没通过晋升过滤条件的 item，会在到达任何终止
  节点之前就被丢弃——它的降级其实已经真实执行过，但桥接层从未看到、也从未
  上报/应用。修复方式是在两个阶段各自发生任何变更之前，先按每个 item 的原始
  驻留位置分叉图结构（通过 `filter_local`/`filter_remote` 原子操作），让每个
  item 最多被变更一次，并且总能到达且仅到达一个终止节点。用一个独立的
  harness 直接驱动真实的桥接/引擎代码端到端验证了修复效果。
- 之前，per-key 热度追踪（zmalloc 分配前缀上的 `numa_get_hotness`/
  `numa_get_access_count`）只在（现已移除的）Composite LRU/TinyLFU 槎位启用时
  才会更新。现在通过 `numa_key_migrate_touch()`
  （`src/numa_key_migrate.c`/`src/db.c`）无条件更新，这样无论当前启用哪个（或
  不启用任何）迁移策略，NUMAflow 的 `enumerate()` 都能拿到一个真实信号。
- `numa_configurable_strategy` 的 `PRESSURE_AWARE` 分配策略此前读取
  `numa_config_get_node_utilization()`——一个无上限的、以 GB 为单位的字节计数
  器，而不是一个 0.0–1.0 的比例——却拿它去和一个 `1.0` 的最小种子值比较，一旦
  任何节点分配超过 1GB 就会悄悄破坏节点排序。现在改为读取 `evict_numa` 同样
  使用的、规范的 `numa_bw_get_node_pressure()` 信号。
- `evict_numa.c` 和 `numa_configurable_strategy.c` 曾各自维护一套独立的
  节点压力计算逻辑，公式和缓存 TTL 都不同。现已合并为单一的
  `numa_bw_get_node_pressure()`（`src/numa_bw_monitor.c`）。
- `WEIGHTED_INTERLEAVE` 曾是 `WEIGHTED` 加权随机节点选择循环的逐字节复制，
  唯一区别只是读取哪个权重数组。现在两种情况共用同一个
  `select_weighted_node()` 辅助函数。

## [未发布] — `feat/redis7-port` 分支上的 Redis 7 迁移

### 变更

- **Redis 内核从 6.2.21 迁移到 7.2.6**，通过一次真正的三方合并完成（用一次合成的
  嫁接（graft）操作，给这个历史全新的 fork 根提交人工接上一个与上游共同的祖先）。
  完整记录——工具链、发现并修复的每一个 bug、验证方式——见
  [`docs/redis7-migration.md`](docs/redis7-migration.md)。
- 全部 10 个 NUMA 模块都已适配 Redis 7 的 dict API（不透明的 `dictEntry`、
  `dictCreate()` 去掉 `privdata` 参数、回调函数签名改为 `dict *d` 打头）、
  listpack（与遗留的 ziplist 并存）、以及 quicklist 的 `->entry` 字段改名。
- 新增 `src/commands/numa.json`，用 Redis 7 的声明式命令自省系统注册 `NUMA` 命令。
- 文档重写以匹配合并后的真实状态：`README.md`、新增的 `ARCHITECTURE.md`、新增的
  `TESTING.md`、`CONTRIBUTING.md`、本更新日志，以及取代
  `docs/redis8-migration.md` 的 `docs/redis7-migration.md`。

### 新增

- `run_full_validation.sh` —— 单一入口脚本：编译、单元测试、NUMAflow 基准测试，
  以及（可选的）下面的 YCSB、QEMU、CXLMemSim 步骤，最终产出一份汇总的 HTML 报告。
- `tests/vm/boot_numa_vm.sh` —— QEMU（TCG）多 NUMA 节点冒烟测试：启动一个 2 节点
  的模拟客户机，并在其中运行 `redis-server` + `NUMA` 命令族 + `redis-benchmark`。
- `tests/cxl/run_cxlmemsim.sh` —— 校验 CXLMemSim（`SlugLab/CXLMemSim`，
  `external/CXLMemSim`）的设备仿真链路：跑通它自己的 CTest 套件，并确认一个 QEMU
  CXL Type2 端点通过 TCP 成功连接到 `cxlmemsim_server`。
- `tests/cxl/cxlmemsim_workload_bench.cpp` —— 直接通过 CXLMemSim 自己的
  `CXLMemExpander` C++ 模型重放与 NUMAflow 评测框架相同的
  zipf/uniform/hotspot/temporal 工作负载形态，提供一个与 NUMAflow 简化成本模型
  并列对比的真实设备模型基准点。接入过程中发现的两个 CXLMemSim 自身缓存失效顺序
  相关的真实 bug（选择绕过而非直接修补，因为 `external/CXLMemSim` 未被 vendor 进
  本仓库历史）详见 `ARCHITECTURE.md`。
- `numaflow eval --cxl-latency-ns`/`--cxl-bandwidth-mbps` —— 用一次真实 CXLMemSim
  设备链路运行测得的数字，校准 NUMAflow 自身的成本模型，替代 `numa_shim.c` 里合成
  的 tier-1 默认值；`run_full_validation.sh` 现在会以两种方式各跑一遍每种工作负载
  （`results/bench_<workload>.json` 与
  `results/bench_<workload>_cxlcal.json`）。
- `tests/report/generate_full_report.py` —— 把各步骤结果与 NUMAflow 的
  `bench_*.json` 数据汇总成一份内嵌 SVG 图表、无外部依赖的单文件 HTML 报告（扩展了
  `numaflow/eval/report.py` 的思路）；现在也会把经 CXLMemSim 校准的 NUMAflow 运行
  结果和 CXLMemSim 原生模型结果，与合成默认值的对比一起画出来。

### 修复

`git merge` 的 recursive 策略在核心合并过程中静默引入（零冲突标记）的 6 个 bug，
新测试套件恰好暴露出的 1 个历史遗留分配器保护缺失 bug，以及 1 个晚于 `7.2.6` 这个
tag 才发布的上游 CVE。逐文件的完整细节见
[`docs/redis7-migration.md`](docs/redis7-migration.md)：

- `src/zmalloc.c` —— 一个 `HAVE_NUMA` 代码块后面缺失的 `#endif`。
- `src/dict.c` —— 一段多余的重复溢出检查，引用了一个已经被移除的变量。
- `src/server.c` —— 重复的 `afterCommand()` 定义；另外，`call()` 函数里重复的
  `replicationFeedMonitors()` 调用，导致 MONITOR 客户端收到每条命令两次。
- `src/networking.c` —— 重复的 `addReplyBigNum()` 和
  `deferredAfterErrorReply()` 定义。
- `src/server.h`/`src/evict.c`/`src/evict_numa.c` —— 过时的 2 参数
  `objectComputeSize()` 原型与实际的 4 参数签名不一致，导致驱逐降级路径中的一次
  SIGSEGV。
- `src/zmalloc.c` —— `numa_alloc_dram()`（`zmalloc_local`/`zcalloc_local`/
  `ztrycalloc_local()` 底层依赖它）从未检查 `numa_ctx.numa_available`，导致除
  `redis-server` 之外、任何创建 dict 的二进制都会堆损坏（表现为
  `tests/unit/cluster/cli.tcl` 下的 `redis-cli` SIGSEGV）。
- `src/hyperloglog.c` —— cherry-pick 了 CVE-2025-32023 的修复
  （`f35b72dd1`），该修复晚于 `7.2.6` tag 才发布。
- `src/config.c` —— `numa-demote-min-size` 用 `createIntConfig()` 注册，但对应
  字段实际是 `size_t`；改为 `createSizeTConfig()`。

### 移除

- `src/redis8_compat.h` —— 死代码；整个仓库没有任何文件 include 它，真正的迁移
  路径直接走的是 Redis 7 的原生 API。
- `docs/redis8-migration.md` —— 被 `docs/redis7-migration.md` 取代。

## 更早的历史

Redis 7 迁移之前的一切（NUMA 模块本身的实现、NUMAflow 的引入、经 Linux/QEMU 验证
的 6.2.21 基线）都早于本更新日志。详细的提交历史见 `git log`——`9ff536438`
（`checkpoint(redis-6.2): Linux/QEMU verified NUMA 6.2 baseline`）是一个方便的标记
点，代表"Redis 7 迁移开始之前，最后一个已知良好的状态"。
