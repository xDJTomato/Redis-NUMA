# evict_numa：NUMA 感知的淘汰前降级

> 本文档没有对应的历史设计稿——`evict_numa` 此前一直没有独立的设计文档，仅在
> `ARCHITECTURE.md` 里作为模块清单的第 10 项被简要提及。以下内容完全基于当前
> 实际源码（`src/evict_numa.c`、`src/evict.h`、`src/evict.c` 中的调用点）重新
> 编写，属于本次重构补齐的空白，而不是对旧文档的重排。

## 1. 职责（Responsibility）

Redis 原生的内存淘汰逻辑是"内存超限 → 从候选池里选一个 key → 直接删除"。
`evict_numa` 在**真正删除之前**插入一个可选步骤：判断这个 key 是否值得"降级"
（demote）——迁移到压力更小、距离更近、带宽更充裕的另一个 NUMA 节点，而不是
被直接淘汰。只有当降级本身不可行（找不到合适的目标节点，或迁移失败）时，才
退回到 Redis 原有的真实淘汰路径。

其核心问题是一个**在线节点选择问题**：给定一个即将被淘汰的对象和它当前所在的
节点，在其余所有 NUMA 节点中，挑一个"性价比最高"的降级目的地——同时兼顾三个
互相牵制的因素：物理距离、目标节点当前压力、目标节点当前带宽占用。

## 2. 接口（Interface）

对外暴露的函数全部声明在 `src/evict.h`（**没有独立的 `evict_numa.h`**——这是
和其他大多数模块不同的一点，声明和实现的.c文件是分开的两个文件，但头文件被合
并进了 `evict.h`）：

```c
/* 淘汰前尝试降级；返回结果码，target_node 输出实际迁到的节点。 */
numa_demote_result_t evictionTryNumaDemote(void *db, char *key, void *val,
                                            int *target_node);

/* 在除 current_node 外的所有节点中，找出综合评分最优的降级目标；
 * 找不到合适节点时返回 -1。 */
int numaFindBestDemoteNode(size_t object_size, int current_node);

/* 三个独立的节点状态查询，均供 numaFindBestDemoteNode 内部使用，
 * 也可单独调用做诊断。 */
double numaGetNodePressure(int node_id);       // 0.0~1.0，越大越紧张
size_t numaGetNodeFreeMemory(int node_id);     // 字节数
double numaGetNodeBandwidthUsage(int node_id); // 0.0~1.0，转调 numa_bw_monitor
```

结果码定义（`src/evict.h`）：

```c
typedef enum {
    NUMA_DEMOTE_OK = 0,      /* 降级成功 */
    NUMA_DEMOTE_NO_NODE,     /* 没有可用的目标节点 */
    NUMA_DEMOTE_FAILED,      /* 找到了目标节点，但迁移本身失败 */
    NUMA_DEMOTE_SKIP         /* 直接跳过（未启用/对象太小/参数无效） */
} numa_demote_result_t;
```

唯一的调用方是 `src/evict.c` 里 `performEvictions()` 淘汰主循环——`evict_numa`
本身不会被任何定时任务主动触发，它纯粹是淘汰路径上的一个"拦截钩子"。

## 3. 内部结构与关键路径（Internal Structure & Key Paths）

### 3.1 与 `ARCHITECTURE.md` 现有描述的一处不一致

`ARCHITECTURE.md` 目前描述本模块时写的是「扩展 `evictionPoolEntry` 结构体，新增
`current_node`、`object_size`、`numa_migrated` 三个字段」。**这与当前源码不符**：
读取 `src/evict.h` 可以看到 `evictionPoolEntry` 结构体本身完全是 Redis 原生的
四个字段，没有任何改动：

```c
struct evictionPoolEntry {
    unsigned long long idle;  /* 对象空闲时间（LFU 下是反向频率） */
    char *key;                 /* key 名（SDS） */
    char *cached;               /* 缓存的 SDS 对象 */
    int dbid;                   /* 所属 DB 编号 */
};
```

实际的降级判断根本不依赖在候选池条目里预先记录"当前节点/对象大小/是否已迁移"
——而是在 `performEvictions()` 真正要淘汰某个候选 key 的那一刻，**当场**调用
`evictionTryNumaDemote()`，函数内部自己重新计算对象大小（`objectComputeSize`）
和当前节点（`numa_get_node_id`），是一个无状态的"淘汰前拦截"，不需要持久化到
候选池结构里。这份文档以实际代码为准；`ARCHITECTURE.md` 里的这处描述建议后续
一并修正。

### 3.2 淘汰主循环里的插入点

`src/evict.c` 的 `performEvictions()` 在原生逐个淘汰候选 key 的循环里，选出
`bestkey` 之后、真正执行淘汰之前，插入了这一段（`#ifdef HAVE_NUMA` 包裹）：

