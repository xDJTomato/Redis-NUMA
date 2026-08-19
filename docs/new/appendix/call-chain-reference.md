# 附录：完整调用链参考

> 本附录是 [`../06-runtime-view.md`](../06-runtime-view.md) 中 6 个精简场景的完整
> 展开版——如果你需要逐层查看某条调用栈的每一步、模块依赖关系图，或线程安全分析
> 的完整依据，来这里查；如果只是想理解系统整体怎么运作，读 `06-runtime-view.md`
> 就够了。内容取自本项目早期的 `10-call-chain.md`，本次随 arc42 重构迁移到此处并
> 重新核对过所有函数名/文件路径。

## 模块全景图

```
┌─────────────────────────────────────────────────────────────────────┐
│                          Redis Core                                 │
│                                                                     │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌───────┐ │
│  │ server.c │  │  db.c    │  │ config.c │  │ zmalloc.c│  │lazyfree│ │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └────┬─────┘  └───┬───┘ │
│       │              │             │              │             │     │
└───────┼──────────────┼─────────────┼──────────────┼─────────────┼─────┘
        │              │             │              │             │
┌───────┼──────────────┼─────────────┼──────────────┼─────────────┼─────┐
│       ▼              ▼             ▼              ▼             ▼     │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                   NUMA 模块层                                 │   │
│  │                                                              │   │
│  │  ┌────────────────────────────────────────────────────────┐  │   │
│  │  │           统一命令接口 (numa_command.c)                 │  │   │
│  │  │     MIGRATE │ CONFIG │ FLOW │ HELP                     │  │   │
│  │  └────────┬──────────────────────────┬────────────────────┘  │   │
│  │           │                          │                        │   │
│  │           ▼                          ▼                        │   │
│  │  ┌─────────────────┐        ┌──────────────────────┐         │   │
│  │  │ Key 迁移模块     │        │ NUMAflow 桥接         │         │   │
│  │  │ (key_migrate)   │        │ (numa_flow.c)         │         │   │
│  │  │                 │        │  enumerate()/apply()  │         │   │
│  │  │ ├─ String 适配器│        │  回调 ──────────────┐  │         │   │
│  │  │ ├─ Hash 适配器  │        │                      │  │         │   │
│  │  │ ├─ List 适配器  │        │  default 工作流：    │  │         │   │
│  │  │ ├─ Set 适配器   │        │  caat / composite_lru│  │         │   │
│  │  │ └─ ZSet 适配器  │        │  / tinylfu / noop    │  │         │   │
│  │  └────────┬────────┘        └──────────┬───────────┘     │   │   │
│  │           │                            │                  │   │   │
│  │           └────────────┬───────────────┘                  │   │   │
│  │                        │                                  │   │   │
│  │                        ▼                                  │   │   │
│  │           ┌────────────────────────┐                      │   │   │
│  │           │  NUMAflow 原子操作 DAG  │                      │   │   │
│  │           │  （numaflow/src/       │                      │   │   │
│  │           │   nf_strategy.c 预设） │                      │   │   │
│  │           └────────────┬───────────┘                      │   │   │
│  │                        │                                  │   │   │
│  │           ┌────────────┼───────────┐                      │   │   │
│  │           ▼            ▼           ▼                      │   │   │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────────────┐    │   │   │
│  │  │ 内存迁移    │ │ 可配置策略  │ │ NUMA FLOW LOAD      │    │   │   │
│  │  │ (migrate)  │ │ (config)   │ │ （自定义 DAG JSON） │    │   │   │
│  │  └─────┬──────┘ └─────┬──────┘ └────────────────────┘    │   │   │
│  │        │              │                                   │   │   │
│  └────────┼──────────────┼───────────────────────────────────┘   │   │
│           │              │                                       │   │
│  ┌────────┼──────────────┼───────────────────────────────────┐   │   │
│  │        ▼              ▼                                   │   │   │
│  │  ┌─────────────────────────────┐                          │   │   │
│  │  │    NUMA Slab 分配器          │                          │   │   │
│  │  │  ├─ Slab 分配器 (≤4KB)      │                          │   │   │
│  │  │  └─ Direct 分配 (>4KB)      │                          │   │   │
│  │  └──────────────┬──────────────┘                          │   │   │
│  │                 │                                         │   │   │
│  └─────────────────┼─────────────────────────────────────────┘   │   │
│                    │                                             │   │
└────────────────────┼─────────────────────────────────────────────┘   │
                     │                                                 │
                     ▼                                                 ▼
          ┌──────────────────┐                              ┌──────────────┐
          │  NUMA 节点 0      │                              │  NUMA 节点 1  │
          │  (本地 DRAM)     │◄──── 跨节点迁移 ────────────►│  (本地/CXL)   │
          └──────────────────┘                              └──────────────┘
```

