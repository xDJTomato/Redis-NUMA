# 12. 术语表（Glossary）

> arc42 第 12 章。收录本项目文档中反复出现、且不查上下文就容易产生歧义的术语。

| 术语 | 含义 |
| --- | --- |
| **NUMA**（Non-Uniform Memory Access，非统一内存访问） | 多路服务器上，每个 CPU 插槽直接连接一部分物理内存；访问本地内存快，访问挂在其它插槽（节点）上的内存慢。这种"访问延迟取决于物理距离"的架构统称 NUMA。 |
| **NUMA 节点（NUMA node）** | 一组 CPU 核心 + 它们直连的一段物理内存，是 NUMA 架构里资源调度的最小地理单位。 |
| **CXL（Compute Express Link）** | 建立在 PCIe 物理层之上的互联协议；其中 **CXL.mem** 子协议允许把挂在 PCIe 插槽上的内存模组当作 CPU 可直接 `load`/`store` 的普通内存使用，延迟/带宽通常弱于本地 DRAM，但可以看作"又多了一个更慢的 NUMA 节点"。 |
| **Slab / tcache** | `numa_pool` 分配器的两级设计：slab 是按固定大小类批量管理小对象（≤4KB）的内存块；tcache（thread-local cache）是每线程私有的空闲对象缓存，避免每次分配/释放都走全局锁。 |
| **PREFIX 元数据（`numa_alloc_prefix_t`）** | `numa_pool` 在每次分配返回的指针前隐藏的 16 字节头部，记录该内存块的大小、所在节点、热度、访问元数据——这是为什么本项目必须 `#define NO_MALLOC_USABLE_SIZE`。 |
| **热度 / hotness** | 一个 key 被访问的频繁程度的量化值，由迁移策略（Composite LRU、TinyLFU 等）维护，用于判断该 key 是否值得被迁移到更快的节点。 |
| **策略槎位（strategy slot）** | `numa_strategy_slots` 提供的 16 个可插拔"迁移策略"挂载点，每个槎位通过 vtable 实现统一的执行接口，可被启用/禁用/替换。 |
| **Composite LRU** | 槎位 1 的默认迁移策略：双通道设计——热候选环形缓冲区（快路径）+ 渐进式字典扫描（慢路径），基于阶梯式惰性衰减的热度值决定迁移。 |
| **TinyLFU** | 槎位 2 的频率驱动迁移策略（默认关闭）：用 Count-Min Sketch + Doorkeeper 布隆过滤器以固定内存（~40KB）近似统计访问频率。 |
| **Count-Min Sketch（CMS）** | 一种概率数据结构，用固定大小的计数器矩阵近似统计元素出现频率，牺牲少量精度换取 O(1) 空间和更新/查询开销。 |
| **Doorkeeper（布隆过滤器）** | TinyLFU 中用于"先过一遍、只有出现过一次以上的 key 才计入 CMS"的布隆过滤器，避免偶发的一次性访问污染频率统计。 |
| **CAAT（Cost-Aware Adaptive Tiering，成本感知自适应分层）** | NUMAflow 新增的默认策略：与 Composite LRU/TinyLFU 不同，CAAT 同时具备"晋升"和"降级"两条流水线，核心是按 `(源节点代价-目标节点代价)×访问率-迁移代价` 计算净收益，只晋升净收益为正的 key，并主动把 DRAM 上不再热的数据降级腾位。 |
| **原子操作（NUMAflow 语境）** | NUMAflow 把现有调度策略拆解出的 36 个最小可复用步骤（如 `score_hotness`、`filter_hot`、`emit_migrate`），可以像乐高积木一样拼接成完整策略。 |
| **DAG 工作流** | 由若干原子操作按依赖关系连成的有向无环图，是 NUMAflow 里"一条完整调度策略"的表达方式，可在 TUI/GUI 中可视化编辑。 |
| **AE time event（AE 定时事件）** | Redis 自身事件循环（`ae.c`）提供的定时任务机制。`numa_strategy_slots` 支持把某个策略槎位的调度从 `serverCron`（每秒一次、与其它槎位共享周期）切换为独立的 AE time event，从而获得自己的调度节奏而不拖慢其它槎位。 |
| **budget / deadline（调度语境）** | AE 调度模式下，一次策略执行被赋予的"最多处理多少项"（budget）和"最晚什么时候必须让出"（deadline）两个约束，用于防止某个慢策略拖长整个事件循环。 |
| **晋升（promotion）/ 降级（demotion）** | 晋升指把数据从慢/远节点搬到快/近节点（通常因为它变热了）；降级指反向操作（通常因为它变冷了，或者目标节点需要腾出容量）。CAAT 是本项目里唯一同时具备两个方向的策略。 |
| **Graft 合并（`git replace --graft`）** | 本项目 6.2.21→7.2.6 迁移时使用的技术：给本仓库无关联历史的根提交"移植"一个指向上游 6.2.21 提交的假父提交，从而让 Git 能计算出正确的合并基点、执行一次真正的三方合并，而不是把两棵无关的树直接对拍。详见 [`redis7-migration.md`](../redis7-migration.md)。 |