```c
if (server.numa_demote_enabled && val != NULL &&
    numa_demotions < server.numa_demote_max_migrate)
{
    int target_node = -1;
    numa_demote_result_t demote_result =
        evictionTryNumaDemote(&server.db[bestdbid], bestkey, val, &target_node);

    if (demote_result == NUMA_DEMOTE_OK) {
        /* 本地内存已释放，对象仍然存在（只是换了节点）。
         * 把这个候选池条目清空，continue 处理下一个候选，
         * 不进入下面真正的淘汰分支。 */
        numa_demotions++;
        ...
        continue;
    }
    if (demote_result == NUMA_DEMOTE_NO_NODE) {
        /* 降级不可行，落到下面的真实淘汰路径 */
    }
}
/* 走到这里说明没有降级（未启用/跳过/找不到节点/降级失败），
 * 继续原生淘汰流程 */
```

`numa_demotions`（本轮淘汰循环内的局部计数器）与配置项
`numa-demote-max-migrate` 一起构成**单轮预算控制**：每次内存超限触发的淘汰
过程里，最多允许这么多次降级迁移，避免为了腾内存反而把大量带宽花在跨节点搬
运上——超过预算之后，即使还有候选适合降级，也会直接落回真实淘汰。

### 3.3 `evictionTryNumaDemote()` 内部流程

1. 总开关检查：`server.numa_demote_enabled` 为假直接 `NUMA_DEMOTE_SKIP`。
2. 计算对象真实大小 `objectComputeSize()`；小于 `numa-demote-min-size` 直接
   `NUMA_DEMOTE_SKIP`——小对象迁移的固定开销（一次跨节点 `memcpy` + 元数据
   更新）相对它能腾出的内存收益不划算。
3. 确定对象当前所在节点：STRING 类型通过 `sdsAllocPtr()` 找到底层分配基址再
   查询节点归属；其它类型直接用 `val_obj->ptr`；查不到时回退到
   `numa_pool_get_node()`（当前线程/进程的默认节点）。
4. 调用 `numaFindBestDemoteNode()` 挑目标节点；挑不到（`-1`）则
   `NUMA_DEMOTE_NO_NODE`。
5. 调用 `numa_migrate_single_key()`（`numa_key_migrate` 模块提供）真正执行迁移；
   成功则累加 `stat_numa_demotions`/`stat_numa_demote_bytes`，并按 NUMA 距离
   （`numa_distance() <= 20` 记为"近"，否则记为"远"）分别累加
   `stat_numa_demote_near`/`stat_numa_demote_far`；失败则累加
   `stat_numa_demote_failed`，向上返回 `NUMA_DEMOTE_FAILED`。

这五个统计计数器最终在 `src/server.c` 里随 `INFO stats` 一并输出（与
`stat_evictedkeys` 相邻），不需要单独的 `NUMA` 子命令去查——降级统计被当作
"淘汰体系的一部分"而不是"NUMA 迁移体系的一部分"来呈现，这个划分与
`numa_key_migrate`/`numa_command` 模块自己的 `NUMA MIGRATE STATS`（追踪的是
主动/策略触发的迁移）是分开计数的两套统计口径，读代码或读 `INFO` 输出时不要
混淆。

### 3.4 `numaFindBestDemoteNode()`：加权评分算法

对当前节点之外的每一个候选节点，依次执行三层过滤，任何一层不通过就整节点跳过
（不进入评分）：

1. **压力过滤**：`numaGetNodePressure(i) >= numa-demote-pressure-threshold/100`
   则跳过。压力的计算本身是自适应的——当 `server.maxmemory > 0` 时，压力定义
   为"该节点上 Redis 自己用掉的内存 / (`maxmemory` 平均分摊到各节点的份额)"，
   而不是整台物理机该节点的内存占用比。源码注释里明确解释了原因：在一台
   441GB 双路服务器上，如果按物理节点总内存计算压力，Redis 自身占用永远只有
   9.7% 左右，压力阈值永远触发不了，因此必须以 Redis 自己的配额为分母才有意
   义。未设置 `maxmemory` 时才回退到读取 `/sys/devices/system/node/nodeX/meminfo`
   算物理层面的压力。压力值本身有 1 秒 TTL 的缓存（`PRESSURE_CACHE_TTL_MS`），
   避免高频淘汰时反复读 sysfs。
2. **带宽过滤**：`numaGetNodeBandwidthUsage(i) >= numa-bw-saturation-threshold/100`
   则跳过（实际转调 `numa_bw_monitor` 模块的 `numa_bw_get_usage()`）。
3. **容量过滤**：目标节点的空闲内存必须至少是待迁移对象大小的 2 倍
   （`free_mem < object_size * 2` 则跳过）——留出安全余量，避免迁移后立刻又
   在目标节点触发新一轮压力。

通过三层过滤的候选节点进入评分。评分前先对距离、压力、带宽三个维度分别按
"候选集合内的最大值"归一化到 `[0,1]`，再按配置的权重加权求和（或在
`numa-demote-prefer-closer` 关闭时用三者等权平均）：

