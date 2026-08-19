# 2. 架构约束

> arc42 §2 Architecture Constraints。这些是"不可协商"的边界条件——不是设计选择，
> 而是设计必须服从的前提。下面按技术约束、组织约束、惯例约束三类列出，每条都标注
> 了理由和验证方式，而不是简单地宣称"必须这样"。

## 2.1 技术约束

| 约束 | 内容 | 理由 |
| --- | --- | --- |
| **必须是 Linux + libnuma** | NUMA 特有能力仅在检测到 `Linux` 时启用（`src/Makefile` 第 133-139 行：`ifneq (,$(findstring Linux,$(uname_S)))` 内追加 `-lnuma`、`-DHAVE_NUMA`），其余平台自动退化为标准 Redis 行为 | `libnuma` 是 Linux 专有的内核/用户态 NUMA 拓扑查询与绑定接口，没有跨平台等价物；NUMAflow 子系统为此专门实现了一套纯 C11 的 `numa_shim.c` 仿真层，以便在非 Linux 环境（如 Windows 笔记本）上也能开发和评测策略，但那属于评测环境，不是 `redis-server` 本体的运行环境 |
| **必须强制 `MALLOC=libc`** | 一旦检测到 Linux，`src/Makefile` 第 137 行无条件把 `MALLOC` 设为 `libc`，覆盖用户可能传入的其他值 | 本项目的分配器直接接管 `zmalloc` 分配路径、在每次分配前后写入 16 字节的 `numa_alloc_prefix_t` 元数据；如果同时启用 jemalloc，两套分配器会对同一块内存的元数据产生互相踩踏的写入，导致堆损坏。这是全仓库唯一一条"绝不能改"的编译选项——`CONTRIBUTING.md` 和 `CLAUDE.md` 都专门标注了这一条 |
| **Redis 事件循环仍是单线程** | 本项目没有改变 Redis 核心的单线程命令处理模型；所有周期性工作（策略执行、chunk 压缩）挂在 `serverCron` 或 AE time event 上，而不是独立线程 | 这是继承自 Redis 本身的架构约束，不是本项目引入的，但它直接决定了迁移/扫描逻辑必须是**渐进式、有预算**的（见 [`08-crosscutting-concepts.md`](08-crosscutting-concepts.md)），不能一次性做完，否则会阻塞所有客户端请求 |
| **内核版本锁定在 7.2.6，不是 8.x** | 分支 `feat/redis7-port`，`src/version.h` 中 `REDIS_VERSION "7.2.6"` | 7.2.6 是 Redis 许可证从 BSD-3-Clause 改为 RSALv2/SSPL 双许可（7.4 起）**之前**的最后一个稳定版本，同时已经包含影响本项目 NUMA 模块的关键 API 变化（opaque `dictEntry`、listpack、quicklist 容器类型）。选它既拿到了需要的 API，又避免了许可证变更和更大的 8.x 重构，保持在本 fork 一直使用的 BSD-3-Clause 许可证下。完整理由见 [`../redis7-migration.md`](../redis7-migration.md#why-726-not-8) |
| **不是每个二进制都调用 `numa_init()`** | 只有 `redis-server` 的 `main()` 调用 `numa_init()`；`redis-cli`/`redis-benchmark`/`redis-check-rdb`/`redis-check-aof`/`redis-sentinel` 都链接了 `zmalloc.o`，但没有一个调用它 | 任何假设"NUMA 分配器已初始化"的新代码，必须显式对 `numa_ctx.numa_available` 做判断并提供普通 `malloc` 退化路径——这条约束曾经因为遗漏（`zmalloc_local()`/`zcalloc_local()`/`ztrycalloc_local()`）导致 `redis-cli --cluster create` 在 `zfree()` 内部 SIGSEGV，详见 [`../redis7-migration.md`](../redis7-migration.md#a-pre-existing-bug-the-merge-exposed-not-caused) |

## 2.2 组织约束

| 约束 | 内容 | 理由 |
| --- | --- | --- |
| **Redis 内核的版本迁移必须是真实的三方合并，不能是纸面重写** | 6.2.21 → 7.2.6 的迁移通过 `git replace --graft` 让本仓库的根提交拥有一个指向上游 6.2.21 的合成父提交，从而让 Git 计算出正确的合并基点，执行一次真正的三方合并，而不是从零对拍 | 早先曾有一份"纸面设计"文档（`docs/redis8-migration.md`，已删除），其中多个假设在真正尝试合并后被证明是错的；这条约束就是从那次教训里直接得出的：**任何跨版本迁移都必须以一次可审查的真实合并为基础，其记录必须是"实际发生了什么"，不能是"计划要发生什么"** |
| **合并"没有冲突标记"不等于"合并正确"** | 6.2.21→7.2.6 合并里有 6 个真实 bug 在零冲突标记的情况下混入代码，全部靠真正编译 + 跑测试套件才发现 | 由此定下的强制流程：任何非小合并之后，必须跑一次完整的 `make -j$(nproc)` 编译到底，并把每一个编译错误当作"可能是合并静默损坏了代码"去核查，而不是想当然地当成正常的 API 迁移工作量。完整案例见 [`../redis7-migration.md`](../redis7-migration.md) |
| **测试脚本必须诚实降级，绝不伪造通过** | YCSB 缺 JDK、QEMU 缺 `/dev/kvm`、CXLMemSim 未构建等情况下，测试脚本会记录 `skipped` 并写明原因，而不是跳过后仍报告"通过" | 这是本项目所有验证脚本（`run_full_validation.sh` 及其调用的每一层）共享的组织纪律，直接影响了本文档集的写作方式：任何"已验证"的表述都必须有对应的真实运行记录 |

## 2.3 惯例约束

| 约束 | 内容 | 理由 |
| --- | --- | --- |
| **模块依赖顺序必须遵守** | `libnuma → numa_pool → numa_migrate → numa_key_migrate → numa_bw_monitor → numa_configurable_strategy → numa_flow（NUMAflow 桥接） → numa_command → evict_numa → server.c`；对应 `src/Makefile` 里 `REDIS_SERVER_OBJ` 的链接顺序要求——NUMA 的 `.o` 文件必须排在 `server.o` 之后 | 上层模块的初始化依赖下层模块建立好的状态；打乱顺序（例如在 `numa_pool` 初始化之前调用迁移函数）大概率导致启动阶段崩溃。详见 [`05-building-block-view.md`](05-building-block-view.md) |
| **`numa_init()` 必须在 `initServer()` 之前调用，其余 NUMA 模块的初始化必须在 `initServer()` 之后** | `server.c` 的 `main()` 里，`numa_init()` 先于 `initServer()`；按键迁移/带宽监控/NUMAflow 桥接的初始化在 `initServer()` 之后 | 后面这些模块的初始化依赖 `initServer()` 建好的 `redisServer` 状态；顺序颠倒是本项目里最常见的一类启动期崩溃原因 |
| **禁止直接调用 `serverLog()`** | NUMA 模块内部必须使用 `extern void _serverLog(int level, const char *fmt, ...)`，这是既有的 Redis 内部惯例（可在 `numa_bw_monitor.c`、`numa_flow.c` 等文件中看到这个模式） | 保持与仓库既有惯例一致，避免符号可见性或链接期问题 |
| **新增模块遵循固定的落地步骤** | 先写 `.h`（接口/结构体）再写 `.c`（实现）；把 `numa_xxx.o` 加进 `src/Makefile` 的 `REDIS_SERVER_OBJ`；在 `server.h` 的 `#ifdef HAVE_NUMA` 下 include 头文件；在 `server.c` 的 `initServer()` 之后调用初始化函数 | 是对上面几条约束的操作化整理，完整清单见 [`../../CONTRIBUTING.md`](../../CONTRIBUTING.md) |

## 延伸阅读

- 这些约束如何体现在具体模块划分上，见 [`05-building-block-view.md`](05-building-block-view.md)。
- 违反约束曾经导致的真实故障，完整案例见 [`../redis7-migration.md`](../redis7-migration.md)。
- 已知的、约束带来的遗留限制（而非违反约束造成的 bug），见 [`11-risks-and-technical-debt.md`](11-risks-and-technical-debt.md)。