## 启动调用链（完整版）

```
main()
    │
    ├── initServer()
    │     │
    │     ├── loadServerConfig()
    │     │     │
    │     │     └── 解析 numa-enabled, numa-flow-default-strategy,
    │     │           numa-flow-interval-sec 等参数
    │     │
    │     └── zmalloc_init()
    │             └── numa_slab_init()   // src/numa_pool.c:312，由 zmalloc.c:94 调用
    │
    ├── #ifdef HAVE_NUMA  (initServer 之后)
    │     │
    │     ├── numa_config_strategy_init()          // src/server.c:7384
    │     │     └── numa_config_set_strategy(WEIGHTED_INTERLEAVE)  // src/server.c:7385，设为默认
    │     │
    │     ├── numa_key_migrate_init()              // src/server.c:7390
    │     │
    │     ├── numa_bw_monitor_init()               // src/server.c:7395
    │     │
    │     └── numa_flow_init()
    │             │
    │             └── if (numa-enabled)  // 默认策略自动加载
    │                     numa_flow_load_default(numa-flow-default-strategy,
    │                                             numa-flow-interval-sec)
    │                         └── nf_strategy_build(&graph, "caat" 等)
    │                               把对应预设登记为 `default` 工作流条目
    │
    └── aeMain()  // 进入事件循环
```

> **注意**：`numa_slab_init()` 由 `zmalloc.c:zmalloc_init()` → `numa_init()` 调用，不在 `server.c` 中。

## serverCron 调用链

```
serverCron()  // 每 100ms 执行一次
    │
    ├── #ifdef HAVE_NUMA
    │     │
    │     ├── run_with_period(1000)  // 每秒
    │     │     │
    │     │     ├── numa_bw_monitor_sample()           // 带宽采样
    │     │     │
    │     │     ├── numa_config_update_pressure_weights()  // 更新压力权重
    │     │     │     │
    │     │     │     └── for each node:
    │     │     │           p = numaGetNodePressure(i)  // 读 /sys/.../meminfo
    │     │     │           w = max(1, (1 - p) * 100)
    │     │     │           atomicSet(pressure_weights[i], w)
    │     │     │
    │     │     └── numa_flow_cron()            // 驱动所有已加载的 NUMAflow 工作流
    │     │             │
    │     │             └── for each loaded entry (至少有 `default`):
    │     │                   if (now - last_run >= interval_sec)
    │     │                       numa_flow_run_entry(entry)
    │     │                           │
    │     │                           ├── dictGetSafeIterator(db->dict)
    │     │                           ├── 构造 nf_bridge_t：
    │     │                           │     enumerate = numa_flow_enumerate
    │     │                           │     apply     = numa_flow_apply
    │     │                           │     ctx.budget = 256
    │     │                           │
    │     │                           └── nf_bridge_run(&br, &entry->graph, &result)
    │     │                                   │
    │     │                                   ├── 对每个枚举出的 key 跑该工作流的
    │     │                                   │     原子操作 DAG（见下方"自动迁移"）
    │     │                                   │
    │     │                                   └── 结果（终止节点输出的并集）与
    │     │                                         入队时的原始节点做 diff，
    │     │                                         对变化项调用 apply()
    │     │                                             └── numa_migrate_key_by_name()
    │     │
    │     └── (无 10 秒 compact 任务 — 旧版 numa_pool_try_compact 已移除)
    │
    └── 其他 Redis 内部任务
```

