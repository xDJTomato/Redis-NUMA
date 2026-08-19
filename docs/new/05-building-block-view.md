# 5. 构件视图（Building Block View）

> arc42 第 5 章。这里只回答一个问题：**系统由哪些构件组成，每个构件负责什么，
> 细节在哪份文档里**——具体到某个构件的数据结构、算法、代码路径，请点进对应的
> `modules/*.md` 详情页，本章只给「地图」。

## 5.1 整体分解（Level 1 白盒视图）

```
┌───────────────────────────────────────────────────────────────────┐
│                         Redis Core（7.2.6）                        │
│   server.c / db.c / config.c / evict.c / zmalloc.c ...            │
└───────────────────────────┬───────────────────────────────────────┘
                             │ 三个接触点：zmalloc / server.c(main,cron) / evict.h
                             ▼
┌───────────────────────────────────────────────────────────────────┐
│                        NUMA 模块层（src/numa_*.c/h）                │
│                                                                     │
│  分配路径：      numa_pool ← zmalloc_numa 集成点                     │
│  迁移路径：      numa_migrate → numa_key_migrate                    │
│  调度路径：      numa_strategy_slots ─┬→ numa_composite_lru（默认）  │
│                                      ├→ numa_tinylfu（默认关闭）     │
│                                      └→ ae_strategy_scheduler（可选） │
│  分配策略：      numa_configurable_strategy（9 种）                  │
│  统一入口：      numa_command（NUMA MIGRATE/CONFIG/STRATEGY）        │
│  观测与淘汰：    numa_bw_monitor ──→ evict_numa                     │
└───────────────────────────┬───────────────────────┬───────────────┘
                             │ 桥接（numa_flow.c）    │
                             ▼                       ▼
                  ┌────────────────────┐   ┌──────────────────────┐
                  │  NUMAflow 子系统    │   │  物理 / 仿真 NUMA 拓扑  │
                  │  （numaflow/，      │   │  DRAM 节点 ←→ CXL/远端 │
                  │   独立姊妹构件）     │   │  节点                 │
                  └────────────────────┘   └──────────────────────┘
```

这是本章唯一需要记住的图：NUMA 模块层夹在「Redis 核心」和「物理/仿真的 NUMA
拓扑」之间，通过三个明确的接触点接入核心，通过一条桥接文件（`numa_flow.c`）可选
地接入姊妹子系统 NUMAflow。下面逐一展开每一块。

## 5.2 模块依赖顺序

十个模块不是平级关系，而是一条严格的依赖链——既是初始化顺序，也是
`src/Makefile` 里链接顺序的硬性要求（NUMA 的 `.o` 必须排在 `server.o` 之后）：

```
libnuma
  └─> numa_pool
       └─> numa_migrate
            └─> numa_key_migrate
                 └─> numa_composite_lru / numa_tinylfu / numa_strategy_slots
                      └─> numa_command
                           └─> evict_numa
                                └─> server.c（main 函数把它们串起来）
```

新增模块时打乱这个顺序（例如在 `numa_pool` 初始化之前调用迁移函数）是本仓库最
常见的启动期崩溃来源；完整原理见 [`08-crosscutting-concepts.md`](08-crosscutting-concepts.md)，
落地检查清单见 [`../../CONTRIBUTING.zh-CN.md`](../../CONTRIBUTING.zh-CN.md)。

## 5.3 十个 NUMA 模块（构件清单）