```text
score = 归一化距离 × distance_weight/100
      + 归一化压力 × pressure_weight/100
      + 归一化带宽 × bandwidth_weight/100
```

分数**越低越好**，取分数最小的候选节点作为最终降级目标。三个权重对应
`redis.conf` 里的 `numa-demote-distance-weight`（默认 40）、
`numa-demote-pressure-weight`（默认 30）、`numa-demote-bandwidth-weight`
（默认 30）——`ARCHITECTURE.md` 里"距离 40% + 压力 30% + 带宽 30%"这句概括说的
就是这三个默认值，这一点和代码是一致的，需要修正的只是上一节提到的
`evictionPoolEntry` 字段描述。

## 4. 质量与性能特性（Quality & Performance Characteristics）

- **无额外常驻内存**：不修改任何 Redis 原生的候选池数据结构，只在函数栈上分配
  一个大小为 `MAX_NUMA_NODES`（16）的候选节点数组，随调用结束即释放。
- **压力查询有缓存**：`numaGetNodePressure()` 的 1 秒 TTL 缓存把"每次淘汰候选
  都要读一遍 sysfs"降到"每个节点每秒最多读一次"，在高频淘汰场景下避免成为
  瓶颈。
- **可预算、可降级为纯淘汰**：`numa-demote-max-migrate` 保证降级带来的额外
  跨节点流量在任何一次淘汰批次里都有硬上限；`numa-demote-enabled=no` 时整个
  模块直接短路为 `NUMA_DEMOTE_SKIP`，退化为原生 Redis 淘汰行为，不需要重新
  编译。
- **单点失败不放大**：一次降级失败（`NUMA_DEMOTE_FAILED`）只会让这一个候选
  key 退回原生淘汰，不会中断整个淘汰批次，也不会影响后续候选的降级判断。

## 5. 与其他模块的关系（Relations to Other Modules）

- **`numa_bw_monitor`**：`numaGetNodeBandwidthUsage()` 是对
  `numa_bw_get_usage()` 的直接透传，`evict_numa` 自己不采集带宽数据，完全依赖
  该模块已经在后台维护好的每节点带宽占用状态。
- **`numa_key_migrate`**：真正的跨节点搬运动作（`numa_migrate_single_key()`）
  由该模块完成；`evict_numa` 只负责"要不要迁"和"迁到哪"的决策，不涉及任何
  一种数据类型编码的迁移细节。
- **`numa_pool`**：`numa_pool_get_node()`/`numa_get_node_id()` 用于查询对象
  当前的节点归属，这两个查询接口由 `numa_pool` 提供。
- 在 `ARCHITECTURE.md` 描述的模块依赖顺序中，`evict_numa` 处于**最上层**——
  它依赖 `numa_key_migrate`/`numa_pool`/`numa_bw_monitor` 已经初始化完毕，但
  没有任何模块反过来依赖它，也不出现在 `numa_strategy_slots` 的槎位调度体系
  里（它不是一个"策略"，是淘汰路径上的一次性拦截，不参与 `serverCron` 的周期
  调度）。

## 6. 未解决问题与已知限制（Open Issues & Known Limitations）

- **本机验证局限**：本项目实际开发主机只有 1 个物理 NUMA 节点，
  `numaFindBestDemoteNode()` 里 "`num_nodes <= 1` 直接返回 -1（无需降级）" 这条
  短路路径在本机上永远命中，三层过滤和加权评分逻辑没有在真实多节点硬件上跑过
  完整路径，只在文档层面和多节点 QEMU 客户机（见 `TESTING.md` 第 5 项）里做过
  功能验证，没有做过大规模压力下的评分准确性验证。
- **压力缓存的一致性代价**：1 秒 TTL 意味着极端场景下（同一秒内节点压力剧烈
  变化）评分可能基于略微过期的数据，目前没有做"淘汰高峰期缩短 TTL"这类自适
  应调整。
- **STRING 类型之外的"当前节点"判断**：`evictionTryNumaDemote()` 对 STRING 用
  `sdsAllocPtr()` 找分配基址，其它类型直接用 `val_obj->ptr`；如果某类型的
  `ptr` 本身不是该对象实际占用内存的起始地址（例如某些编码下 `ptr` 指向的是
  一个内部子结构而不是整块分配），`numa_get_node_id()` 查到的节点可能不准确，
  这里的判断依赖各类型编码的内存布局假设，没有对全部编码分支做过逐一验证。
- **`ARCHITECTURE.md` 需要同步修正**：如第 3.1 节所述，现有英文/中文
  `ARCHITECTURE.md`/`ARCHITECTURE.zh-CN.md` 中关于 `evictionPoolEntry` 扩展字
  段的描述与当前代码不符，建议在下一次编辑那两份文档时一并订正为本文档第 3.1
  节的说法。