## Key 访问调用链（完整版）

### GET 命令

```
客户端: GET user:100
    │
    ▼
getCommand(c)
    │
    ▼
lookupKeyRead(c->db, c->argv[1], flags)
    │
    ├── lookupKey(db, key, flags)
    │     │
    │     ├── dictFind(db->dict, key)
    │     │
    │     └── #ifdef HAVE_NUMA
    │             │
    │             ├── numa_key_migrate_touch(val)     // 无条件调用，不再判断任何策略是否 enabled
    │             │       │
    │             │       ├── 1. 读取 PREFIX 热度
    │             │       │     hotness = numa_get_hotness(val)
    │             │       │
    │             │       ├── 2. 计算空闲时间
    │             │       │     idle = now - numa_get_last_access(val)
    │             │       │
    │             │       ├── 3. 阶梯式惰性衰减
    │             │       │     decay = calculate_decay(idle)
    │             │       │     if (hotness > decay) hotness -= decay
    │             │       │     else hotness = 0
    │             │       │
    │             │       ├── 4. 热度 +1
    │             │       │     if (hotness < 7) hotness++
    │             │       │
    │             │       └── 5. 写回 PREFIX
    │             │             numa_set_hotness(val, hotness)
    │             │             numa_set_last_access(val, lru_clock)
    │             │             ↑ 这是 NUMAflow enumerate() 读取的唯一 ground truth
    │             │
    │             └── numa_flow_observe_access(key->ptr)   // src/numa_flow.c
    │                     │
    │                     └── nf_tracker_observe(&g_tracker, key)
    │                             │
    │                             ├── Doorkeeper 检查：首次访问只记入 Doorkeeper
    │                             ├── CMS 递增（通过 Doorkeeper 后）
    │                             └── 全局衰减：total_ops 达到 reset_interval 时
    │                                   cms_halve + dk_clear
    │                     ↑ 这是 Redis 桥接里唯一调用 nf_tracker_observe() 的地方
    │                       （修复前完全缺失，见 ADR-09：TinyLFU/CAAT 的
    │                       cms_estimate 曾永远读到 0）
    │
    └── 返回 Value 给客户端
```

### SET 命令

```
客户端: SET user:100 "value"
    │
    ▼
setCommand(c)
    │
    ├── setGenericCommand()
    │     │
    │     ├── zmalloc(size)  // 分配新内存
    │     │     │
    │     │     ├── node = numa_config_get_best_node(size)
    │     │     │
    │     │     ├── if (should_use_slab(size))  // size ≤ 4KB
    │     │     │       └── numa_slab_alloc(size, node, &total_size)
    │     │     │
    │     │     └── else  // size > 4KB
    │     │             └── numa_alloc_onnode(size + PREFIX, node)
    │     │
    │     ├── 写入 PREFIX
    │     │     prefix->size = size
    │     │     prefix->from_slab = (size <= 4096) ? 1 : 0
    │     │     prefix->node_id = node
    │     │     prefix->hotness = 0
    │     │
    │     └── 设置 Key-Value
    │           dbAdd(db, key, val)
    │
    └── (热度在首次 GET 时由 numa_key_migrate_touch 初始化)
```

## Key 迁移调用链（完整版）

### 手动迁移

