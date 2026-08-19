# 附录：Intel TieredMemDB 分层设计分析（相关工作对比）

> 本附录由 [`09-architecture-decisions.md`](../09-architecture-decisions.md) 引用，
> 作为「为什么本项目选择运行时热度感知 + 双向迁移，而不是纯分配期静态分流」这一
> 决策的支撑材料。内容忠实保留自原 `14-tieredmemdb-analysis.md` 的分析结论，仅调整
> 了标题层级以适配 arc42 附录的位置，分析本身未重新演绎。

本文档基于 TieredMemDB（Intel 基于 Redis 7.0.2 的 NUMA/PMEM 内存分层分支）源码
分析，提炼其设计思路，与本项目（Redis-NUMA/CXL）的方案进行对比，探讨可借鉴的
设计方向。

## 1 项目概况

TieredMemDB 的定位是为多层内存系统（DRAM + Persistent Memory / CXL）提供透明的
分层存储。其核心思想是：**小对象、热数据放 DRAM，大对象、温数据放 PMEM**，通过
一个全局阈值（threshold）在 zmalloc 层自动路由。

依赖：memkind 库（Intel，封装了 DAX KMEM NUMA 节点的分配接口）。

基线版本：Redis 7.0.2。

## 2 核心设计：阈值分流

TieredMemDB 的分层机制完全集中在 `zmalloc.c`，通过一个全局变量 `pmem_threshold`
控制分流：

```
zmalloc(size):
    if size < pmem_threshold → malloc (DRAM, jemalloc)
    else                     → memkind_malloc (PMEM)

zfree(ptr):
    if memkind_detect_kind(ptr) == DRAM → free_dram(ptr)
    else                                → free_pmem(ptr)

zrealloc(ptr, size):
    if memkind_detect_kind(ptr) == DRAM → realloc_dram(ptr, size)
    else                                → realloc_pmem(ptr, size)
```

关键点：**realloc 不跨层迁移**。一个对象一旦分配到 DRAM 或 PMEM，后续 realloc
仍留在同一层。没有运行时热度感知，没有跨层迁移。

## 3 四种内存分配策略

通过 `redis.conf` 的 `memory-alloc-policy` 配置：

| 策略 | 行为 | pmem_threshold |
|------|------|---------------|
| only-dram | 仅用 DRAM，不使用 PMEM | SIZE_MAX |
| only-pmem | 仅用 PMEM | 0 |
| threshold | 静态阈值：小于阈值用 DRAM，大于等于阈值用 PMEM | 用户配置值 |
| ratio | 动态阈值：自动调整阈值以维持 DRAM:PMEM 目标比例 | 动态调整 |

### 3.1 Ratio 策略（动态阈值调节）

这是 TieredMemDB 最有特色的设计。`pmem.c` 中的 `adjustPmemThresholdCycle()` 由
`serverCron` 周期性调用（默认 100ms），执行以下逻辑：

```
1. 读取 zmalloc_used_dram_memory() 和 zmalloc_used_pmem_memory()
2. 计算当前 PMEM/DRAM 比例
3. 与目标比例（target_pmem_dram_ratio）比较
4. 如果偏差 > 2%：
   - 当前比例过高（PMEM 过多）→ 提升阈值（更多分配去 DRAM）
   - 当前比例过低（DRAM 过多）→ 降低阈值（更多分配去 PMEM）
5. 调节步长分两档：
   - 正常步进：5% × variableMultiplier
   - 激进步进：25% × variableMultiplier（偏差扩大时）
```

配置参数：

```
dram-pmem-ratio 1 3              # 目标 DRAM:PMEM = 1:3（即 25% DRAM）
initial-dynamic-threshold 64     # 初始阈值 64 字节
dynamic-threshold-min 24         # 阈值下限
dynamic-threshold-max 10000      # 阈值上限
memory-ratio-check-period 100    # 检查周期（毫秒）
```

### 3.2 PMEM 变体

控制 PMEM 分配的 NUMA 节点选择：

```
single    使用最近的 PMEM NUMA 节点（MEMKIND_DAX_KMEM）
multiple  使用所有 PMEM NUMA 节点（MEMKIND_DAX_KMEM_ALL）
```

## 4 关键结构体设计

### 4.1 zmalloc 层

TieredMemDB 的 zmalloc 通过 memkind 库实现分层，不修改分配对象的布局（没有额外
PREFIX）。memkind 内部使用 jemalloc 作为底层分配器，通过 `memkind_detect_kind(ptr)`
在 free/realloc 时判断指针来源。

DRAM 和 PMEM 各自独立统计内存用量：

```c
static redisAtomic size_t used_dram_memory = 0;
static redisAtomic size_t used_pmem_memory = 0;
```

### 4.2 redisServer 中的 PMEM 字段

```c
int memory_alloc_policy;              // only-dram / only-pmem / threshold / ratio
unsigned int static_threshold;        // threshold 策略的静态阈值
unsigned int initial_dynamic_threshold; // ratio 策略的初始阈值
unsigned int dynamic_threshold_min;   // 动态阈值下限
unsigned int dynamic_threshold_max;   // 动态阈值上限
ratioDramPmemConfig dram_pmem_ratio;  // 目标 DRAM:PMEM 比例
double target_pmem_dram_ratio;        // 计算后的目标比值
int ratio_check_period;               // 比例检查周期
int hashtable_on_dram;                // 哈希表是否强制 DRAM
int pmem_variant;                     // single / multiple
```

### 4.3 hashtable-on-dram 优化

dict.c 中增加了 `dict_always_on_dram` 标志。当 `hashtable-on-dram yes` 时，dict
的哈希表数组（`ht_table[]`）和迭代器始终通过 `zcalloc_dram()` 分配，确保高频访问
的哈希桶不落入 PMEM。

