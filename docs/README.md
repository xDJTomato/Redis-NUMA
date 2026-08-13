# 文档索引与当前状态

本目录聚合了 Redis-NUMA 项目的全部文档。为避免文档与代码脱节，这里给出**权威的当前状态**
与各文档的定位。

## 当前状态（事实核对）

| 项目 | 状态 |
| --- | --- |
| Redis 内核版本 | **6.2.21**（`src/version.h` 未改；迁移到 Redis 8 需在 Linux + libnuma 环境重编译） |
| Redis 内核 NUMA 模块 | 10 个模块（`numa_pool` / `numa_migrate` / `numa_key_migrate` / `numa_strategy_slots` / `numa_composite_lru` / `numa_tinylfu` / `numa_configurable_strategy` / `numa_command` / `numa_bw_monitor` / `evict_numa`） |
| 分配策略数量 | **9 种**（`local_first` / `interleaved` / `round_robin` / `weighted` / `pressure_aware` / `cxl_optimized` / `weighted_interleave` / `adaptive` / `latency_aware`） |
| 新增 NUMAflow 子系统 | `numaflow/`（纯 C11，无 Redis/libnuma 依赖），36 个原子操作、13 个内置策略 |
| 新默认策略 | **CAAT**（Cost-Aware Adaptive Tiering，晋升 + 降级） |
| Redis 8 迁移 | 已提供 `docs/redis8-migration.md` 指南 + `src/redis8_compat.h` 兼容头；内核本身仍为 6.2.21 |
| Redis 桥接适配器 | `src/numa_flow.c`（`HAVE_NUMA` 下编译）：`NUMA FLOW` 命令加载/运行/列出 DAG 工作流 |
| 自适应 DAG | `numaflow/src/nf_adapt.c`：参数爬山 + 结构切换（conservative/balanced/aggressive） |

## 文档目录

- **`new/`** — Redis 内核 NUMA 模块的设计文档（`00` 方案设计 → `19` AE 策略调度器技术设计）。
  其中 `08-numa-configurable.md` 的 `adaptive`/`latency_aware` 在内核中为占位实现，
  完整实现在 NUMAflow 的 `alloc_adaptive`/`alloc_latency_aware` 原子操作。
- **`numaflow/README.md`** — NUMAflow 子系统（本次新增）的架构、原子操作拆解、CAAT 策略、
  公平评测、TUI/GUI、追踪框架与构建说明。
- **`redis8-migration.md`** — Redis 6.2.21 → 8 迁移指南与验证清单。
- **`test/`** — 测试组织、YCSB 指南、历史基准结果（含 `benchmark_results.txt` 中新增的
  NUMAflow 公平评测一节）、诊断使用说明。
- **`thesis_chapter3.md` / `thesis_chapter4.md` / `thesis_review/`** — 论文章节与修改稿。
- **`devlog/`** — 开发日志（`zmalloc-goals.txt` 等）与原始 Redis README（`original/`）。

## 构建与测试（快速入口）

```bash
# Redis 内核（需 Linux + libnuma）
cd src && make clean && make -j$(nproc) && make test

# NUMAflow 子系统（纯 C11，任意平台）
cd numaflow && make && make test && make report
```