```
客户端: NUMA MIGRATE KEY user:100 1
    │
    ▼
numaCommand(c)
    │
    ├── numa_cmd_migrate(c)
    │     │
    │     └── numa_cmd_migrate_key(c)
    │             │
    │             ├── key = c->argv[3]
    │             ├── target_node = atoi(c->argv[4]->ptr)
    │             │
    │             └── numa_migrate_single_key(db, key, target_node)
    │                     │
    │                     ├── 1. 查找 Key
    │                     │     val = lookupKeyRead(db, key)
    │                     │
    │                     ├── 2. 获取当前节点
    │                     │     current_node = numa_get_key_current_node(val)
    │                     │     └── 已在目标节点 ──► 返回 OK
    │                     │
    │                     ├── 3. 根据类型选择适配器
    │                     │     switch (val->type) {
    │                     │         case OBJ_STRING:  migrate_string_type()
    │                     │         case OBJ_HASH:    migrate_hash_type()
    │                     │         case OBJ_LIST:    migrate_list_type()
    │                     │         case OBJ_SET:     migrate_set_type()
    │                     │         case OBJ_ZSET:    migrate_zset_type()
    │                     │     }
    │                     │
    │                     ├── 4. 执行迁移
    │                     │     ├── 目标节点分配新内存（直接调用适配器）
    │                     │     ├── 复制数据
    │                     │     ├── 原子指针切换
    │                     │     └── 释放旧内存
    │                     │
    │                     ├── 5. 更新 PREFIX
    │                     │     numa_set_key_node(val, target_node)
    │                     │
    │                     └── 6. 更新统计
    │                           stats.successful_migrations++
    │
    └── 返回 OK/ERR
```

### 自动迁移（NUMAflow `default` 工作流触发，默认 CAAT）

```
serverCron()  // 每秒
    │
    ▼
numa_flow_cron()
    │
    └── numa_flow_run_entry(default 工作流)
            │
            ├── enumerate() 枚举 db->dict 里的每个 key（读 PREFIX 热度/节点信息）
            │
            └── nf_bridge_run() 驱动该工作流的原子操作 DAG——以默认的 CAAT
                  预设（`build_caat`，numaflow/src/nf_strategy.c）为例：
                  │
                  ├── 打分（对每个候选 key 都跑一遍）
                  │     cms_estimate ──► score_cost_benefit
                  │
                  ├── 按原始驻留位置分叉（ADR-09 修复点：必须在任何一侧
                  │     发生变更之前分叉，否则被降级但没通过晋升过滤的
                  │     item 会从结果里丢失）
                  │     │
                  │     ├── filter_local（node=0，即在 DRAM 上）
                  │     │     └── 降级子链：demote_cold(threshold=1)
                  │     │           ──► emit_migrate（终止节点，唯一一次变更）
                  │     │
                  │     └── filter_remote（node=0，即不在 DRAM 上）
                  │           └── 晋升子链：filter_freq(threshold=1)
                  │                 ──► filter_benefit(threshold=0)
                  │                 ──► rank_cost
                  │                 ──► budget_limit(budget=512)
                  │                 ──► select_dest_node(require_benefit=1)
                  │                 ──► emit_migrate（终止节点，唯一一次变更）
                  │
                  └── nf_exec_run() 的结果 = 两个终止节点输出的并集
                        （每个 item 只经过其中一条子链，只被变更一次）
            │
            └── 结果与入队时的原始节点 diff，对变化项调用 apply()
                  └── numa_flow_apply() ──► numa_migrate_key_by_name()
```

`composite_lru`（`build_composite_lru`：`score_hotness → filter_hot(threshold=5)
→ budget_limit(budget=512) → select_dest_node → emit_migrate`，单链、只晋升
不降级）和 `tinylfu`（`build_tinylfu`：`cms_estimate → filter_freq(threshold=2)
→ budget_limit → select_dest_node → emit_migrate`，同样只晋升不降级）是另外
两个预设，通过 `NUMA FLOW DEFAULT composite_lru`/`tinylfu` 切换；`noop` 是空
图，不做任何迁移。

