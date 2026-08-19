# 6. 运行时视图（Runtime View）

本章按 arc42 §6 的惯例，只挑选几个真正能说明系统行为的关键场景，用简洁的编号步骤
呈现——不是详尽的调用链堆栈。需要逐层展开的完整版本见
[`appendix/call-chain-reference.md`](appendix/call-chain-reference.md)。

## 6.1 场景：进程启动

1. `main()` 调用 `initServer()`，完成 Redis 核心状态初始化，其中
   `zmalloc_init()` 会调用 `numa_slab_init()`（`src/zmalloc.c` → `src/numa_pool.c:312`）
   ——这是 NUMA 分配器最早初始化的一环，不在 `server.c` 里，而是挂在
   `zmalloc` 自己的初始化路径上。
2. `initServer()` 返回后，`#ifdef HAVE_NUMA` 块依次调用：
   - `numa_config_strategy_init()`（`src/server.c:7384`），随后
     `numa_config_set_strategy(WEIGHTED_INTERLEAVE)`（`src/server.c:7385`）
     把默认分配策略设为加权轮询插值。
   - `numa_key_migrate_init()`（`src/server.c:7390`）。
   - `numa_bw_monitor_init()`（`src/server.c:7395`）。
   - `numa_flow_init()`：初始化 NUMAflow 桥接状态；除非
     `numa-enabled no`，随后自动读取 `numa-flow-default-strategy`（默认
     `caat`）与 `numa-flow-interval-sec`，用 `nf_strategy_build()` 把对应的
     原子操作 DAG 登记为 `default` 工作流条目——不需要手动 `NUMA FLOW LOAD`
     就能得到迁移行为。
3. 进入 `aeMain()` 事件循环，NUMA 模块此后只在 `serverCron`（`numa_flow_cron()`
   按 `interval_sec` 判断是否该跑已加载的工作流，没有 AE time event 变体，见
   ADR-08）和命令处理路径中被驱动。

要点：`numa_init()`／`numa_slab_init()` 必须先于 `initServer()` 建立的状态完成，
而按键迁移/带宽监控/NUMAflow 桥接模块的初始化必须晚于 `initServer()`——顺序颠倒
是启动期崩溃最常见的原因（见 [`08-crosscutting-concepts.md`](08-crosscutting-concepts.md)）。

## 6.2 场景：GET 命令触发热度追踪

1. 客户端发 `GET user:100`，`getCommand()` → `lookupKeyRead()` → `lookupKey()`
   在 `db->dict` 里找到 `val`。
2. `#ifdef HAVE_NUMA` 分支里，`db.c` 无条件调用 `numa_key_migrate_touch()`：
   - 读出 PREFIX 里的当前热度与上次访问时间；
   - 按空闲时长做阶梯式惰性衰减；
   - 热度 +1（上限 7）——这是 NUMAflow `enumerate()` 读取的唯一中立
     ground truth，不再依赖任何策略槎位是否 enabled。
3. 同一处访问路径上，`db.c` 也调用 `numa_flow_observe_access(key)`
   （`src/numa_flow.c`）：把这次真实访问喂给 `nf_tracker_observe()`——这是
   Redis 桥接里唯一调用该函数的地方（此前的一个真实 bug：这一步完全缺失，
   TinyLFU/CAAT 的 `cms_estimate` 永远读到 0，见 [ADR-09](09-architecture-decisions.md)）。
4. 返回值给客户端——整个追踪过程不阻塞、不改变返回结果，只更新 PREFIX 元数据
   与 NUMAflow 的频率追踪器。

## 6.3 场景：serverCron 驱动的自动迁移

1. `serverCron()` 每秒调用 `numa_flow_cron()`；对每个已加载的工作流（至少有
   启动时自动登记的 `default`），若到达其 `interval_sec` 就调用
   `nf_bridge_run()`。
