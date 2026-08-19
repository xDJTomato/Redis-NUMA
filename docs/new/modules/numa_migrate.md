# numa_migrate — 底层块级 NUMA 迁移

> 本文档是 `docs/new/05-building-block-view.md`（arc42 §5 构建块视图）下 `numa_migrate`
> 模块的详情表。源码：`src/numa_migrate.c` / `src/numa_migrate.h`。

## 1. 职责（Responsibility）

`numa_migrate` 提供最原始的一条能力：**把一块已分配的内存，从它当前所在的 NUMA
节点整块搬到另一个节点**——分配新内存、`memcpy` 内容、释放旧内存、记录统计。它
不关心这块内存对应哪个 Redis key、是什么数据类型，也不关心迁移决策（该不该迁、
迁到哪）——这些都是更上层模块的职责。可以把它理解成"NUMA 迁移的汇编指令"：一个
被设计为供其他模块调用的底层原语。

在 `libnuma → numa_pool → numa_migrate → numa_key_migrate → …` 的模块依赖链中，
它排在 `numa_pool`（分配器）之上、`numa_key_migrate`（按 key 迁移）之下。

## 2. 接口（Interface）

```c
int   numa_migrate_init(void);
void  numa_migrate_cleanup(void);

void *numa_migrate_memory(void *ptr, size_t size, int target_node);

void  numa_migrate_get_stats(numa_migrate_stats_t *stats);
void  numa_migrate_reset_stats(void);
```

```c
typedef struct {
    uint64_t total_migrations;   // 已完成的迁移次数
    uint64_t bytes_migrated;     // 已迁移的总字节数
    uint64_t failed_migrations;  // 失败的迁移次数（仅统计分配失败，见下文）
    uint64_t migration_time_us;  // 迁移消耗的总时间（微秒）
} numa_migrate_stats_t;
```

`numa_migrate_memory()` 是唯一的核心操作：把 `ptr` 指向的、大小为 `size` 的内存块
迁移到 `target_node`。成功返回新地址（旧指针立即失效），失败返回 `NULL`（此时旧
内存保持不变，调用者可以继续安全使用旧指针）。

## 3. 内部结构与关键路径（Internal Structure & Key Paths）

### 3.1 迁移路径

```
numa_migrate_memory(ptr, size, target_node)
    │
    ├── 0. 前置检查：模块已初始化？ptr/size 有效？target_node 在合法范围内？
    │     （任一不满足 → 直接返回 NULL，不计入 failed_migrations）
    │
    ├── 1. 记录开始时间（clock_gettime(CLOCK_MONOTONIC)）
    │
    ├── 2. 在目标节点分配新内存
    │     new_ptr = numa_zmalloc_onnode(size, target_node)
    │     │
    │     └── 分配失败 ──► failed_migrations++，返回 NULL
    │
    ├── 3. memcpy(new_ptr, ptr, size)
    ├── 4. zfree(ptr)                     // 释放旧内存
    ├── 5. total_migrations++，bytes_migrated += size，累加耗时
    └── 6. 返回 new_ptr
```

**与旧版设计文档的一处重要差异**：本模块的实现走的是 **`numa_zmalloc_onnode` +
`zfree`**（即 `zmalloc.c` 那一层的封装，会经过 `numa_pool` 的 PREFIX 元数据管
理），而不是直接调用裸的 `numa_alloc_onnode`/`numa_free`（`ARCHITECTURE.md` 的
简述里用的是后一种更笼统的说法）。这意味着通过本函数迁移的内存，其 PREFIX 元数
据（大小、节点、热度字段）会随 `numa_zmalloc_onnode` 的分配路径被正确重建，不会
因为绕过 `numa_pool` 而产生元数据不一致。

### 3.2 参数校验与错误统计的实际口径

当前实现（`src/numa_migrate.c`）里，只有"目标节点分配失败"这一种情况才会让
`failed_migrations` 计数器 +1；参数非法（`ptr == NULL`、`size == 0`）或
`target_node` 越界，都是直接返回 `NULL`，**不会**计入 `failed_migrations`。也就
是说这个统计字段目前只反映"资源不足导致的迁移失败"，看不出"调用方传参错误"发
生过多少次——见第 6 节的已知限制。

### 3.3 初始化与前置条件

```c
int numa_migrate_init(void) {
    if (migrate_initialized) return 0;
    if (numa_available() == -1) return NUMA_MIGRATE_ERR;
    memset(&migrate_stats, 0, sizeof(migrate_stats));
    migrate_initialized = 1;
    return NUMA_MIGRATE_OK;
}
```