### migrate_string_type 内部流程

```
migrate_string_type(NULL, val_obj, target_node)
    │
    ├── 1. 获取 SDS 分配信息
    │     sds old_str = val_obj->ptr
    │     total = sdsAllocSize(old_str)       // 整个 SDS 块大小
    │     old_base = sdsAllocPtr(old_str)     // 含 PREFIX 的原始指针
    │     str_offset = old_str - old_base     // SDS 头偏移
    │
    ├── 2. 在目标节点分配新内存
    │     new_base = numa_zmalloc_onnode(total, target_node)
    │         └── if (should_use_slab(total))
    │                 └── numa_slab_alloc(total, target_node)
    │             else
    │                 └── numa_alloc_onnode(total + PREFIX_SIZE, target_node)
    │
    ├── 3. 完整复制
    │     memcpy(new_base, old_base, total)
    │
    ├── 4. 重算 SDS 指针 + 原子切换
    │     new_str = new_base + str_offset
    │     val_obj->ptr = new_str              // 此刻生效
    │
    ├── 5. 释放旧内存
    │     sdsfree(old_str)
    │         └── zfree → numa_free_with_size
    │             └── 读 old PREFIX: from_slab → 路由到 Slab 或 Direct
    │
    └── 6. 更新节点标记
          numa_set_node_id(val_obj, target_node)
```

## 内存分配调用链（完整版）

```
zmalloc(size)
    │
    ├── #ifdef HAVE_NUMA
    │     │
    │     ├── node = numa_config_get_best_node(size)
    │     │     │
    │     │     └── 根据当前策略选择节点
    │     │         (默认 WEIGHTED_INTERLEAVE，读 atomicGet(pressure_weights))
    │     │
    │     ├── if (should_use_slab(size))  // size ≤ 4KB
    │     │     │
    │     │     └── numa_slab_alloc(size, node, &total_size)
    │     │             │
    │     │             ├── 二分查找 size class (33 级)
    │     │             ├── 遍历 partial_slabs，原子 CAS 分配
    │     │             └── 写入 PREFIX (from_slab=1)
    │     │
    │     └── else  // size > 4KB
    │             │
    │             └── numa_alloc_onnode(size + PREFIX, node)
    │                     │
    │                     └── 写入 PREFIX (from_slab=0)
    │
    └── #else
            │
            └── malloc(size + PREFIX)
```

## 内存释放调用链（完整版）

```
zfree(ptr)
    │
    ├── 找回 PREFIX
    │     prefix = (numa_alloc_prefix_t *)ptr - 1
    │
    ├── 读取元数据
    │     from_slab = prefix->from_slab
    │     node_id = prefix->node_id
    │     size = prefix->size
    │
    ├── switch (from_slab)
    │     │
    │     ├── case 1 (Slab):
    │     │     └── numa_slab_free(ptr, total_size, node_id)
    │     │             │
    │     │             └── 原子位图标记空闲
    │     │
    │     └── case 0 (Direct):
    │             └── numa_free(prefix, size + PREFIX)
    │                     │
    │                     └── numa_free_onnode(prefix, size + PREFIX, node_id)
    │
    └── 更新统计
          update_zmalloc_stat_free(size + PREFIX)
```

## 运行时切换迁移策略调用链（完整版）

```
redis-cli NUMA FLOW DEFAULT composite_lru
    │
    ▼
numaCommand(c)
    │
    ├── numa_flow_command(c)   // src/numa_flow.c，NUMA FLOW 域整体路由（不在 numa_command.c 里）
    │     │
    │     └── numa_flow_set_default(name)
    │             │
    │             ├── name 校验属于 {caat, composite_lru, tinylfu, noop}
    │             │
    │             ├── nf_strategy_build(&new_graph, name)
    │             │     └── 按预设名重新构造对应的原子操作 DAG
    │             │           （见上方"自动迁移"一节列出的三条链）
    │             │
    │             └── 用 new_graph 替换 `default` 工作流条目当前挂载的图
    │
    └── 返回 OK  // 无需重启，下一次 numa_flow_cron() 触发时就跑新策略
```