这是一个务实的优化：哈希表的桶数组虽然可能很大，但每次随机访问都需要读取，放在
DRAM 可以避免 PMEM 的高延迟影响查找性能。

### 4.4 robj 强制 DRAM

object.c 中 `createObject()` 使用 `zmalloc_dram()` 分配 robj 结构体，
`decrRefCountDRAM()` 使用 `zfree_dram()` 释放。robj 本身（48 字节的元数据头）
始终在 DRAM，只有 `robj->ptr` 指向的实际数据可能在 PMEM。

## 5 INFO 输出扩展

TieredMemDB 在 `INFO memory` 中增加了：

```
pmem_threshold:64
used_memory_dram:1234567
used_memory_dram_human:1.18M
used_memory_pmem:9876543
used_memory_pmem_human:9.42M
```

## 6 没有做的事情

TieredMemDB 的设计有意保持简洁，以下功能**没有实现**：

- **没有运行时热度追踪**：不记录 Key 的访问频率
- **没有跨层迁移**：对象分配后不会在 DRAM 和 PMEM 之间移动
- **没有 Key 级别控制**：无法指定某个 Key 应该在哪一层
- **没有 NUMA 命令接口**：没有类似 `NUMA MIGRATE KEY` 的命令
- **没有带宽监控**：不感知内存带宽压力
- **realloc 不跨层**：扩容/缩容后留在原来的层

## 7 与本项目的设计对比

| 维度 | TieredMemDB | 本项目（Redis-NUMA/CXL） |
|------|-------------|-----------|
| 分流机制 | 全局阈值（按大小） | NUMA 节点选择（按压力权重） |
| 分配器 | memkind（jemalloc 封装） | 自研 Slab + Direct（libc） |
| 热度感知 | 无 | 16 字节 PREFIX，阶梯衰减 |
| 运行时迁移 | 无 | Composite LRU 双通道（内核）/ CAAT 晋升+降级（NUMAflow） |
| 比例控制 | 动态阈值反馈环 | 压力感知权重交错 |
| 元数据头 | 无额外 PREFIX | 16 字节 PREFIX |
| 哈希表优化 | hashtable-on-dram 强制 DRAM | 无特殊处理 |
| robj 位置 | 强制 DRAM | 跟随 zmalloc 策略 |
| 配置方式 | redis.conf 原生指令 | JSON 配置文件 + NUMA 命令 |
| Redis 版本 | 7.0.2 | 7.2.6 |
| 依赖 | memkind + daxctl + autoconf | libnuma |

## 8 可借鉴的思路

### 8.1 动态比例反馈环（Ratio 策略）

TieredMemDB 的 Ratio 策略提供了一个有价值的自适应思路：不需要预测哪些 Key 是热
的，而是通过调节全局阈值，让 DRAM 和 PMEM 的**总量比例**趋近目标值。

**可参考方向**：在本项目的 `serverCron` 中增加一个 Node 0/Node 1 内存比例监控。
当 Node 0（DRAM）占比超过目标时，提高 Node 1 的分配权重；反之降低。这与现有的
压力感知权重交错策略互补——压力感知关注的是「剩余容量」，比例反馈关注的是
「已用分布」。

### 8.2 关键结构强制 DRAM

TieredMemDB 将 dict 哈希桶数组和 robj 结构体强制分配到 DRAM，这是一个低成本高
收益的优化。

**可参考方向**：在本项目中，可以为 dict、robj 等高频随机访问的元数据结构提供
`zmalloc_local()` 接口，强制分配到 Node 0（DRAM）。目前这些结构通过 `zmalloc()`
分配，会被压力权重交错策略可能分配到 Node 1（CXL），导致元数据查找延迟上升。

### 8.3 按大小分流

TieredMemDB 的阈值分流虽然简单，但基于一个合理假设：小对象通常是元数据和频繁
访问的结构，大对象通常是用户数据（value）。

**可参考方向**：在现有的 `cxl_optimized` 策略中已有类似实现（小对象本地、大对象
远端），但目前该策略不是默认的。可以考虑将大小感知作为默认策略
`weighted_interleave` 的一个权重因子：大对象更倾向于 CXL 节点，小对象更倾向于
DRAM 节点。

### 8.4 memkind_detect_kind() 的启示

TieredMemDB 通过 `memkind_detect_kind(ptr)` 在 free/realloc 时自动判断指针来源
层。本项目则通过 PREFIX 的 `from_slab` 和 `node_id` 字段实现类似功能，但有 16
字节额外开销。

**对比**：memkind 的方案零额外开销，但依赖 memkind 库的内部元数据查询，且无法
携带热度等自定义信息。PREFIX 方案虽然有开销，但支持内联热度追踪，是本项目迁移
功能的基础。两种设计各有权衡，本项目的 PREFIX 方案在支持运行时迁移方面更有优势。

## 9 总结

TieredMemDB 和本项目代表了内存分层的两种设计哲学：

- **TieredMemDB**：分配时决策，不运行时迁移。简单、低开销、零侵入，但无法适应
  访问模式变化。适合稳态工作负载。

- **本项目**：分配时 + 运行时双重决策。复杂、有元数据开销，但能根据实际访问
  热度动态调整。适合访问模式变化的工作负载。

两者并非互斥。本项目可以借鉴 TieredMemDB 的比例反馈和关键结构 DRAM 固定策略，
在保留运行时迁移能力的同时，优化分配阶段的决策质量——这正是
[`09-architecture-decisions.md`](../09-architecture-decisions.md) 中相关决策记录
引用本附录的原因。
