# Redis-NUMA 学习指南

> 本文档是写给**学生 / 初次接触本项目的人**的完整导览，风格上参考 Redis 官方文档
> （redis.io）——每个命令给出语法、说明与真实的 `redis-cli` 交互示例，每个模块给出
> 「它解决什么问题」「怎么解决」「在哪个文件」。它不会取代其它文档，而是把它们串成
> 一条可以从头读到尾的主线；每一节末尾都有指向更详细文档的链接。
>
> 建议阅读方式：如果你是第一次接触 NUMA / CXL，请从第 1 章开始，不要跳过背景知识；
> 如果你已经熟悉这些概念、只是想快速搞清楚这个仓库的代码结构，可以直接跳到第 3 章。

## 目录

1. [为什么需要一个「NUMA-aware」的 Redis](#第-1-章为什么需要一个numa-aware的-redis)
2. [项目地图：五分钟跑起来](#第-2-章项目地图五分钟跑起来)
3. [内核里改了什么：八个模块逐一讲解](#第-3-章内核里改了什么八个模块逐一讲解)
4. [配置详解](#第-4-章配置详解)
5. [NUMAflow 子系统：把策略拆成乐高积木](#第-5-章numaflow-子系统把策略拆成乐高积木)
6. [一次真实的版本迁移：Redis 6.2.21 → 7.2.6](#第-6-章一次真实的版本迁移redis-6221--726)
7. [测试与验证体系](#第-7-章测试与验证体系)
8. [性能数据一览](#第-8-章性能数据一览)
9. [常见坑与 FAQ](#第-9-章常见坑与-faq)
10. [推荐学习路径](#第-10-章推荐学习路径)

---

## 第 1 章：为什么需要一个「NUMA-aware」的 Redis

### 1.1 什么是 NUMA

在一台多路（multi-socket）服务器上，每个 CPU 插槽（socket）通常直接连着一部分物理
内存——这部分内存对该插槽上的核心来说是「本地内存」（local memory），访问延迟低；
访问挂在**另一个**插槽上的内存，则要经过插槽间的互联总线（如 Intel 的 UPI、AMD 的
Infinity Fabric），延迟明显更高，带宽也更受限。这种「内存访问延迟取决于访问者和
内存物理位置的距离」的架构，就叫 **NUMA**（Non-Uniform Memory Access，非统一内存
访问）。

一台典型的双路服务器上，跨节点访问的延迟可能是本地访问的 1.5～2 倍。如果一个像
Redis 这样单线程处理请求、但会被操作系统调度到任意 CPU 核心上运行的进程，其数据
恰好被 `malloc` 分配在了「远端」NUMA 节点上，那么**每一次**访问这块内存都要多付出
这部分延迟——这个成本会在高 QPS 场景下被放大成实际可测的尾延迟。

用 `numactl --hardware` 或本仓库的 `./utils/numa/check_numa_config.sh` 可以看到你
机器上的 NUMA 拓扑（节点数、每个节点的 CPU 范围、节点间的 distance 矩阵）。

### 1.2 什么是 CXL，为什么它像「插得更远的一个 NUMA 节点」

CXL（Compute Express Link）是一种建立在 PCIe 物理层之上的互联协议，其中的
**CXL.mem** 子协议允许把挂在 PCIe 插槽上的内存模组（或未来的内存池化设备）当作
CPU 可以直接 `load`/`store` 的普通内存来用——不需要走文件系统、不需要走网络协议
栈，对软件而言就是「多了一块内存」。但它的物理路径比本地 DRAM 通道更长、协议开销
更高，延迟通常在几百纳秒量级（相比本地 DRAM 的几十纳秒），带宽也通常低于本地内存
通道。

也就是说，从操作系统和应用程序的视角看，**一块 CXL 内存基本上就是又多了一个
「NUMA 节点」，只是它比传统的远端 socket 内存更慢、更便宜、容量可以做得更大**。这
正是本项目把 CXL 内存管理问题当成 NUMA 分层问题来解决的原因：不需要为 CXL 发明一
整套新概念，只需要把现有的「热数据留在本地节点、冷数据迁到远端节点」这套 NUMA 迁
移逻辑，扩展到「本地 DRAM 节点 vs. CXL 慢速节点」这个新场景。

### 1.3 原生 Redis 分配器的问题

原生 Redis 依赖 `libc` 的 `malloc`（或 jemalloc）分配内存，这些分配器**完全不知道
NUMA 拓扑**——它们只关心把一块空闲内存尽快给你，不关心这块内存物理上挂在哪个节
点。结果是：

- 同一个 key 的数据，可能被分配在离当前处理它的 CPU 核心很远的节点上；
- 没有任何机制去追踪「这个 key 到底有多热」，从而决定要不要把它挪到更快的内存；
- 完全没有意识到「这块内存其实是一片慢速的 CXL 扩展内存，应该只放冷数据」。

### 1.4 这个项目做了什么（一句话概览）

在保留 Redis 全部 API 兼容性的前提下，本项目：

1. 把 `zmalloc` 分配路径改造成 NUMA 感知的：可以按策略把新分配的内存放到指定节点
   （见 [3.5](#35-numa_configurable_strategy7-种分配策略)）；
2. 给每个 key 加上「热度」追踪，并用 NUMAflow 的可插拔策略在后台周期性地把热 key
   迁到快速节点、把冷 key 降级到慢速（CXL）节点（见 [3.3](#33-numa_migrate--numa_key_migrate块--key-两级迁移)、[3.4](#34-迁移策略去哪了现在统一收敛到-numaflow)）；
3. 新增了一个完全独立、跟 Redis/libnuma 无关的纯 C11 子系统 **NUMAflow**
   （见 [第 5 章](#第-5-章numaflow-子系统把策略拆成乐高积木)），把「迁移策略」这
   件事拆成可组合的原子操作，并在此基础上设计了一个新的默认策略 **CAAT**。

带着这个整体图像，我们从「怎么把它跑起来」开始。

---

## 第 2 章：项目地图：五分钟跑起来

### 2.1 目录结构（只看和本指南相关的部分）

```text
Redis-NUMA/
├── src/                    # Redis 内核 + 8 个 NUMA 模块（本文第 3 章）
│   ├── numa_*.c/h           # 八个模块（含 numa_flow.c，NUMAflow 桥接适配器）
│   └── commands/numa.json   # NUMA 命令的声明式注册（COMMAND INFO/DOCS 用）
├── numaflow/                # 独立子系统：策略引擎（本文第 5 章）
├── redis.conf               # 配置文件，NUMA 相关参数见第 4 章
├── composite_lru.json       # 已退役：仅作 NUMAflow composite_lru 预设的字段参考，内核不再读取
├── tests/                   # 测试（第 7 章）
│   ├── unit/*.tcl            # 标准 Redis 单测
│   ├── ycsb/                 # YCSB 基准测试
│   ├── vm/                   # QEMU 多 NUMA 节点冒烟测试
│   └── cxl/                  # CXLMemSim 设备级链路校验
├── docs/                    # 全部文档，见 docs/README.md 的索引
└── run_full_validation.sh   # 一体化验证脚本
```

### 2.2 编译与第一次运行

```bash
# 编译需要 Linux + libnuma-dev（或 numactl-devel）
cd src
make clean && make -j$(nproc)
```

编译脚本会**强制使用 `MALLOC=libc`**（`src/Makefile` 第 133–140 行）并链接
`-lnuma`——这不是随意的选择：本项目的分配器直接接管了 `zmalloc`，如果同时启用
jemalloc，两个分配器会互相踩踏内存元数据。**如果你要改 Makefile 的编译选项，
千万不要引入 jemalloc。**

编译成功后会在 `src/` 下得到 `redis-server`、`redis-cli`、`redis-benchmark`、
`redis-sentinel`、`redis-check-rdb`、`redis-check-aof` 六个二进制。

启动并连接：

```bash
./src/redis-server ./redis.conf --daemonize yes --logfile /tmp/r.log
./src/redis-cli set foo bar
./src/redis-cli numa config get
./src/redis-cli numa flow list
```

如果一切正常，`numa config get` 会打印当前的分配策略、节点权重等信息；
`numa flow list` 会打印当前已加载的 NUMAflow 工作流及其状态——默认情况下会有一条
名为 `default` 的条目，跑的是 `numa-flow-default-strategy` 指定的策略（默认
`caat`），Redis 启动时自动加载，不需要手动 `NUMA FLOW LOAD`。

### 2.3 一体化验证

```bash
./run_full_validation.sh --quick     # 编译 + 单测 + NUMAflow 基准（快，几分钟）
./run_full_validation.sh             # 以上 + YCSB + QEMU 多节点冒烟 + CXLMemSim
```

会在 `results/full_report_<timestamp>/index.html` 生成一份单文件 HTML 报告（内嵌
SVG 图表）。任何在当前环境跑不了的步骤（没有 JDK、没有 `/dev/kvm`、没有编译
CXLMemSim）都会被标记为 `skipped` 并写明原因——**这个脚本的设计原则是宁可诚实地跳
过，也不伪造一个「通过」的假象**，这个原则贯穿本项目所有测试脚本，在 [第 7 章](#第-7-章测试与验证体系)
会反复看到。

### 2.4 文档索引

| 文档 | 内容 |
| --- | --- |
| `docs/README.md` | 全部文档的中文索引 + 事实核对表（当前状态的权威来源） |
| `ARCHITECTURE.md` | 模块布局、依赖顺序、与 Redis 核心的接触点（英文，本指南第 3 章是它的中文讲解版） |
| `docs/redis7-migration.md` | 6.2.21→7.2.6 真实合并记录（本指南第 6 章的原始素材） |
| `TESTING.md` | 每一层测试怎么跑（本指南第 7 章的原始素材） |
| `CONTRIBUTING.md` | 新增一个 NUMA 模块的规范流程 |
| `docs/numaflow/README.md` | NUMAflow 子系统详细设计（本指南第 5 章的原始素材） |
| `docs/new/` | arc42 风格的架构文档：12 个顶层章节 + `modules/` 逐组件详情表 + `appendix/` |

---

## 第 3 章：内核里改了什么：八个模块逐一讲解

### 3.0 全局依赖顺序

八个模块不是互相独立的，它们有严格的依赖顺序（既是加载顺序，也是 `src/Makefile`
里链接顺序的要求——NUMA 的 `.o` 文件必须排在 `server.o` **之后**）：

```text
libnuma
  └─> numa_pool                （分配器）
       └─> numa_migrate         （底层块迁移）
            └─> numa_key_migrate（按 key 迁移）
                 └─> numa_bw_monitor        （带宽/压力监控）
                      └─> numa_configurable_strategy（分配层选节点）
                           └─> numa_flow    （NUMAflow 桥接，迁移策略）
                                └─> numa_command （统一命令入口）
                                     └─> evict_numa
                                          └─> server.c （main 函数把它们串起来）
```

（三个迁移策略——`caat`/`composite_lru`/`tinylfu`——曾经各有一份内核原生实现，
经 [ADR-08](../new/09-architecture-decisions.md) 收敛后统一由 `numa_flow` 桥接到
NUMAflow 的原子操作引擎，见 3.4 节与[第 5 章](#第-5-章numaflow-子系统把策略拆成乐高积木)。）

这个顺序背后的直觉很简单：**先有分配器，才能有迁移；先有迁移的底层能力，才能有
「按什么策略触发迁移」；先有策略，才能有把策略暴露给用户的命令；最后才是把这套
迁移能力接进 Redis 原有的淘汰（eviction）逻辑。** 新增模块时如果打乱这个顺序（比
如在 `numa_pool` 初始化之前就调用迁移函数），大概率会在启动阶段就崩溃——这也是
`CONTRIBUTING.md` 里反复强调的一条规则。

### 3.1 numa_pool：自定义分配器

**文件**：`src/numa_pool.c` / `.h`

这是最底层的模块，直接决定"一次 `malloc` 请求，内存物理上分配在哪个 NUMA 节点"。
设计要点：

- **33 个尺寸类（size class）**，覆盖 8 字节到 64KB——类似 jemalloc/tcmalloc 的
  思路：把请求量化到固定档位，避免每次都精确匹配尺寸带来的碎片和查找开销。
- **原子位图管理的两级 Slab**：小对象（≤4KB）走 64KB 的小 slab，大对象走 2MB 的
  大 slab；每个 slab 内部用一个原子位图记录哪些槎位空闲，分配/释放都是原子位图
  操作（`bitmap_find_and_set`/`bitmap_clear`），不需要全局锁。
- **Thread-Local Cache（tcache）**：每个线程为每个尺寸类维护一小批已分配对象，
  命中 tcache 时完全不用碰共享的 slab 状态，是真正的无锁快速路径；tcache 缓存打
  满后批量归还（drain）给共享 slab。

每一次分配都会在返回给调用者的指针前面藏一个 **16 字节的 `numa_alloc_prefix_t`**
前缀（定义在 `src/zmalloc.c`），记录这块内存的大小、所在节点、热度、访问元数据
——这也是为什么本项目必须 `#define NO_MALLOC_USABLE_SIZE`：Redis 原生依赖
`malloc_usable_size()` 来做内存统计，但加了前缀之后这个系统调用返回的"可用大
小"已经不准确，必须绕过它、自己记账。

### 3.2 numa_migrate：底层块迁移

**文件**：`src/numa_migrate.c` / `.h`

在 `numa_pool` 之上提供最原始的"把一块内存从节点 A 搬到节点 B"能力：调用
`numa_zmalloc_onnode()` 在目标节点分配新内存，`memcpy` 内容过去，释放旧内存。
**但要注意**：实际的按 key 迁移（下一节 3.3）并不会调用这个模块的迁移函数，而
是在 `numa_key_migrate.c` 里针对每种数据类型独立实现了一遍"分配+拷贝+释放"——
`numa_migrate_memory()` 本身在当前代码里没有任何调用点，属于尚未接入主路径的
遗留接口。放在这里主要是作为理解"跨节点迁移的最小操作单元长什么样"的参考。

### 3.3 numa_key_migrate：块 → key 两级迁移

**文件**：`src/numa_key_migrate.c` / `.h`

这一层把"迁移一块内存"升级成"迁移一个 Redis key（一个 `robj`）"，这比听起来复杂
得多，因为 Redis 的 5 种数据类型各自有好几种内部编码（encoding），每种编码的内存
布局都不一样：

| 类型 | 涉及的编码 |
| --- | --- |
| STRING | RAW、EMBSTR |
| HASH | listpack、ziplist（兼容旧 RDB）、hashtable |
| LIST | quicklist（内部区分 LZF 压缩/原始，以及 `PLAIN`/`PACKED` 两种节点容器子路径） |
| SET | intset、hashtable |
| ZSET | listpack、ziplist、skiplist |

本项目为这**全部** 5 种类型、每种类型的**全部**编码都实现了对应的迁移适配器——这
是 `CLAUDE.md` 里特别强调"全部实现，不是占位"的部分。迁移逻辑同时集成了 **LRU 式
热度追踪**：每次访问一个 key 会更新它的热度计数，热度计数带**惰性阶梯衰减**（不需
要为每个 key 单独跑定时器去衰减，而是在下次访问时才补算衰减量，省掉大量无意义的
后台开销）。

### 3.4 迁移策略去哪了：现在统一收敛到 NUMAflow

**文件**：`src/numa_flow.c` / `.h`（`HAVE_NUMA` 下才编译）

这个项目早期的设计是内核原生的 **16 槎位可插拔策略框架**（`numa_strategy_slots`）
分别装载两个手写策略实现：Composite LRU（槎位 1，默认开启，双通道热候选环形缓冲
区 + 渐进式字典扫描）和 TinyLFU（槎位 2，默认关闭，Count-Min Sketch + Doorkeeper
布隆过滤器，固定约 40KB 内存）。这套框架已经**整体退役**——`src/numa_strategy_
slots.{c,h}`、`src/numa_composite_lru.{c,h}`、`src/numa_tinylfu.{c,h}` 都已从代码
库删除，原因和过程记在 [ADR-08](../new/09-architecture-decisions.md)：简单说是因
为 NUMAflow（见[第 5 章](#第-5-章numaflow-子系统把策略拆成乐高积木)）早就把这两
个算法拆成了等价的原子操作 DAG，两份实现要同步维护，而且内核默认用的是较差的
Composite LRU，更好的 CAAT 却要手动加载才能跑。

现在，三个迁移策略——`caat`（新默认）、`composite_lru`、`tinylfu`，外加什么都不
做的 `noop`——**只**作为 NUMAflow 的原子操作 DAG 预设存在（`numaflow/src/
nf_strategy.c`），`numa_flow.c` 是接入 Redis 的唯一桥接层：

- 启动时按 `numa-flow-default-strategy`（默认 `caat`）自动把对应预设注册成
  NUMAflow 的 `default` 工作流条目，跟着 `numa-flow-interval-sec`（默认 1 秒）
  周期执行——不再需要手动 `NUMA FLOW LOAD` 才能得到迁移行为；
- 运行时用 `NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>` 在三个预设间切
  换，或者用 `NUMA FLOW LOAD` 加载自定义工作流 JSON；
- `numa_key_migrate_touch()`（3.3 节）不再受"槎位 1/2 是否启用"的条件限制，改为
  在 `db.c` 的真实访问路径上无条件调用，为 NUMAflow 的 `enumerate()` 提供中立的热
  度 ground truth。

想理解 Composite LRU/TinyLFU/CAAT 具体怎么用原子操作拼出来、以及退役前这套框架
长什么样，见[第 5 章](#第-5-章numaflow-子系统把策略拆成乐高积木)和已标注"已退
役"的模块文档（`docs/new/modules/numa_strategy_slots.md` 等），这里不重复教一遍
已经删除的 vtable 机制。

### 3.5 numa_configurable_strategy：7 种分配策略

**文件**：`src/numa_configurable_strategy.c` / `.h`

前面几节讲的是"数据已经在某个节点上了，要不要把它迁走"；这一节是更早一步的决
策：**一次新的 `zmalloc` 请求，第一次应该分配到哪个节点**。7 种独立行为（原来的
`WEIGHTED_INTERLEAVE` 只是 `WEIGHTED` 权重来源不同的一份重复实现，现在合并成同一
个 `select_weighted_node()` 辅助函数，不再算独立的第 9 种）：

| 策略 | 思路 |
| --- | --- |
| `LOCAL_FIRST` | 优先分配在当前 CPU 所在的节点，本地节点满了才考虑其它节点 |
| `INTERLEAVE` | 简单轮流分配到各节点（类似 `numactl --interleave`） |
| `ROUND_ROBIN` | 与 INTERLEAVE 类似，按固定顺序循环 |
| `WEIGHTED` | 按人工配置的权重比例分配，`WEIGHTED_INTERLEAVE` 的轮流变体共用同一份加权随机逻辑，只是权重来源不同（见 `NUMA CONFIG SET weight`） |
| `PRESSURE_AWARE` | 优先分配到当前内存压力（使用率）更低的节点——现在读取的是 `numa_bw_monitor` 提供的 `numa_bw_get_node_pressure()`，和 `evict_numa` 共用同一个压力信号 |
| `CXL_OPTIMIZED` | 区分"快速本地节点"与"慢速 CXL 节点"，把新分配优先放本地，为 CXL 节点保留给迁移降级用 |
| `ADAPTIVE` / `LATENCY_AWARE` | 根据运行时反馈/节点间实测延迟动态调整（内核中为占位实现，完整版本在 NUMAflow 的 `alloc_adaptive`/`alloc_latency_aware` 原子操作里） |

> 注意最后两种：内核里的实现是占位（placeholder），真正可用的完整实现是在
> [第 5 章](#第-5-章numaflow-子系统把策略拆成乐高积木)要讲的 NUMAflow 子系统里，
> 通过 `NUMA FLOW` 命令桥接进 Redis。这是这个项目一个刻意的设计取舍（见
> [ADR-05](../new/09-architecture-decisions.md)）：内核里的策略保持简单、可预测；
> 复杂的自适应逻辑放到独立子系统里迭代，不直接耦合进内核的关键路径。这一层现在
> 不止在文档里说明这件事——设置时会打一条启动日志，`NUMA CONFIG GET` 的回复里也
> 多了一个 `strategy_note` 字段说明"这是占位，完整实现见 NUMAflow"。

### 3.6 numa_command：统一命令入口

**文件**：`src/numa_command.c` / `.h`，命令声明在 `src/commands/numa.json`

所有和 NUMA 相关的操作都挂在一个顶层命令 `NUMA` 下面（这是 Redis 命令设计的常见
模式——用一个"命名空间命令"聚合一组子命令，而不是注册一堆平级的新命令）。完整
参考：

```text
NUMA MIGRATE KEY <key> <node>      迁移单个 key 到目标 NUMA 节点
NUMA MIGRATE DB <node>             把整个数据库迁移到目标节点
NUMA MIGRATE SCAN [COUNT n]        手动触发一次 NUMAflow default 工作流（COUNT 仅为兼容旧 CLI 保留，已不生效）
NUMA MIGRATE STATS                 查看迁移统计
NUMA MIGRATE RESET                 重置迁移统计
NUMA MIGRATE INFO <key>            查看某个 key 的 NUMA 元数据（当前节点/热度等）

NUMA CONFIG GET                    查看当前分配器配置（含 strategy_note 占位说明字段）
NUMA CONFIG SET strategy <name>    设置分配策略（见 3.5 的 7 种策略名）
NUMA CONFIG SET weight <node> <w>  设置某节点权重
NUMA CONFIG SET cxl_optimization <on|off>
NUMA CONFIG SET balance_threshold <percent>
NUMA CONFIG SET enabled_nodes <all|n[,m]>
NUMA CONFIG REBALANCE              手动触发一次再平衡
NUMA CONFIG STATS                  查看每节点的分配统计

NUMA FLOW LOAD <name> <path.json> [interval_sec] [ADAPT]   加载自定义 NUMAflow 工作流
NUMA FLOW RUN [name]                                        立即执行（省略 name 则全部执行）
NUMA FLOW LIST / STATUS <name>                              列出/查看工作流运行状态与反馈分
NUMA FLOW UNLOAD <name>                                     卸载
NUMA FLOW ADAPT <name> <ON|OFF>                             开关自适应
NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>         运行时切换默认迁移策略

NUMA HELP                          打印本帮助
```

`NUMA STRATEGY`（槎位插拔/启停/调度/查询）和 `NUMA CONFIG LOAD`（composite-lru
JSON 热加载）随 ADR-08 的收敛一起被整体移除；`NUMA CONFIG SET`/`GET` 里
`access_tracking`/`locality_stats`/`debug_logging` 这几个 composite-lru 私有参数
也一并消失了，因为它们的数据源不再存在。

上面这份表本身就是从 `numa_command.c` 里的帮助文本原样摘出的——`redis-cli numa
help` 随时可以查到最新版本，不需要记忆。

一个完整的手动迁移示例：

```bash
$ ./src/redis-cli numa migrate info mykey
1) "node"
2) "0"
3) "hotness"
4) "3"
$ ./src/redis-cli numa migrate key mykey 1
OK
$ ./src/redis-cli numa migrate info mykey
1) "node"
2) "1"
...
```

（具体返回字段以你运行的版本为准，上面是示意。）

### 3.7 numa_bw_monitor：带宽监控

**文件**：`src/numa_bw_monitor.c` / `.h`

实时统计每个 NUMA 节点的内存带宽占用，三种数据来源可选：`resctrl`（Intel RDT 提
供的硬件级带宽/占用监控接口，最精确但需要内核和硬件支持）、`numastat`（读取
`/proc` 下的软件统计，兼容性最好）、`manual`（人工配置的静态值，用于没有前两者时
的降级方案）。这个模块产出的带宽数据会被 `numa_configurable_strategy` 的
`PRESSURE_AWARE` 策略和 `evict_numa` 的评分（见下一节）消费。

### 3.8 evict_numa：NUMA 感知的淘汰

**文件**：`src/evict_numa.c`（接口声明在 `evict.h` 里，没有独立的 `evict_numa.h`）

Redis 原生的淘汰逻辑是"内存不够了，按 LRU/LFU/TTL 等策略选一批 key 直接删掉"。本
项目在**删除之前**插入了一步：**能不能先把这个 key 降级（demote）到压力更小的节
点，而不是直接删掉？** 只有在降级也无法缓解压力，或者这个 key 太小不值得迁移时，
才真正走原生淘汰路径。是否降级、降级到哪个节点，由一个加权评分决定：

```text
score = 距离(40%) + 压力(30%) + 带宽(30%)
```

即优先选**离当前节点近**、**内存压力小**、**带宽没有饱和**的目标节点。这个决策
是一次**无状态的函数调用**（`evictionTryNumaDemote`），插在 Redis 原有淘汰循环
（`performEvictions()`）里、真正执行淘汰之前——`evict.h` 里的 `evictionPoolEntry`
结构体本身**没有被修改**（仍然是 `idle`/`key`/`cached`/`dbid` 四个原生字段），
不需要额外的持久状态来记录"这个 key 当前在哪个节点、是否迁移过"，这些信息在决
策的当下直接查询得到。

### 3.9 与 Redis 核心的三个关键接触点

除了上面八个独立模块，本项目还在 Redis 核心的三处"缝进"了钩子：

- **`zmalloc.c` / `.h`**——所有 `zmalloc`/`zfree`/`zrealloc` 都被改造成先检查 NUMA
  是否可用，可用则走 NUMA 分配器，否则原样退化成普通 `malloc`。这个"退化"很重要：
  `redis-cli`、`redis-benchmark`、`redis-check-rdb`/`aof`、`redis-sentinel` 这些
  二进制**都链接了 `zmalloc.o`，但没有一个会调用 `numa_init()`**（只有
  `redis-server` 的 `main()` 会），所以这些二进制里 `numa_ctx.numa_available`
  永远是 0，必须靠这层退化保证它们仍然能正常分配内存——这条"退化保护"在
  [第 6 章](#第-6-章一次真实的版本迁移redis-6221--726)会讲到一个真实的、因为这个
  细节没做全而导致的 SIGSEGV。
- **`server.c`**——`numa_init()` 在 `main()` 里、`initServer()` **之前**调用；策略
  /按键迁移/带宽监控这些模块的初始化则在 `initServer()` **之后**调用（它们依赖
  `initServer()` 建好的状态）。周期性的策略执行都挂在 `serverCron`
  里，跟着 Redis 原有的 1 秒定时任务节奏走，不需要额外起线程。
- **`evict.h`**——如 3.8 所述，插入了一次无状态的降级尝试调用，
  `evictionPoolEntry` 结构体本身未被修改。

---

## 第 4 章：配置详解

`redis.conf` 里和 NUMA 相关的配置分成两块：

### 4.1 降级（demote）相关（第 1184–1211 行）

```conf
# 是否启用"淘汰前先降级"（对应 3.8 节 evict_numa）
# numa-demote-enabled yes

# 小于此大小的对象直接淘汰，不值得为它付迁移成本
# numa-demote-min-size 1kb

# 每轮淘汰周期最多允许几次降级迁移（控制迁移抖动）
# numa-demote-max-migrate 3

# 节点压力超过此阈值（0-100）就不会被选为降级目标
# numa-demote-pressure-threshold 90

# 三个权重（应当合计为 100），对应 evict_numa 评分公式：
# numa-demote-distance-weight 40    # 越大越偏好"物理距离近"的目标节点
# numa-demote-pressure-weight 30    # 越大越偏好"内存压力小"的目标节点
# numa-demote-bandwidth-weight 30   # 越大越偏好"带宽没有饱和"的目标节点

# 距离相同时是否优先选更近的节点
# numa-demote-prefer-closer yes

# 带宽饱和阈值（0-100），超过此值的节点不会被选为降级目标
# numa-bw-saturation-threshold 95
```

### 4.2 总开关与策略配置文件（第 2340–2354 行）

```conf
# NUMA 管理总开关（默认 yes）。设为 no 时，热度追踪/自动迁移/降级
# （即上面全部 numa-demote-* 配置）都会被关闭，但 NUMA 感知的分配
# （slab/direct、加权轮询）仍然生效——适合"只想要分配优化、不想要
# 自动迁移"的场景。
# numa-enabled yes

# 启动时自动加载为默认迁移策略的 NUMAflow 原子操作预设名字。三个曾经
# 内核原生实现的策略现在都只作为 NUMAflow 预设存在：
#   caat          - 成本感知自适应分层（晋升+降级），当前默认
#   composite_lru - 阶梯热度双通道迁移
#   tinylfu       - Count-Min Sketch + Doorkeeper 频率驱动迁移
#   noop          - 不做任何自动迁移
# 运行时可用 NUMA FLOW DEFAULT <name> 切换，不需要重启。
# numa-flow-default-strategy caat

# 默认 NUMAflow 工作流通过 serverCron 运行的间隔（秒）。
# numa-flow-interval-sec 1
```

`composite_lru.json` 已经退役为"字段名参考"——内核不再读取它；如果要手写一份
NUMAflow 的 `composite_lru` 工作流 JSON，可以参考它里面的字段命名（每节点带宽基
线、迁移调优参数），最新版本请直接看文件内注释。

---

## 第 5 章：NUMAflow 子系统：把策略拆成乐高积木

### 5.1 为什么要重新设计

第 3 章的策略（Composite LRU、TinyLFU）都是**手写的一整套逻辑**：想理解、修改、
或者公平地对比它们和一个新想法，只能整段整段地读 C 代码。NUMAflow 换了一个思路：
把"一个迁移/分配策略"抽象成**一串可以像流水线一样连起来的原子操作**，类似
n8n/Node-RED 那种可视化工作流工具的思路，只是这里的"节点"是内存调度里的具体动作
（打分、过滤、排序、迁移……）。这样做的好处：

1. 已有策略可以被"翻译"成一串原子操作的组合，方便对照理解；
2. 新策略只是重新排列/替换这些原子操作，不需要从零写一整个模块；
3. 整个引擎是**纯 C11、零 Redis/libnuma 依赖**——可以在任何机器（包括没有真实 NUMA
   硬件的笔记本）上编译、测试、评测，极大降低了"研究一个新调度策略"的门槛。

### 5.2 36 个原子操作

按功能分成 6 类（源码在 `numaflow/src/nf_ops.c`）：

| 类别 | 操作 | 对应的语义 |
| --- | --- | --- |
| **alloc**（分配） | `alloc_local_first` `alloc_interleave` `alloc_round_robin` `alloc_weighted` `alloc_pressure_aware` `alloc_cxl_optimized` `alloc_weighted_interleave` `alloc_adaptive` `alloc_latency_aware` | 对应第 3.5 节的 7 种分配策略——这里才是 `ADAPTIVE`/`LATENCY_AWARE` 的完整实现（`alloc_weighted_interleave` 是 `alloc_weighted` 权重来源不同的变体，内核侧对应同一个 `WEIGHTED` 行为） |
| **score**（打分） | `score_hotness` `decay_hotness` `cms_observe` `cms_estimate` `global_decay` `score_ewma` `score_cost_benefit` | Composite LRU 的阶梯热度 / TinyLFU 的 CMS+Doorkeeper |
| **filter**（过滤） | `filter_hot` `filter_freq` `filter_cold` `filter_remote` `filter_local` `filter_size_min` `filter_size_max` `filter_benefit` | 从候选集合里筛出满足条件的 key |
| **rank**（排序） | `rank_lru` `rank_frequency` `rank_hotness` `rank_cost` `rank_ewma` `rank_size` | 决定"先迁移谁" |
| **decide**（决策） | `select_dest_node` `budget_limit` | 选目标节点 / 限制这一轮最多迁移多少 |
| **emit**（执行） | `emit_migrate` `demote_cold` `balance_nodes` | 真正触发迁移 / 冷数据降级 / 节点间再平衡 |
| **track**（追踪） | `track_access` | 记录一次访问，供后续打分用 |

### 5.3 已有策略如何用原子操作拼出来

**Composite LRU**（历史上曾经是内核槎位 1，现在只以 NUMAflow 预设的形式存在，
`build_composite_lru`）：

```text
score_hotness → filter_hot → rank_hotness → budget_limit → select_dest_node → emit_migrate
```

**TinyLFU**（历史上曾经是内核槎位 2，现在是 `build_tinylfu` 预设；`cms_observe`
在每次访问的热路径完成，其余在批处理里只做只读的频率估计）：

```text
cms_estimate → filter_freq → rank_frequency → budget_limit → select_dest_node → emit_migrate
```

把这两串图和第 3.4 节的文字描述对照读一遍，会发现每一步都能一一对应上——这
正是 NUMAflow 存在的意义：**同一个算法，用图和用 C 代码看到的是同一件事，只是抽
象层次不同。**

### 5.4 新策略 CAAT：只升不降的问题，以及怎么解决

Composite LRU 和 TinyLFU 有一个共同的局限：**它们都只会"升"（把冷节点的热数据搬
到快节点），一旦快节点（DRAM）写满了，就没有办法继续晋升新的热 key**——因为它们
从没有考虑过"把 DRAM 上已经不再热的数据主动降级出去，给新的热数据腾位置"。

**CAAT（Cost-Aware Adaptive Tiering，成本感知自适应分层）**是一条完整的"晋升 +
降级"流水线：

```text
# 降级子链：原本就在 DRAM（本地）上的 item
filter_local → cms_estimate → score_cost_benefit → demote_cold → emit_migrate

# 晋升子链：原本在 CXL（远端）上的 item
filter_remote → filter_freq → filter_benefit → rank_cost → budget_limit → select_dest_node → emit_migrate
```

两条子链在 `filter_local`/`filter_remote` 这一步就按 item **原始驻留位置**分叉，
之后各自独立跑到底、只在自己的终止 `emit_migrate` 处变更一次状态。这不是随便选
的写法——[ADR-09](../new/09-architecture-decisions.md) 记录了一个真实发现的 bug：
最早的 `build_caat` 是一条单链（降级阶段的 `emit_migrate` 直接喂给晋升阶段的过滤
器），DAG 引擎的最终结果只取"没有出边的终止节点"输出的并集，所以任何被降级、但
没通过晋升阶段过滤条件的 item，会在到达任何终止节点之前被丢弃——它的降级其实已
经真实执行了，但桥接层的"入队原始节点 vs 结果最终节点"diff 逻辑完全看不到它，宿
主的 `apply()` 回调永远不会被调用，等价于这次降级从未发生过。TinyLFU 还有另一个
独立的 bug：`cms_estimate` 读取的频率信号从来没被 Redis 侧的桥接代码喂过数据（只
有 NUMAflow 自己的评测 harness 会调用写入侧的 `nf_tracker_observe()`），修复方式
是新增 `numa_flow_observe_access()`，从 `src/db.c` 的真实访问路径调用它——和
3.4 节提到的 `numa_key_migrate_touch()` 走的是同一条路径。

核心是 `score_cost_benefit` 这一步算的一个净收益公式：

```text
benefit = (cost(当前节点) − cost(目标节点)) × 访问率 − 迁移代价
```

直觉是：一个 key 值不值得迁移，取决于"迁过去之后每次访问能省多少延迟，乘以它被
访问的频率，再减去这一次迁移本身的固定成本"。**只有净收益为正的 key 才会被真正
晋升**，并且所有净收益为正的候选还要按收益排序、受容量和预算的双重约束——这避免
了"为了迁移一个几乎不会再被访问的 key 而占用本该给更热 key 用的迁移预算"。同时，
DRAM 上不再热的 key 会被主动降级到 CXL 节点，从而保证 DRAM 始终留给"当下最值得占
用它"的数据，而不是"历史上曾经热过"的数据。

实测结果（zipf 分布 / 3000 个 key / 120000 次访问 / DRAM 容量约束在工作集的 50%）：

| 策略 | 本地命中率 | 净代价 | 迁移次数 |
| --- | --- | --- | --- |
| 基线（no-op，完全不迁移） | 0.0% | 305.0M | 0 |
| Composite LRU | 81.2% | 134.5M | 2187 |
| TinyLFU | 79.3% | 105.9M | 832 |
| **CAAT（新）** | **91.1%** | **84.9M** | 2422 |

CAAT 在净代价上比表现最好的既有基线（TinyLFU）低约 **20%**，比 Composite LRU 低
约 **37%**——代价是迁移次数略高于两者（换回来的是命中率的明显提升）。

> **这张表后来被重新核实过，结论比最初看起来更细致**（见 ADR-09"遗留事项"，
> `docs/new/09-architecture-decisions.md`）：上面这条单链丢结果的 bug，
> `numaflow/src/nf_bench.c`（这张表数字的来源）自己也会中，用修复后的二进制重新
> 生成四个基准工作负载后发现，bug 对结果的影响是真实且不小的——例如 zipf 场景下
> CAAT 修复前/后的命中率是 84.6% → 91.0%，净代价下降约 54%（其它三个工作负载的
> 净代价下降幅度在 50%-57% 之间）。同时，把修复后的 CAAT 拿去和 Composite
> LRU/TinyLFU 做四种工作负载的横向对比后，"CAAT 全面更优"这个结论需要加条件：
> CAAT 在冷热分层明显的负载（zipf、hotspot）上确实更优（净代价比 Composite LRU
> 低 36.9%/39.5%，比 TinyLFU 低 19.9%/16.4%），但在访问接近均匀分布、没有真正的
> "冷"数据的 uniform 负载上反而比 Composite LRU 差（净代价高 31.1%）——`demote_
> cold` 主动搬走"冷"数据这个动作，在没有真实冷热差异时变成纯粹的浪费开销；
> temporal 负载上两者基本打平（CAAT 净代价高 3.3%）。结论是：ADR-08 把 CAAT 设为
> 默认这个决策本身不需要重新考虑（真实的 Redis 访问分布大多是偏态的，接近
> zipf/hotspot），但引用这张表时应理解为"CAAT 在冷热分层明显时更优，在访问接近
> 均匀分布时反而可能更差"，而不是一个无条件成立的单一百分比。具体数值会随工作负
> 载分布变化，`numaflow eval`/`make bench` 可以在不同负载下重新跑出这张表。

### 5.5 公平评测框架：没有真实多节点硬件也能可信地比较策略

这是本子系统另一个重要贡献：如果没有一台真正的多路 NUMA/CXL 服务器，怎么让"策略
A 比策略 B 好 20%"这句话可信？`nf_bench.c` 的做法是把"公平性"拆成 5 条明确的约
束，全部满足才算一次有效对比：

1. **相同拓扑**——DRAM 容量被人为限制在工作集的约 50%，逼真地制造"装不下、必须
   取舍"的场景，而不是在一个宽松到策略永远不需要做决定的拓扑上比较；
2. **相同工作负载**——`zipf`（长尾）/`uniform`（均匀）/`hotspot`（少量极热点）
   /`temporal`（热点随时间漂移）四种合成负载，用同一个随机种子生成；
3. **相同访问轨迹**——先把访问序列完整生成一遍，所有策略重放**完全相同**的一条
   序列，而不是各自独立采样（独立采样会引入采样方差，让对比失去意义）；
4. **相同预算与容量**——迁移预算、节点容量在各策略之间保持一致，`emit_migrate`
   强制执行容量上限，不允许某个策略"偷偷"用更宽松的资源限制；
5. **确定性执行**——用 xorshift64* 伪随机源、单线程执行，保证同样的输入产出比特
   级完全相同的输出，方便复现和调试。

这套框架跑在 `numa_shim.c` 提供的**纯 C11 libnuma 仿真环境**之上：它把"NUMA 节
点"建模成可配置的拓扑参数（容量/延迟/带宽/距离/压力），默认给一个 `dram0 + cxl1`
的两层模板；`nf_numa_access_cost`/`nf_numa_migrate_cost` 是解析式的访问代价模型
——本地访问只计延迟，远端访问按 NUMA 距离放大延迟、按带宽折算传输时间。这意味着
**任何一台机器（包括本项目实际开发用的、只有一块物理 NUMA 节点的机器，甚至一台
16GB 的 Windows 笔记本）都能用完全确定性的方式模拟异构内存分层**，从而在没有真
实硬件的前提下，仍然公平地评测调度策略。

想让这套仿真更贴近某个具体的 CXL 器件而非通用假设时，`numaflow eval` 支持
`--cxl-latency-ns <n>` 和 `--cxl-bandwidth-mbps <n>`，用真实 CXLMemSim 设备级仿真
测出来的数字替换 `numa_shim.c` 的合成默认值（见 [第 7 章](#第-7-章测试与验证体系)
中 CXLMemSim 的部分）。

```bash
cd numaflow && make && make report   # 生成 results/report.html
```

### 5.6 TUI / GUI / 追踪反馈 / 自适应 DAG

- **TUI**（`./build/nf_tui`）：命令行交互界面，可以列出全部原子操作/策略，用原子
  操作手工拼出自定义工作流，保存/加载为 JSON，跑评测，甚至创建**周期性的内存调度
  任务**并逐 tick 打印追踪反馈分——适合在终端里快速试验一个新想法。
- **GUI**（`python gui/server.py`，浏览器打开 `http://127.0.0.1:8090`）：n8n 风
  格的可视化编辑器，拖拽原子操作节点、连线成 DAG、编辑参数、导入/导出/运行工作
  流；后端是一个 Python HTTP 服务，实际执行时调用编译好的 `numaflow` C11 二进
  制，而不是自己重新实现一遍引擎逻辑。
- **追踪反馈**（`nf_track.c`）：用固定内存实现 Count-Min Sketch + Doorkeeper（和
  第 3.4 节 TinyLFU 用的是同一套设计），加上一个滑动窗口反馈：EWMA 命中率 + 归一
  化代价 → 汇总成单一的 `feedback_score`，供自适应策略在线调参用。
- **自适应 DAG**（`nf_adapt.c`）：根据每次运行的反馈，既能**微调参数**（比如
  `filter_benefit.threshold`、`demote_cold.threshold`、`budget_limit.budget`——用
  简单的爬山法：反馈变好就保持调整方向，变差就反向），也能**切换整个结构**——
  根据"DRAM 驻留率 + 迁移抖动率"在三个模板间切换：`conservative`（只晋升高收益
  key，抑制抖动）/`balanced`（就是 CAAT，晋升+降级）/`aggressive`（晋升+降级+再
  平衡，DRAM 驻留率低时更激进地腾位置）。

### 5.7 与 Redis 的桥接：`NUMA FLOW` 命令

`nf_bridge.c`（同样纯 C11，可独立测试）定义了引擎和"任意键值存储"之间的最小契
约：宿主只需要实现两个回调——`enumerate`（把 keyspace 逐条产出为引擎能理解的
`nf_item_t`）和 `apply`（把某个 key 真正迁移到目标节点）。`src/numa_flow.c` 就是
Redis 侧的薄胶水层，实现了这两个回调：`enumerate` 用
`numa_get_key_current_node`/`numa_get_key_metadata` 填充 item；`apply` 调用
`numa_migrate_key_by_name`；引擎里 `emit_migrate` 产生的决策（`current_node`
字段变化）被这层胶水翻译成一次真实的 key 迁移调用。

```text
NUMA FLOW LOAD <name> <path.json> [interval_sec] [ADAPT]   # 加载 GUI 导出的工作流
NUMA FLOW RUN  [name]                                       # 立即执行（省略 name 则全部执行）
NUMA FLOW LIST / STATUS <name>                               # 查看运行状态 / 反馈分
NUMA FLOW UNLOAD <name>                                      # 卸载
NUMA FLOW ADAPT <name> <ON|OFF>                              # 开关自适应
NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>          # 运行时切换默认策略
```

加载的工作流会被 `serverCron` 按 `interval_sec` 周期自动执行——也就是说，你完全可
以在 GUI 里拼一个 CAAT 之外的新策略，导出 JSON，`NUMA FLOW LOAD` 进一个正在运行
的 Redis 实例，不需要重新编译内核。Redis 启动时还会自动做一件事：按
`numa-flow-default-strategy`（默认 `caat`）把对应的预设注册成一个名为 `default`
的工作流条目并开始跑——所以一个刚启动、什么都没手动 `LOAD` 过的 Redis 实例，也已
经在按 CAAT 迁移数据了，不是一个空转的桥接层。

这次把三个策略收敛进 NUMAflow 之后，跑一遍针对真实桥接代码的全策略回归测试
（不只是命令层的 smoke test）还揭出了两个从 ADR-08 之前就存在、但从未被真实触发过
的 bug——完整技术叙述见
[ADR-09](../new/09-architecture-decisions.md)，这里只留一句话摘要：`nf_ops.c` 的
`cms_estimate` 读的 CMS 频率信号，此前在真实 Redis 桥接路径上从来没被写入过（写入
口 `nf_tracker_observe()` 只有 `nf_bench.c` 自己的评测 harness 调用过），修复方式
是新增 `numa_flow_observe_access()`，从 `db.c` 里和 `numa_key_migrate_touch()` 同
一条真实访问路径调用；另外 `build_caat` 的旧单链设计会让"已经执行过降级、但没通
过晋升过滤"的 item 在到达 DAG 终止节点之前被丢弃，导致降级明明发生了却从未被桥接
层看到、从未被真正应用，修复方式是让图在两个阶段变更之前先按原始驻留位置分叉。

---

## 第 6 章：一次真实的版本迁移：Redis 6.2.21 → 7.2.6

这一章值得所有学生认真读一遍，因为它是一个关于**版本合并/代码审查方法论**的真实
案例，结论不止适用于 Redis：

> **"合并没有冲突标记"和"合并是正确的"是两件完全不同的事。**

### 6.1 为什么选 7.2.6 而不是更新的 8.x

7.2.6 是 Redis 在许可证从 BSD-3-Clause 改为 RSALv2/SSPL 双许可（7.4 版本开始）**之
前**的最后一个稳定版本，同时已经包含了会影响本项目 NUMA 模块的全部关键 API 变化
（`dictEntry` 变为不透明、listpack、quicklist 容器类型）。选它既能拿到需要的 API
变化，又不必处理更大的 8.x 重构，还保持在本项目一直使用的 BSD-3-Clause 许可证下。

### 6.2 为什么不能直接 `git merge`

本仓库当初是作为一份**全新的历史**导入的（`git log` 只有一个根提交"first
commit"），跟 `redis/redis` 官方仓库**没有共同的提交历史**。直接
`git merge upstream/7.2.6` 会被 Git 当成两棵完全不相关的树，对比结果是漫天的
"整文件冲突"，而不是一次真正的三方合并。

解决办法：先拉取上游的 `6.2.21` 和 `7.2.6` 两个 tag，然后用
`git replace --graft <本仓库根提交> <上游 6.2.21 提交>`，给本仓库的根提交"移植"
一个假的父提交——让它指向上游的 6.2.21。这样 Git 就能正确计算出合并基点（上游
6.2.21），在"我们的 6.2.21 + NUMA 模块"和"上游 7.2.6"之间做一次真正的三方合
并，而不是从零对拍。

### 6.3 核心教训：合并干净 ≠ 合并正确

`git merge` 的 recursive 策略，只要能算出**一个**结果就不会留下 `<<<<<<<` 冲突标
记——但"没有冲突标记"只代表"没有*无法自动判断*的冲突"，不代表"自动判断的结果是
对的"。这次合并里，**6 个真实 bug 在零冲突标记的情况下混进了代码**，对 `grep`
冲突标记这种检查方式完全不可见：

| # | 文件 | 具体问题 | 症状 |
| --- | --- | --- | --- |
| 1 | `src/zmalloc.c` | 一个包住 `PREFIX_SIZE` 的 `#ifdef HAVE_NUMA` 块，闭合的 `#endif` 丢了 | 编译错误，后面所有符号全炸 |
| 2 | `src/dict.c` | 一段多余的重复溢出检查，引用了一个在 `dictht`→`ht_table[]` 结构调整后已经不存在的变量 `realsize` | 编译错误：未声明的标识符 |
| 3 | `src/server.c` | `afterCommand()` 的 6.2.21 版本和 7.2.6 版本**两份函数体都被保留了** | 编译错误：重复定义 |
| 4 | `src/networking.c` | `addReplyBigNum()` 和 `deferredAfterErrorReply()` 同样两份函数体都被保留 | 编译错误：重复定义 |
| 5 | `src/server.h` | `objectComputeSize()` 的旧 2 参数原型，在 `.c` 里的定义已经改成 4 参数之后，仍然留在头文件里没更新 | 没有编译报错——是隐式声明，直到 `evict.c`/`evict_numa.c` 里按旧原型调用，在 `integration/replication-buffer.tcl` 测试中触发 SIGSEGV |
| 6 | `src/server.c` | `call()` 函数里，6.2.21 那个无条件调用 `replicationFeedMonitors()` 的语句，和 7.2.6 那个受 flag 保护的调用，**两句都被保留了** | 不崩溃，但 MONITOR 客户端会看到每条命令被喂两次（`unit/introspection.tcl` 测试失败） |

这 6 个问题没有一个留下冲突标记，全部是靠**真正跑完整的 `make -j$(nproc)` 编译 +
跑真实测试套件**才被发现的——只 grep 冲突标记、或者"读一遍 diff 觉得看起来合
理"，都不足以发现它们。**据此定下的规则：本仓库以后任何一次跨版本的非小合并之
后，都必须跑一次完整的 `make -j$(nproc)` 编译到底，把每一个编译错误都当成"可能
是合并静默损坏了代码"去查，而不是想当然地当成"这是正常的 API 迁移工作量"；区分
这两者的方法是把这段代码分别和"合并前的本项目提交"以及"真实的上游 tag 提交"各
diff 一遍，而不是只凭感觉猜。**

### 6.4 期望之内的真实迁移工作（不是 bug，是正常的 API 适配）

- **`dictCreate(dictType*)` 去掉了 `privdata` 参数**——`numa_key_migrate.c` 和
  `numa_composite_lru.c` 里每一处 `dictCreate(&sometype, NULL)` 改成
  `dictCreate(&sometype)`。
- **`dictEntry` 变成完全不透明的类型**——原来直接手动遍历
  `d->ht[t].table[i]` 的代码（在 `numa_object_sample_alloc_ptr`/
  `numa_object_sample_alloc_size` 里）改写成用
  `dictGetIterator`/`dictNext`/`dictGetKey`/`dictGetVal`/`dictReleaseIterator`，
  统计内存用 `dictEntryMemUsage()` 和 `dictSlots(d)`，而不是
  `sizeof(dictEntry)` 加手算槎位数。
- **`dictType` 的回调**（`keyCompare`/`keyDup`/`keyDestructor`/
  `valDestructor`）第一个参数从 `void *privdata` 变成 `dict *d`——两个模块的回调
  表都要同步改。
- **`dictGenHashFunction`** 的长度参数从 `int` 变成 `size_t`——顺手删掉了
  `numa_tinylfu.c` 里一处冗余、且已经用旧签名"遮蔽"了 `dict.h` 正确原型的
  `extern` 重复声明。
- **`quicklistNode->zl` 改名为 `->entry`**——`numa_key_migrate.c` 里 LIST 类型的
  迁移适配器整段跟着改名。
- **hash/zset 编码**：`OBJ_ENCODING_ZIPLIST` 为了 RDB 向后兼容仍然保留，但新对象
  用 `OBJ_ENCODING_LISTPACK`。在 `migrate_hash_type()`/`migrate_zset_type()` 里给
  ziplist 分支旁边加了 listpack 分支（用 `lpBytes()`）——好在迁移本身两种编码都
  是"整块 `memcpy`"，不需要逐条遍历，风险比预想的低。
- **`src/commands/numa.json`**：用 Redis 7 的声明式命令自省系统（`COMMAND INFO`/
  `COMMAND DOCS`）注册 `NUMA` 命令，并重新生成了 `src/commands.def`。

### 6.5 一个被合并"暴露"、但不是合并造成的历史 bug

`numa_init()`（负责初始化 `zmalloc.c` 里的 slab/direct-cache 分配器状态）只在
`server.c` 的 `main()` 里被调用过。但每一个链接了 `zmalloc.o` 的其它二进制
（`redis-cli`、`redis-benchmark`、`redis-check-rdb`/`aof`、`redis-sentinel`）**从
来没有调用过它**，所以这些进程整个生命周期里 `numa_ctx.numa_available` 都是 0。
`zmalloc()`/`zcalloc()`/`zrealloc()` 本身已经对这个 flag 做了判断，退化成普通
`malloc()`——但 `dict.c` 用的 `zmalloc_local()`/`zcalloc_local()`/
`ztrycalloc_local()` 没有做同样的判断，会**无条件**调用
`numa_alloc_dram()`，在从未初始化过的全局状态上运行 slab 分配器逻辑，用一种"当
下不炸、之后在一次完全不相关的 `free()` 里炸"的方式损坏堆。

这个 bug 在合并之前的 6.2.21 版本 `dict.c`/`zmalloc.c` 里其实**一直存在**——只是
7.x 之前的测试套件从没有恰好触发过 `redis-cli` 的 dict/迭代器代码路径。Redis 7 新
增的 `tests/unit/cluster/cli.tcl`（会跑 `redis-cli --cluster create`，内部会建一
个反亲和性打分用的 dict）第一次踩中了它，表现为 `redis-cli` 在 `zfree()` 内部
SIGSEGV。修复方式是给 `numa_alloc_dram()` 补上和 `zmalloc()` 一样的
`numa_ctx.numa_available` 判断 + 普通 `malloc` 退化路径。

**这条教训延伸出的规则**：一个"NUMA-aware"的分叉项目，需要审查**每一个**二进制入
口点的分配器初始化时序，不能只看 `redis-server` 的 `main()`。这一类 bug 在其它
进程恰好触发相关代码路径之前是完全隐形的。

### 6.6 一个必须单独 cherry-pick 的安全修复

选定 `7.2.6` 作为合并基点，**不等于**选到了一个完全打齐安全补丁的基线。
`tests/unit/hyperloglog.tcl` 里"Corrupted sparse HyperLogLogs ... XZERO opcode"
这条测试，在完全没有改过、和上游逐字节相同的 `src/hyperloglog.c` 里让
`hllMerge()` 崩溃——这正是
[CVE-2025-32023](https://github.com/redis/redis/security/advisories)（一个
HyperLogLog 越界写漏洞），上游修复提交是 `f35b72dd1`，时间上**晚于** `7.2.6` 这个
tag。这个提交被干净地 cherry-pick 到了 `hyperloglog.c` 上；唯一的冲突出在对应的
`.tcl` 测试文件本身——7.2.6 自带的测试套件里已经有一条措辞不同、但逻辑上等价的回
归测试，解决方式是保留现有措辞。

**教训**：任何合并基点选定之后，都要针对合并拉进来的具体文件单独核查已知 CVE
——这和"diff 出 NUMA 相关改动"是两件独立的事，缺一不可。

### 6.7 两个靠编译器警告（不是报错）抓到的配置注册 bug

- `src/config.c`：`numa-demote-min-size` 用 `createIntConfig()` 注册，但它在
  `server.h` 里对应的字段实际类型是 `size_t`——当 `CONFIG SET` 设置接近或超过
  `INT_MAX` 的值时会静默截断。修复方式是换成 `createSizeTConfig()`。
- 上面这个问题，加上前面提到的 `dictType` 回调签名不匹配问题，都是靠**重新 grep
  完整的编译日志**（而不是只看 `head -30`）才被发现的——用的关键词是
  `implicit declaration|incompatible pointer|too few arguments|too many
  arguments`。**教训：一定要 grep 完整的编译日志，不要只看开头几十行**——一次编
  译在报出第一个 error 之前，完全可能已经打印了远超过 30 行值得关注的 warning。

### 6.8 最终验证

- `cd src && make clean && make -j$(nproc)`：零错误，只剩下和这次迁移无关的既有
  警告；
- `make test`（完整 Tcl 套件）：最终一次运行 **91/91 全部通过**（在此之前的四次
  运行分别抓出并修复了上面 6 个 bug 里的一个）；
- `numaflow` 自己的测试套件全程保持绿色——因为它跟 Redis/libnuma 完全无关，正好
  可以当一个"对照组"：如果它坏了，说明问题出在 NUMAflow 自身，跟这次核心合并无
  关；
- 手工功能烟雾测试：`NUMA` 命令及其三个子命令（`CONFIG`/`STRATEGY`/`MIGRATE`）、
  跨全部 5 种数据类型的驱逐压力测试、一次真实的 3 节点
  `redis-cli --cluster create`；
- `tests/vm/boot_numa_vm.sh`：在一台 2-NUMA-节点的 QEMU 客户机（本机没有
  `/dev/kvm`，纯软件 TCG 模拟）里跑通 `redis-server` + `NUMA` 命令族 +
  `redis-benchmark`。

### 6.9 被删除的内容

- `src/redis8_compat.h`——死代码：整个仓库没有任何文件 `#include` 它，说明当初真
  正的迁移路径直接走的是 Redis 7 的真实 API（如本章所述），而不是通过一个兼容层；
- `docs/redis8-migration.md`——被本章所依据的 `docs/redis7-migration.md` 取代，
  原文档是一份在真正尝试合并**之前**写的纸面设计，其中好几个假设在真正合并之后
  被证明是错的。

---

## 第 7 章：测试与验证体系

### 7.1 总览

```bash
./run_full_validation.sh --quick     # 编译 + make test + NUMAflow 基准
./run_full_validation.sh             # 以上 + YCSB（若有 java） + QEMU 冒烟 + CXLMemSim 校验
```

每次运行都会生成 `results/full_report_<timestamp>/index.html` 和每一步的原始日
志/JSON。**任何在当前环境跑不了的步骤都记录为 `skipped` 并写明原因，绝不伪造结
果**——这条原则贯穿全部脚本，是理解这一章的关键：下面出现的"跳过"不是偷懒，而
是这个项目对"诚实报告"的一贯选择。

可用开关：`--skip-build`、`--skip-test`、`--skip-ycsb`、`--skip-vm`、
`--skip-cxlmemsim`、`--vm-timeout SECONDS`；`--quick` 等价于
`--skip-ycsb --skip-vm --skip-cxlmemsim`。

### 7.2 六个测试层级

**① 标准 Redis Tcl 套件**

```bash
cd src && make clean && make -j$(nproc)
cd .. && make test
```

跑 `tests/unit/*.tcl` 全部用例——[第 6 章](#第-6-章一次真实的版本迁移redis-6221--726)
提到的全部 6 个静默合并 bug 和那 1 个历史遗留 bug，都是在这一层被真正抓到的。这
一层出现任何回归都应该当作严重信号，不是噪音。

**② NUMA 环境脚本**

```bash
./utils/numa/check_numa_config.sh
./utils/numa/diagnose_numa.sh
```

对照真实主机 NUMA 拓扑做的快速健全性检查，即使主机只有 1 个物理节点也能正常跑
（会优雅降级，不会报错）。

**③ NUMAflow 子系统**（纯 C11，不依赖 Redis）

```bash
cd numaflow && make && make test && make report
```

`make test` 跑单元/冒烟/桥接-自适应/分配器测试；`make report` 跑第 5.5 节的公平评
测框架，覆盖 `zipf`/`uniform`/`hotspot`/`temporal` 四种负载，重新生成
`results/bench_*.json` + `results/report.html`（纯 stdlib SVG 条形图，不依赖
matplotlib，比较 noop/composite_lru/tinylfu/CAAT 的净代价和本地命中率）。

**④ YCSB 带宽基准**

```bash
cd tests/ycsb && ./run_bw_benchmark.sh    # 三阶段：Fill → Hotspot → Sustain
cd tests/ycsb && ./run_ycsb.sh            # baseline/stress 模式
```

需要 JDK 和 YCSB 发行包。

> **本次更新**：此前的验证会话里，这一层因为沙箱环境没有安装 JDK 而被跳过。本次
> 已在沙箱里补装 JDK 并实际跑通/或明确记录了新的失败原因——具体结果见本节末尾的
> 「最新验证结果」，以及 `docs/test/` 下对应的原始记录文件。

**⑤ QEMU 多 NUMA 节点冒烟测试**

```bash
./tests/vm/boot_numa_vm.sh [--timeout SECONDS] [--keep] [--skip]
```

启动一个带 2 个模拟 NUMA 节点的 Debian 12 云镜像（通过
`-object memory-backend-ram` + `-numa node,memdev=...`），带一个诚实的超时等待
SSH（默认 480 秒——本机没有 `/dev/kvm`，是纯 TCG 软件模拟，启动本来就慢），把本
地编译好的 `redis-server`/`redis-cli`/`redis-benchmark` 拷进去，在客户机里跑
`PING`/`SET`/`GET`、`NUMA CONFIG GET`、`NUMA FLOW LIST` 和一次
`redis-benchmark`。检查 NUMA 节点数直接读 `/sys/devices/system/node/`，不依赖
`numactl`（云镜像默认没装这个包，装它要走一次在 TCG 慢速 NAT 下可能耗时数分钟的
`apt-get`，性价比不高）。如果客户机没能在超时内通过 SSH，脚本会记下串口最后输出，
写一条 `"timeout"` 状态到 JSON 结果文件，然后正常退出（一个跑得慢/跑不起来的
QEMU 环境不算 NUMA 代码本身的 bug）。

**⑤b 真实双 NUMA 节点验证：放置质量 + 相对性能基准**

1 节点的开发机永远测不到真正的迁移*执行*路径（那里 `migrations` 恒为
0——决策做出来了，但没有第二个节点可以真的搬过去），上面的冒烟测试也没有跑够
久、跑够真实的迁移策略去验证放置效果。下面两个工具在真实的 ≥2 节点拓扑上补上
这一环，需要先用 `--keep` 保持 guest 运行：

```bash
./tests/vm/boot_numa_vm.sh --keep --timeout 600

# 放置质量（在 guest 内按策略运行）：热 key 是否留在本地、冷 key 是否被挪走。
# 第一次这样跑的时候就测出并修复了真实迁移执行路径里两个此前零覆盖的 bug
# （SDS key 查找、tick/recency 截断）——见 docs/new/09-architecture-decisions.md
# 的 ADR-11。
ssh -p 10222 -i tests/vm/.cache/vm_test_key numatest@127.0.0.1 \
    './placement_quality.sh caat'

# 相对性能基准（在开发机上运行，通过 SSH 编排 guest 里的全部四个策略）：
# 采集一份真实的按 key 放置轨迹，喂进 NUMAflow 标定过的代价模型
# （`numaflow replay`），算出一个*建模*的相对 ns 级投影——不是实测延迟，因为
# QEMU 的两个 -numa node 背后是同一块宿主机 DRAM。见 ADR-12。
./tests/vm/relative_perf_bench.sh
```

**⑥ CXLMemSim 设备级链路校验**

```bash
./tests/cxl/run_cxlmemsim.sh [--timeout SECONDS] [--skip]
```

需要提前 clone `external/CXLMemSim`（`SlugLab/CXLMemSim`）并构建好它自带的改版
QEMU 和 `cxlmemsim_server`。三项各自独立、各自可优雅降级的检查：

1. CXLMemSim 自己的 CTest 套件；
2. `tests/cxl/cxlmemsim_workload_bench.cpp`——直接调用 CXLMemSim 自己的
   `CXLMemExpander` C++ 模型（而不是 NUMAflow 简化的单一延迟/带宽模型），重放
   `zipf`/`uniform`/`hotspot`/`temporal` 同一批轨迹，写出
   `tests/cxl/results/cxlmemsim_native_bench_<ts>.json`；
3. 启动改版 QEMU，把一个 CXL Type2 端点接到 `cxlmemsim_server`（TCP），确认设备
   真的完成了连接（在 QEMU 日志里检查 `"Device realized"` 和
   `"Connected to CXLMemSim"`）——不需要跑完整客户机操作系统，QEMU 在设备连接完
   成后立刻用 `-S` 暂停。

三项都通过时的真实输出大致是这样（数字来自一次实际运行，不是虚构示例）：

```text
$ ./tests/cxl/run_cxlmemsim.sh
[1/3] CXLMemSim CTest 套件 ......... 22/22 通过（协议 / 一致性 / MESI 单测）

[2/3] 设备级链路 ...................
  QEMU 日志: "Device realized - Cache: 16 MB, DevMem: 64 MB"
  QEMU 日志: "Connected to CXLMemSim"
  server 侧拓扑: capacity=256GB read_bw=25GB/s write_bw=25GB/s
                 read_lat=100ns write_lat=150ns

[3/3] 原生工作负载 bench（cxlmemsim_workload_bench）:
  workload   avg_latency_ns   bandwidth_penalty_ns
  zipf       98.5             0.0766
  uniform    104.0            0.0315
  hotspot    98.7             0.0732
  temporal   117.7            0.0124
  (DRAM baseline: 60.0 ns，供对照)

status: "passed" — "ctest=passed, device link=passed, native bench=passed"
```

注意四种工作负载的延迟/带宽惩罚数字并不相同（`zipf`/`hotspot` 这类长尾/热点分布
的带宽惩罚明显高于 `uniform`），这正是 5.5 节和下面坑①要求的"模型必须对工作负载
形状敏感"在真实运行中的体现。**但要把这一层校验的范围看清楚**：它验证的是
CXLMemSim 这个时序仿真器本身、以及它和改版 QEMU 的设备级集成是否work——不是"redis-server
真的跑在了这块模拟 CXL 内存上"。后者是本指南 7.3 节单独尝试、并诚实记录为部分成
功的另一件事，二者不冲突，只是范围不同。

调这个 bench 时值得记住两个 CXLMemSim 自身的坑（不是本项目引入的，本项目选择绕
过而不是去 patch 一个没被 vendor 进本仓库历史的外部依赖）：

- 不先对每次访问调用 `CXLMemExpander::insert()` 就直接跑
  `calculate_latency`/`calculate_bandwidth`，会让模型对工作负载的"形状"完全不敏
  感（四种负载跑出完全相同的输出，这是第一版实现时被实测发现的）——`insert()`
  才是把"第一次访问某地址"分类成 store、"重复访问"分类成 load 的地方，而这个
  load/store 比例正是 `calculate_bandwidth()` 拥塞模型真正读取的输入；
- `calculate_latency()` 内部先调用 `update_address_cache()`，这一步会设置
  `is_address_local()` 检查的同一个 `cache_valid` 标志，却**不会**填充
  `address_ranges` 这个真正被读取的数组——在一个全新的 endpoint 上，先调用
  `calculate_latency()` 会让两个计算函数之后都静默返回 0。规避方式是先调用
  `calculate_bandwidth()`。

**⑦（对比参照）真实 Redis 层 DRAM-vs-远端内存对比**

上面第⑥项验证的是"设备仿真链路能不能连通"，不是"redis-server 在 CXLMemSim 客户
机里跑起来是什么表现"。要看真实 Redis 层面的对比，用
`tests/ycsb/scripts/eval_cxl_memory.sh`——它用 `numactl --membind` 在**真实的、
至少 2 个 NUMA 节点**的环境（比如第⑤项的 QEMU 客户机）里跑，而不依赖
CXLMemSim。本项目实际开发用的这台主机只有 1 个物理 NUMA 节点，所以这一项的数字
在本机跑不出多节点意义下的结果。

### 7.3 本次新增的验证：redis-server 跑在 CXLMemSim 客户机里

在这份指南写作的同一次会话里，专门尝试了此前明确标注为"未尝试/超出范围"的一
项：让 `redis-server` 真正跑在一个挂了 CXLMemSim CXL Type2 设备的 QEMU 客户机
里，而不只是验证设备链路。**结果是部分成功——比"完全没试过"走得远得多，但最终
没能让 redis-server 真正碰到 CXL 仿真内存**，原因诚实记录如下：

- 用 CXLMemSim 自带的改版 QEMU（`-M q35,cxl=on`，挂 `pxb-cxl`/`cxl-rp`/
  `cxl-type2` 设备链，连接一个正在运行的 `cxlmemsim_server`）启动了
  `tests/vm/boot_numa_vm.sh` 同款的 Debian 12 云镜像客户机，纯 TCG 软件模拟（本机
  没有 `/dev/kvm`），约 30 秒内启动完成、SSH 可用；
- 客户机内核（`6.1.0-52-cloud-amd64`，发行版自带，未经任何修改）本身就带了
  `cxl_pci`/`cxl_acpi`/`cxl_mem` 内核模块，`lspci` 也正确识别出设备：
  `0d:00.0 CXL [0502]: Intel Corporation Device [8086:0d92]`；
- **卡在这一步**：这块设备从来没有真正出现在 `/sys/bus/cxl/devices/` 下的内存端
  点（只有 `root0`/`port1`/`decoder1.0`，没有 `mem0`/`endpoint0`）。手动执行
  `echo 0000:0d:00.0 > .../drivers/cxl_pci/bind` 会直接返回一个 I/O 错误，
  dmesg 里连一条日志都没留下——是一次静默的探测失败。追根溯源，CXLMemSim 自己的
  `qemu_integration/launch_qemu_vcs_dcd_gfam.sh` 脚本给出了答案：它默认使用的
  `KERNEL_IMAGE` 是 `/root/linux-cxl-type2/arch/x86/boot/bzImage`——**CXLMemSim
  作者自己维护的一份打了专用补丁的 Linux 内核**。发行版自带的标准 `cxl_pci`
  驱动无法绑定这个 `cxl-type2` 设备，是因为它没有实现标准驱动所依赖的 DVSEC
  register-locator 能力——这不是配置错误，是这个模拟设备本身就要求一个专门修改
  过的内核才能把它的内存正确暴露成客户机可见的 RAM/NUMA 容量。

这份专用内核在本沙箱环境里完全不存在，从头构建它（clone Linux 源码、打上
CXLMemSim 的补丁、编译出一个 bzImage）本身是另一项数小时量级的独立工作，本轮判
断为超出验证范围——这也和 `run_cxlmemsim.sh` 脚本此前注释里已经标注的判断一致。
**结论：本次没有让 `redis-server` 真正访问到 CXL 仿真内存**，因为在没有那个专用
内核的前提下，这块设备的内存根本没有路径暴露成客户机内存/NUMA 容量。具体的 PCI
设备识别信息和绑定失败证据已经写进 `ARCHITECTURE.md`「External validation
layers」一节和 `tests/cxl/run_cxlmemsim.sh` 的头部注释，替换了原来"从未尝试"的
说法。

### 7.4 本次新增的验证：YCSB 带宽基准

此前的验证会话里，这一层因为沙箱环境没有安装 JDK 而被跳过。本次补装了一份**便携
式 JDK 17**（Temurin，解压到 `~/.local/opt/jdk-17`）和便携式 Maven，都**不需要
root**——沙箱里没有可用的 sudo/TTY，这也是给同样处境的学生的一个实用提示：装不了
系统级 JDK 时，直接解压官方 tarball 到用户目录、把 `bin/` 加进 `PATH` 一样能用。

跑的过程中还踩了两个和"本机只有 1 个 NUMA 节点"相关的坑，一并记录下来：

- `run_bw_benchmark.sh` 默认会执行 `numactl --cpunodebind=0,2 --membind=0,2`
  （把 Redis 进程绑定到节点 0 和 2），但本机 `numactl --hardware` 只有一个节点
  （`available: 1 nodes (0)`），直接报错退出。脚本本身就提供了
  `--process-nodes all` 参数来取消这个绑定，加上就能正常跑——**这是任何单
  NUMA 节点开发机上跑这个脚本都会遇到的同一个坑，不是本次沙箱独有**。
- YCSB 0.17.0 官方发行包里 `bin/ycsb`（Python 命令行入口）是 Python 2 语法
  （`except Foo, err:`），本机只有 python3，会直接语法错误退出；`bin/ycsb.sh`
  （Java 入口，命令行参数完全一致）不受影响——`run_bw_benchmark.sh` 自己的注释
  里其实已经提到过这同一个兼容性问题、并主动改用了 `ycsb.sh`。解决方式是把解压
  出来的 `bin/ycsb`（属于第三方发行包，不受本仓库版本控制）换成一个转发到
  `ycsb.sh` 的一行 shim 脚本。

三组基准全部跑通、数字均为实测（非仿真）：

| 测试 | 阶段 | 吞吐 (ops/sec) |
| --- | --- | --- |
| `run_bw_benchmark.sh --process-nodes all` | Phase 1 Fill | 9,688 |
| | Phase 2 Hotspot Migration | 120,602 |
| | Phase 3 Sustained Pressure | 130,370 |
| `run_ycsb.sh --mode baseline`（10 万条记录，4 线程） | Load | 34,282 |
| | Run（READ 均延 50.0us / UPDATE 均延 52.6us） | 73,314 |
| `run_ycsb.sh --mode stress`（100 万条记录，32 线程） | Load | 53,743 |
| | Run（READ 均延 212.0us / UPDATE 均延 225.3us） | 142,005 |

完整原始输出见 `docs/test/benchmark_results.txt` 第 9 节，以及
`tests/ycsb/results/{bw_bench_20260819_144153,baseline_20260819_151756,
stress_20260819_151847}/`。一个直观的读法：高并发（stress 的 32 线程）换来更高
吞吐，但尾延迟同步变大，是典型的吞吐-延迟权衡；`Phase 1 Fill` 吞吐明显低于
Phase 2/3，是因为填充阶段是纯写入、此时迁移/热度追踪还没有建立起有效的候选
集合，谈不上"命中缓存"。

### 7.5 手动功能烟雾测试

日常改一个模块、不想跑全套时很有用：

```bash
./src/redis-server ./redis.conf --daemonize yes --logfile /tmp/r.log
./src/redis-cli numa config get
./src/redis-cli numa flow list
./src/redis-cli numa migrate stats
./src/redis-benchmark -q -n 20000 -c 20 -t set,get
./src/redis-cli --cluster create 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003 \
    --cluster-replicas 0   # 顺带练到 redis-cli 自己的 dict/迭代器代码路径，
                            # 不只是 redis-server
```

---

## 第 8 章：性能数据一览

| 场景 | 数据来源 | 关键结论 |
| --- | --- | --- |
| NUMAflow 公平评测（zipf/3000 key/12万次访问） | `numaflow/eval/report.py` | CAAT 本地命中率 91.0%，净代价比 TinyLFU 低约 20%，比 Composite LRU 低约 37%——⚠️ 这是 zipf 单一工作负载的数字；ADR-09 用修复后的二进制在四种工作负载上复测后发现 CAAT 的优势是工作负载依赖的，在 uniform 负载上净代价反而比 Composite LRU 高约 31%（详见 5.4 节、[ADR-09](../new/09-architecture-decisions.md)） |
| 独立内存分配器 `nf_alloc` | `docs/numaflow/allocator.md` | 单线程吞吐约为系统 `malloc` 的 1.85×，内部碎片 3.82%，省掉 16B 前缀开销 |
| CXLMemSim 真实时序校准 | `run_full_validation.sh` 的 NUMAflow 步骤 | 用真实 CXLMemSim 测得的延迟/带宽（约 125ns / 25000MB/s）替换合成默认值后，CAAT 在全部 4 种工作负载上仍然领先 |
| Redis 内核编译 + 单测 | `make -j$(nproc)` / `make test` | 零错误编译，91/91 测试通过 |

（YCSB 与 CXLMemSim 客户机内 redis-server 的实测数据见第 7.3、7.4 节。）

---

## 第 9 章：常见坑与 FAQ

**Q：为什么不能用 jemalloc？**
A：本项目的分配器直接接管了 `zmalloc` 层，如果同时启用 jemalloc，两套分配器会争
抢同一段内存的元数据，导致堆损坏。`src/Makefile` 已经强制 `MALLOC=libc`；如果你
修改编译选项，一定不要引回 jemalloc（见 [3.1](#31-numa_pool自定义分配器)、
CLAUDE.md 的 Key Gotchas）。

**Q：为什么新模块的初始化顺序很重要？**
A：`numa_init()` 必须在 `initServer()` **之前**跑，而策略/按键迁移/带宽监控这些
模块的初始化必须在 `initServer()` **之后**跑，因为它们依赖 `initServer()` 建好
的状态。顺序颠倒是这个项目里最常见的启动期崩溃原因（见
[CONTRIBUTING.md](../CONTRIBUTING.md)）。

**Q：为什么 NUMA 模块里不能直接调用 `serverLog()`？**
A：Redis 内部约定，这类模块要用 `extern void _serverLog(int level, const char
*fmt, ...)`，这是仓库既有的惯例（参考 `numa_configurable_strategy.c`、
`numa_bw_monitor.c` 的写法）。

**Q："合并没有冲突标记"是不是就说明合并没问题？**
A：不是。见 [第 6 章](#第-6-章一次真实的版本迁移redis-6221--726)——6 个真实 bug
在零冲突标记的情况下混入了 6.2.21→7.2.6 的合并，只有真正跑完整编译 + 完整测试套
件才抓得到。

**Q：为什么 `ADAPTIVE`/`LATENCY_AWARE` 两个分配策略在内核里"看起来没做什么"？**
A：它们在内核里确实是占位实现——完整版本刻意放在了 NUMAflow 的
`alloc_adaptive`/`alloc_latency_aware` 原子操作里，通过 `NUMA FLOW` 桥接进来，而
不是直接写进内核关键路径（见 [3.5](#35-numa_configurable_strategy7-种分配策略)）。

**Q：本机只有 1 个物理 NUMA 节点，怎么测试多节点场景？**
A：三条路径任选：① NUMAflow 的仿真拓扑（`numa_shim.c`，纯软件建模，见
[5.5](#55-公平评测框架没有真实多节点硬件也能可信地比较策略)）；② QEMU 多节点客
户机（`tests/vm/boot_numa_vm.sh`，跑真实 `redis-server`）；③ CXLMemSim 设备级仿
真（`tests/cxl/`，仿真具体的 CXL 器件时序）。

---

## 第 10 章：推荐学习路径

如果你是第一次接触本项目、准备从头系统地读一遍，建议按这个顺序：

1. **先跑起来**：按 [第 2 章](#第-2-章项目地图五分钟跑起来)编译、启动，敲几个
   `NUMA` 命令，对"这东西到底能做什么"建立直观感受。
2. **再理解全局**：读一遍 [第 1 章](#第-1-章为什么需要一个numa-aware的-redis)和
   [第 3 章](#第-3-章内核里改了什么八个模块逐一讲解)的开头（3.0 依赖顺序图），
   在脑子里搭好八个模块之间的关系骨架，**不要**急着去读每个模块的实现细节。
3. **顺着依赖顺序读源码**：`numa_pool.c` → `numa_migrate.c` →
   `numa_key_migrate.c` → `numa_bw_monitor.c` → `numa_configurable_strategy.c` →
   `numa_flow.c` → `numa_command.c` → `evict_numa.c`。每读完一个模块，回来对照
   本指南对应小节验证理解是否一致。
4. **读一遍第 6 章**，把它当成一次软件工程案例研究，而不是"和我要写的代码没关
   系的历史"——这一章教的合并审查方法论，几乎可以搬到任何跨版本升级、跨分支合
   并的场景。
5. **进入 NUMAflow**（第 5 章）：先读 5.2 的 36 个原子操作分类表，再读 5.3 把已
   有策略"翻译"成图，最后读 5.4 的 CAAT——这时候你已经有了"用图理解策略"的能
   力，CAAT 的推导会顺理成章。
6. **动手改一个小东西**：比如在 NUMAflow 的 TUI/GUI 里拼一个和 CAAT 不同的自定
   义工作流，跑一遍公平评测，看看数字怎么变——比通读代码更快建立直觉。
7. **最后**，如果要给内核加一个新模块，把 [CONTRIBUTING.md](../CONTRIBUTING.md)
   当 checklist 逐条对照着做，尤其是初始化顺序和 `_serverLog` 那两条——这是两个
   最容易踩、又最难在事后快速定位的坑。

延伸阅读：`docs/new/`（arc42 风格架构文档：12 个顶层章节 + 每个组件的
`modules/` 详情表）、`docs/numaflow/`（更细的
子系统文档）、`docs/redis7-migration.md`（第 6 章的完整原始记录）、
`docs/README.md`（全部文档的权威索引）。
</content>