2. `nf_bridge_run()` 通过桥接回调 `enumerate()`（`src/numa_flow.c`）枚举候选
   key（读取 `numa_key_migrate_touch()` 维护的热度信号），构造 NUMAflow 的
   执行上下文，驱动该工作流对应的原子操作 DAG（`caat`/`composite_lru`/
   `tinylfu`/`noop` 预设，定义于 `numaflow/src/nf_strategy.c`）：
   - `caat` 预设先按每个 item 的原始驻留位置分叉（`filter_local`/
     `filter_remote`），DRAM 上的走降级子链（决策后终止于 `emit_migrate`），
     非 DRAM 的走晋升子链（先过滤候选，最后才变更）——这个分叉写法本身是
     ADR-09 修复的一部分：旧的单链设计会在降级后过滤晋升条件时，把已经
     执行过降级的 item 丢出结果之外。
   - `composite_lru`/`tinylfu` 预设分别复刻热度阶梯衰减双通道 / CMS+
     Doorkeeper 频率估计的原子操作链。
3. `nf_exec_run()` 的结果是图上所有终止（无出边）节点输出的并集；
   `nf_bridge_run()` 拿它跟入队时的原始驻留节点做 diff，对每个发生变化的
   item 调用桥接回调 `apply()`（`src/numa_flow.c`），后者调用
   `numa_migrate_key_by_name()`。
4. `numa_migrate_key_by_name()` 内部按 `val->type` 分派到对应的类型迁移适配器
   （STRING/HASH/LIST/SET/ZSET），完成「目标节点分配 → memcpy → 原子指针切换
   → 释放旧内存 → 更新 PREFIX 节点标记」。

## 6.4 场景：内存分配 / 释放（zmalloc / zfree）

**分配**：`zmalloc(size)` → `numa_config_get_best_node(size)` 按当前策略选出
目标节点 → `should_use_slab(size)` 判断走 slab（≤4KB，`numa_slab_alloc()`）还是
direct（`numa_alloc_onnode()`）→ 写入 16 字节 `numa_alloc_prefix_t` 前缀
（size/node/hotness 等）。

**释放**：`zfree(ptr)` → 从 `ptr` 前面读回 PREFIX → 按 `from_slab` 标志分派到
`numa_slab_free()`（原子位图标记空闲）或 `numa_free_onnode()`（direct 释放）→
更新分配统计。

## 6.5 场景：运行时切换迁移策略

`redis-cli NUMA FLOW DEFAULT composite_lru` → `numaCommand()` →
`numa_cmd_flow()` → 在 `caat`/`composite_lru`/`tinylfu`/`noop` 四个预设名里校验
参数 → `nf_strategy_build("composite_lru", ...)` 重新构造对应的原子操作 DAG →
替换 `default` 工作流条目当前挂载的图 → 返回 `OK`，无需重启进程，下一次
`numa_flow_cron()` 触发时就会跑新策略。加载自定义 DAG（GUI 编排导出的 JSON）走
`NUMA FLOW LOAD <name> <path.json> [interval_sec] [ADAPT]` 这条独立命令，不经过
预设名校验。`numa-migrate-config`／`NUMA CONFIG LOAD`（composite-lru JSON 热
加载）已随 ADR-08 整体移除；`composite_lru.json` 现在只是字段名参考，内核不再
读取它。

## 6.6 场景：压力权重更新（服务于分配决策）

`serverCron()` 每秒调用 `numa_config_update_pressure_weights()`：对每个节点读
`numaGetNodePressure()`（`/sys/.../meminfo`），换算成权重后 `atomicSet` 写入
`pressure_weights[]`；下一次 `zmalloc` 时 `numa_config_get_best_node()` 用
`atomicGet` 读取这份权重，影响分配目标的选择——这是一条纯只读/原子操作的路径，
不需要加锁。

---

以上 6 个场景覆盖了系统运行时最重要的路径；完整的、逐层展开的调用栈（包含模块
依赖关系图、写路径/读路径/迁移路径的数据流总结、以及线程安全分析）见
[`appendix/call-chain-reference.md`](appendix/call-chain-reference.md)。