加载自定义 DAG（GUI 编排导出的 JSON，不经过预设名校验）走独立的
`NUMA FLOW LOAD <name> <path.json> [interval_sec] [ADAPT]` 命令。
`numa-migrate-config` / `NUMA CONFIG LOAD`（composite-lru JSON 热加载）已随
ADR-08 整体移除；`composite_lru.json` 现在只是字段名参考，内核不再读取它。

## 模块依赖关系

```
numa_command.c
    ├── numa_key_migrate.c
    │     └── numa_migrate.c
    │
    ├── numa_flow.c                    // NUMAflow 桥接，FLOW 域独立路由
    │     ├── numaflow/src/nf_strategy.c   （caat/composite_lru/tinylfu/noop 预设）
    │     ├── numaflow/src/nf_bridge.c     （enumerate/apply 契约 + 执行）
    │     ├── numaflow/src/nf_track.c      （CMS + Doorkeeper 频率追踪器）
    │     └── numaflow/src/nf_adapt.c      （ADAPT 开关时的自适应调参/换图）
    │
    └── numa_configurable_strategy.c
          └── numa_bw_get_node_pressure() (numa_bw_monitor.c，与 evict_numa.c 共用)

numa_pool.c  (Slab 分配器)
    └── libnuma (系统库)

zmalloc.c
    ├── numa_pool.c (Slab 分配器)
    ├── numa_configurable_strategy.c (节点选择)
    └── numa_key_migrate.c (numa_key_migrate_touch()：热度接口)
```

## 数据流总结

### 写路径

```
客户端 SET ──► zmalloc ──► numa_config_get_best_node() ──► Slab/Direct 分配 ──► 写入 PREFIX ──► 存入 DB
```

### 读路径

```
客户端 GET ──► lookupKey ──► numa_key_migrate_touch() ──► 更新 PREFIX 热度（阶梯衰减 + 1）
                          ──► numa_flow_observe_access() ──► nf_tracker_observe()（Doorkeeper → CMS）
```

### 迁移路径

```
serverCron ──► numa_flow_cron() ──► default 工作流（默认 CAAT）
    ──► nf_bridge_run()：enumerate() 枚举 keyspace ──► 原子操作 DAG 打分/过滤/决策
    ──► apply() ──► numa_migrate_key_by_name() ──► dictFind ──► 类型适配器 ──► 更新指针 ──► 释放旧内存
```

### 压力权重更新路径

```
serverCron (每秒) ──► numa_config_update_pressure_weights()
    ──► numaGetNodePressure(i) ──► atomicSet(pressure_weights[i], w)
    ──► 下次 zmalloc 时 atomicGet 读取，影响分配目标选择
```

> **注意**：bw_benchmark 采集脚本使用 `NUMA MIGRATE STATS`（而非 `NUMA CONFIG GET`）获取 `successful_migrations` 计数。

## 线程安全分析

### 单线程部分

- 所有 Redis 命令处理（主线程）
- serverCron 调用
- Key 迁移执行

### 原子操作保护

- Slab 分配器：原子位图 CAS（无锁分配/释放）
- 策略统计计数器：`atomicIncr` / `atomicGet`（无锁）
- 压力权重：`atomicSet` 写入 / `atomicGet` 读取（无锁）
- 仅 WEIGHTED 策略短暂持锁复制权重数组

### 并发安全

```
线程 A (主线程): 处理客户端命令 ──► lookupKey ──► record_access
                                                     │
线程 B (主线程): 执行 serverCron ──► numa_strategy_run_all ──► migrate
                                                     │
                                                     └── 安全：串行执行
```

由于 Redis 是单线程模型，所有操作在主线程中串行执行，不存在并发冲突。
