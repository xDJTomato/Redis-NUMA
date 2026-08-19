# 更新日志

本 fork 的所有重要变更都记录在这里，格式参考
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

> 本文档是 [`CHANGELOG.md`](CHANGELOG.md) 的中文版本；英文版是权威原文，两者并存。

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
