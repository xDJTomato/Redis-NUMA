# 文档索引与当前状态

本目录聚合了 Redis-NUMA 项目的全部文档。为避免文档与代码脱节，这里给出**权威的当前状态**
与各文档的定位。

## 当前状态（事实核对）

| 项目 | 状态 |
| --- | --- |
| Redis 内核版本 | **7.2.6**（`feat/redis7-port` 分支，`src/version.h`；6.2.21 → 7.2.6 的真实三方合并 + 编译 + 全量测试验证过程见 `docs/redis7-migration.md`） |
| Redis 内核 NUMA 模块 | 10 个模块（`numa_pool` / `numa_migrate` / `numa_key_migrate` / `numa_strategy_slots` / `numa_composite_lru` / `numa_tinylfu` / `numa_configurable_strategy` / `numa_command` / `numa_bw_monitor` / `evict_numa`），已全部适配 Redis 7 的 opaque `dictEntry` / listpack / quicklist `->entry` API |
| 分配策略数量 | **9 种**（`local_first` / `interleaved` / `round_robin` / `weighted` / `pressure_aware` / `cxl_optimized` / `weighted_interleave` / `adaptive` / `latency_aware`） |
| 新增 NUMAflow 子系统 | `numaflow/`（纯 C11，无 Redis/libnuma 依赖），36 个原子操作、13 个内置策略 |
| 新默认策略 | **CAAT**（Cost-Aware Adaptive Tiering，晋升 + 降级） |
| Redis 7 迁移 | `docs/redis7-migration.md` 记录**实际执行**的合并步骤、6 个被 merge 静默引入的真实 bug 及修复、1 个被测试意外触发的历史 bug、1 个需要单独 cherry-pick 的 CVE（2025-32023）。原先的 `docs/redis8-migration.md` 与 `src/redis8_compat.h`（未被任何文件 include 的死代码）已删除 |
| Redis 桥接适配器 | `src/numa_flow.c`（`HAVE_NUMA` 下编译）：`NUMA FLOW` 命令加载/运行/列出 DAG 工作流 |
| QEMU 多 NUMA 节点验证 | `tests/vm/boot_numa_vm.sh`：本机无 `/dev/kvm`，纯 TCG 软件模拟启动 2 节点客户机，跑通 `redis-server` + `NUMA` 命令族 + `redis-benchmark` |
| CXLMemSim 集成 | `external/CXLMemSim`（`SlugLab/CXLMemSim`）：自带改版 QEMU + `cxlmemsim_server` 已构建，`tests/cxl/run_cxlmemsim.sh` 验证设备级链路（CXL Type2 端点 <-> server 的 TCP 时序转发） |
| 一体化验证脚本 | 仓库根目录 `run_full_validation.sh`：编译 → 单测 → NUMAflow 基准 → (可选) YCSB → (可选) QEMU → (可选) CXLMemSim → 单文件 HTML 报告 |
| 自适应 DAG | `numaflow/src/nf_adapt.c`：参数爬山 + 结构切换（conservative/balanced/aggressive） |
| 独立内存分配器 | `numaflow/src/nf_alloc.c`：无 header + metamap + tcache，吞吐 ≈ malloc 1.85×，见 `docs/numaflow/allocator.md` |
| 新手模板库 | 23 个开箱即用模板（tiering/allocation/cost/adaptive/special），见 `docs/numaflow/templates.md` |

## 文档目录

- **`new/`** — Redis 内核 NUMA 模块的设计文档（`00` 方案设计 → `19` AE 策略调度器技术设计）。
  其中 `08-numa-configurable.md` 的 `adaptive`/`latency_aware` 在内核中为占位实现，
  完整实现在 NUMAflow 的 `alloc_adaptive`/`alloc_latency_aware` 原子操作。
- **`numaflow/README.md`** — NUMAflow 子系统（本次新增）的架构、原子操作拆解、CAAT 策略、
  公平评测、TUI/GUI、追踪框架与构建说明。
- **`redis7-migration.md`** — Redis 6.2.21 → 7.2.6 真实合并记录：工具链、6 个静默合并 bug、
  1 个 CVE cherry-pick、验证方式。
- **`test/`** — 测试组织、YCSB 指南、历史基准结果（含 `benchmark_results.txt` 中新增的
  NUMAflow 公平评测一节）、诊断使用说明。
- **`thesis_chapter3.md` / `thesis_chapter4.md` / `thesis_review/`** — 论文章节与修改稿。
- **`devlog/`** — 开发日志（`zmalloc-goals.txt` 等）与原始 Redis README（`original/`）。

## 构建与测试（快速入口）

```bash
# 一体化验证（推荐）：编译 + 单测 + NUMAflow 基准 + 可选 QEMU/CXLMemSim/YCSB
./run_full_validation.sh --quick   # 跳过慢速的 QEMU/CXLMemSim/YCSB 步骤
./run_full_validation.sh           # 跑全部步骤

# Redis 内核（需 Linux + libnuma）
cd src && make clean && make -j$(nproc) && cd .. && make test

# NUMAflow 子系统（纯 C11，任意平台）
cd numaflow && make && make test && make report
```