| 构件 | 源文件 | 职责一句话 | 详情页 |
| --- | --- | --- | --- |
| NUMA Slab 分配器 | `numa_pool.c/h` | 决定「一次分配的内存物理上落在哪个节点」，33 级尺寸类 + bump-pointer + slab + chunk 压缩 | [`modules/numa_pool.md`](modules/numa_pool.md) |
| 底层块迁移 | `numa_migrate.c/h` | 把一段内存从节点 A 搬到节点 B（`numa_alloc_onnode` + `memcpy`），不关心这块内存属于哪个 key | [`modules/numa_migrate.md`](modules/numa_migrate.md) |
| 按 key 迁移 | `numa_key_migrate.c/h` | 把 `numa_migrate` 的能力升级到「一个 Redis key（`robj`）」粒度，覆盖全部 5 种数据类型的全部编码；集成 LRU 热度追踪 + 惰性阶梯衰减 | [`modules/numa_key_migrate.md`](modules/numa_key_migrate.md) |
| 策略插槽框架 | `numa_strategy_slots.c/h` | 16 槎位、vtable 多态的可插拔策略框架，`serverCron` 或 AE 定时器驱动 | [`modules/numa_strategy_slots.md`](modules/numa_strategy_slots.md) |
| Composite LRU | `numa_composite_lru.c/h` | 默认迁移策略（槎位 1）：热候选环形缓冲区（快路径）+ 渐进式字典扫描（慢路径） | [`modules/numa_composite_lru.md`](modules/numa_composite_lru.md) |
| TinyLFU | `numa_tinylfu.c/h` | 频率驱动迁移策略（槎位 2，默认关闭）：Count-Min Sketch + Doorkeeper 布隆过滤器，固定 ~40KB 内存 | [`modules/numa_tinylfu.md`](modules/numa_tinylfu.md) |
| 可配置分配策略 | `numa_configurable_strategy.c/h` | 「新分配的内存第一次该放哪」的 9 种策略，作用在 `zmalloc` 层 | [`modules/numa_configurable_strategy.md`](modules/numa_configurable_strategy.md) |
| 统一命令接口 | `numa_command.c` + `commands/numa.json` | `NUMA MIGRATE` / `CONFIG` / `STRATEGY` 三域路由，Redis 7 声明式命令自省 | [`modules/numa_command.md`](modules/numa_command.md) |
| 带宽监控 | `numa_bw_monitor.c/h` | 实时每节点带宽占用（resctrl / numastat / manual 三种后端） | [`modules/numa_bw_monitor.md`](modules/numa_bw_monitor.md) |
| NUMA 感知淘汰 | `evict_numa.c/h` | 淘汰前先尝试降级：距离(40%)+压力(30%)+带宽(30%) 加权评分选目标节点 | [`modules/evict_numa.md`](modules/evict_numa.md) |

## 5.4 与 Redis 核心的接触点

以上十个模块不是凭空运行的，它们通过三处明确的钩子接入 Redis 核心，其中第一处
本身重要到值得有自己的详情页：

- **`zmalloc.c`/`zmalloc.h`** — 全部 `zmalloc`/`zfree`/`zrealloc` 路由检查、16 字
  节 `numa_alloc_prefix_t` 前缀、`NO_MALLOC_USABLE_SIZE`、以及 `redis-cli` 等从不
  调用 `numa_init()` 的二进制的退化保护。详情见
  [`modules/zmalloc_numa.md`](modules/zmalloc_numa.md)。
- **`server.c`** — `numa_init()` 在 `main()` 里、`initServer()` 之前调用；其余模块
  初始化在 `initServer()` 之后；周期性 compaction 与策略执行挂在 `serverCron`。
  这条初始化顺序约定属于「贯穿性设计概念」，见
  [`08-crosscutting-concepts.md`](08-crosscutting-concepts.md)，不单独设详情页。
- **`evict.h`/`evict.c`** — `evictionPoolEntry` 扩展的三个字段
  (`current_node`/`object_size`/`numa_migrated`)，属于 `evict_numa` 构件本身的接口，
  见 [`modules/evict_numa.md`](modules/evict_numa.md)。

## 5.5 可选调度增强：AE 时间事件调度

`numa_strategy_slots` 默认由 `serverCron`（每秒一次）驱动每个启用的策略槎位。这
不是唯一的调度方式——每个槎位可以单独切换为挂在 Redis 事件循环（`ae.c`）上的
**时间事件**驱动，带 deadline/budget/续跑语义，避免慢策略拖长 `serverCron` 一轮
的耗时。这是 `numa_strategy_slots` 构件的一个可选运行模式，不是独立模块，但因为
内容体量大（调度模型、状态机、分阶段落地过程）单独成页：
[`modules/ae_strategy_scheduler.md`](modules/ae_strategy_scheduler.md)。

## 5.6 姊妹子系统：NUMAflow

`numaflow/`（纯 C11，零 Redis/libnuma 依赖）是与上述 NUMA 模块层**平级的独立构
件**，不在 5.2 的依赖链条内。它把「迁移策略」这件事拆解成 36 个可组合的原子操
作，并在此基础上实现了新的默认策略 CAAT。两者通过一条薄桥接文件相连：
`src/numa_flow.c`（仅在 `HAVE_NUMA` 下编译）实现 NUMAflow 桥接契约要求的两个回调
（`enumerate`/`apply`），并把 `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT` 暴露
给 Redis 客户端；加载后的工作流由 `serverCron` 按配置的间隔周期执行。

NUMAflow 自身的构件分解、36 个原子操作分类、CAAT 算法、公平评测框架等，完整记
录在它自己的文档里，此处不重复：[`../numaflow/README.md`](../numaflow/README.md)。

## 5.7 到运行时视图和贯穿性概念的指引

本章只回答「有哪些构件」。这些构件在具体场景下（客户端发起 `SET`、`serverCron`
触发一轮迁移、进程启动）如何互相调用，见
[`06-runtime-view.md`](06-runtime-view.md)；跨越多个构件的共享约定（初始化顺
序、`_serverLog` 规范、按编码类型的迁移适配器模式等），见
[`08-crosscutting-concepts.md`](08-crosscutting-concepts.md)。