`numa_migrate_memory()` 内部会检查 `migrate_initialized` 标志，未初始化时直接返
回 `NULL`——这与本仓库其它 NUMA 模块共用的"未初始化就安全退化/拒绝"惯例一致（参
见 `docs/new/02-constraints.md` 关于初始化顺序的约束）。

## 4. 质量与性能特性（Quality & Performance Characteristics）

- **原子性**：迁移的"决策 + 执行"目前全部发生在 Redis 主线程内（无论是手动触发
  还是 `serverCron` 里的自动迁移），Redis 本身单线程处理客户端命令，因此迁移期间
  不存在其它命令并发访问同一块内存的情况——原子性是靠"从不并发"天然成立的，模
  块本身没有加锁。
- **失败时不破坏原状态**：无论是参数校验失败还是目标节点分配失败，旧指针/旧内
  存都保持不变，调用方可以安全地继续使用原指针——不存在"迁移失败但旧数据已被破
  坏"的中间状态。
- **单次迁移是 O(size) 的同步阻塞操作**：`memcpy` 的开销随对象大小线性增长，且
  发生在调用者的执行路径上（没有异步化）。对大对象的迁移会短暂阻塞调用者——这
  是 `docs/new/09-architecture-decisions.md` 中"为什么迁移预算/渐进式扫描要限制
  每轮迁移数量"的原因之一。

## 5. 与其他模块的关系（Relations to Other Modules）

- **依赖 `numa_pool`**（通过 `zmalloc.c` 暴露的 `numa_zmalloc_onnode`）：目标节点
  上的实际分配由 `numa_pool` 的 slab/尺寸类逻辑完成。
- **理论上应被 `numa_key_migrate` 复用，但实际并未被调用**：见第 6 节。
- **不直接被 NUMAflow 迁移策略调用**：`caat`/`composite_lru`/`tinylfu` 三个预设
  （`numaflow/src/nf_strategy.c`，经桥接 `src/numa_flow.c`）触发迁移时，最终
  调用的是 `numa_key_migrate.c` 暴露的 `numa_migrate_single_key()`/
  `numa_migrate_multiple_keys()`，而不是本模块的 `numa_migrate_memory()`。

## 6. 未解决问题与已知限制（Open Issues & Known Limitations）

- **`numa_migrate_memory()` 目前是未被调用的死代码路径**：在当前代码库中对
  `numa_migrate_memory` 做全仓库引用检索（`grep -rn "numa_migrate_memory" src/`），
  除了它自己的定义/声明外没有任何调用点。真正的按 key 迁移（`numa_key_migrate.c`）
  为每种数据类型各自实现了一遍"目标节点分配 + `memcpy` + 释放旧内存"的逻辑（见
  `modules/numa_key_migrate.md`），并没有复用本模块提供的通用块迁移原语。这是一
  处**实现与最初模块划分意图不一致**的技术债：`numa_migrate` 的存在理由是"给上
  层一个可复用的块迁移原语"，但目前上层选择了自己直接实现，而不是调用它。后续
  如果要减少重复代码，可以考虑让 `numa_key_migrate.c` 的各类型适配器改为调用
  `numa_migrate_memory()`，或者干脆明确把本模块降级为"历史遗留/供手动调试使用
  的独立工具函数"，二者都需要一次有意识的决策，而不是让现状继续含糊下去。
- **`numa_migrate_multiple_keys()` 不属于本模块**：早期设计文档把这个函数放在
  `numa_migrate` 模块下描述，但它实际定义在 `numa_key_migrate.c`/`.h` 里（按 key
  批量迁移，内部逐个调用 `numa_migrate_single_key()`），与本模块的
  `numa_migrate_memory()` 是两个不同层次、不同文件的函数，只是名字相似容易混
  淆。本文档已按实际代码归属改正，完整描述见 `modules/numa_key_migrate.md`。
- **`failed_migrations` 统计口径较窄**：如 3.2 节所述，只统计分配失败，不统计参
  数非法/节点越界，排查"迁移为什么变少了"时不能只看这一个计数器。
- **头文件注释仍标注"Phase 1: basic migration functionality for testing"**：
  `numa_migrate.h` 顶部注释写着这是测试阶段的基础功能，Phase 2（自动负载均衡+
  热度追踪）"planned"——但热度追踪已经在 `numa_key_migrate` 中无条件实现，自动
  迁移调度已经在 NUMAflow（经桥接 `src/numa_flow.c` 驱动 `caat`/`composite_lru`/
  `tinylfu` 三个预设）中实现，只是没有经过本模块。这条注释已经过时，容易让读
  代码的人误以为"自动迁移还没做"，是一处应当更新（或删除）的注释债务，本文档
  予以指出但未改动源码本身。
