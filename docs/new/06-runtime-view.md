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
   - `numa_strategy_init()`（`src/server.c:7371`）：注册槎位 0（no-op）、
     槎位 1（`numa_composite_lru_register()`）、槎位 2（`numa_tinylfu_register()`，
     默认禁用）。
   - `numa_config_strategy_init()`（`src/server.c:7384`），随后
     `numa_config_set_strategy(WEIGHTED_INTERLEAVE)`（`src/server.c:7385`）
     把默认分配策略设为加权轮询插值。
   - `numa_key_migrate_init()`（`src/server.c:7390`）。
   - `numa_bw_monitor_init()`（`src/server.c:7395`）。
   - 若配置了 `numa-migrate-config`：`composite_lru_load_config()` 读 JSON，
     `composite_lru_apply_config()` 应用到槎位 1 的运行时状态
     （`src/server.c:7408-7410`）。
3. 进入 `aeMain()` 事件循环，NUMA 模块此后只在 `serverCron`（或槎位若切到
   AE 调度模式，见 [`modules/ae_strategy_scheduler.md`](modules/ae_strategy_scheduler.md)）
   和命令处理路径中被驱动。

要点：`numa_init()`／`numa_slab_init()` 必须先于 `initServer()` 建立的状态完成，
而策略/迁移/带宽监控模块的初始化必须晚于 `initServer()`——顺序颠倒是启动期崩溃
最常见的原因（见 [`08-crosscutting-concepts.md`](08-crosscutting-concepts.md)）。

## 6.2 场景：GET 命令触发热度追踪

1. 客户端发 `GET user:100`，`getCommand()` → `lookupKeyRead()` → `lookupKey()`
   在 `db->dict` 里找到 `val`。
2. `#ifdef HAVE_NUMA` 分支里，槎位 1（Composite LRU）调用
   `composite_lru_record_access(strategy, key, val, lru_clock)`：
   - 读出 PREFIX 里的当前热度与上次访问时间；
   - 按空闲时长做阶梯式惰性衰减；
   - 热度 +1（上限 7）；
   - 若热度首次越过阈值且数据当前在远端节点，把这个 key 的 SDS 拷贝写入
     「热候选池」（快路径的输入）。
3. 若槎位 2（TinyLFU）也启用，`tinylfu_record_access()` 并行做一套独立的
   Doorkeeper → CMS 频率估计，满足阈值时把 key 推入自己的环形缓冲区。
4. 返回值给客户端——整个追踪过程不阻塞、不改变返回结果，只更新 PREFIX 元数据。

## 6.3 场景：serverCron 驱动的自动迁移

1. `serverCron()` 每秒调用 `numa_strategy_run_all()`。
2. 槎位 1 执行 `composite_lru_execute()`：
   - 快路径：遍历上一节写入的热候选池，重读当前热度、按源节点带宽调整生效
     阈值、检查目标节点资源状态，满足条件则调用
     `numa_migrate_key_by_name()`；
   - 慢路径：`composite_lru_scan_once()` 按批次渐进扫描 `key_heat_map`，
     捕获快路径没覆盖到的冷门热 key 或需要降级的冷 key。
3. 槎位 2（若启用）执行 `tinylfu_execute()`：遍历候选环形缓冲区，用 CMS 重新
   估计频率，达到阈值同样调用 `numa_migrate_key_by_name()`。
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

## 6.5 场景：配置热加载

`redis-cli NUMA CONFIG LOAD /path/to/composite_lru.json` → `numaCommand()` →
`numa_cmd_config()` → `numa_cmd_config_load()`：读取并解析 JSON → 校验参数范围
（`composite_lru_load_config()`）→ `composite_lru_apply_config()` 把新参数应用
到槎位 1 的运行时状态（候选池大小变化时重建，重置扫描游标）→ 返回 `OK`，无需
重启进程。

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
