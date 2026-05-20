# 调用链与模块交互

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
│  │  │     MIGRATE │ CONFIG │ STRATEGY │ HELP                 │  │   │
│  │  └────────┬──────────────────────────┬────────────────────┘  │   │
│  │           │                          │                        │   │
│  │           ▼                          ▼                        │   │
│  │  ┌─────────────────┐        ┌──────────────────────┐         │   │
│  │  │ Key 迁移模块     │        │ 策略插槽框架          │         │   │
│  │  │ (key_migrate)   │        │ (strategy_slots)     │         │   │
│  │  │                 │        │   ┌──────────────┐   │         │   │
│  │  │ ├─ String 适配器│        │   │ Slot 0: Noop │   │         │   │
│  │  │ ├─ Hash 适配器  │        │   │ Slot 1: C-LRU│◄──┼─────┐   │   │
│  │  │ ├─ List 适配器  │        │   │ Slot 2: TLFU │   │     │   │   │
│  │  │ ├─ Set 适配器   │        │   │ Slot 3-15:   │   │     │   │   │
│  │  │ └─ ZSet 适配器  │        │   │   自定义扩展  │   │     │   │   │
│  │  └────────┬────────┘        └──────────┬───────────┘     │   │   │
│  │           │                            │                  │   │   │
│  │           └────────────┬───────────────┘                  │   │   │
│  │                        │                                  │   │   │
│  │                        ▼                                  │   │   │
│  │           ┌────────────────────────┐                      │   │   │
│  │           │  Composite LRU 策略     │                      │   │   │
│  │           │  ├─ 快速通道(候选池)    │                      │   │   │
│  │           │  └─ 兜底通道(渐进扫描)  │                      │   │   │
│  │           └────────────┬───────────┘                      │   │   │
│  │                        │                                  │   │   │
│  │           ┌────────────┼───────────┐                      │   │   │
│  │           ▼            ▼           ▼                      │   │   │
│  │  ┌────────────┐ ┌────────────┐ ┌────────────────────┐    │   │   │
│  │  │ 内存迁移    │ │ 可配置策略  │ │ JSON 配置加载       │    │   │   │
│  │  │ (migrate)  │ │ (config)   │ │                    │    │   │   │
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

## 启动调用链

```
main()
    │
    ├── initServer()
    │     │
    │     ├── loadServerConfig()
    │     │     │
    │     │     └── 解析 numa-enabled, numa-migrate-config 等参数
    │     │
    │     └── zmalloc_init()
    │
    ├── #ifdef HAVE_NUMA  (initServer 之后)
    │     │
    │     ├── numa_strategy_init()                // 初始化策略插槽框架
    │     │     ├── numa_strategy_register_noop()  // 注册 Slot 0
    │     │     ├── numa_composite_lru_register()  // 注册 Slot 1
    │     │     └── numa_tinylfu_register()        // 注册 Slot 2（默认禁用）
    │     │
    │     ├── numa_config_strategy_init()          // 初始化可配置分配策略
    │     │     └── numa_config_set_strategy(WEIGHTED_INTERLEAVE)  // 设为默认
    │     │
    │     ├── numa_key_migrate_init()              // 初始化 Key 迁移模块
    │     │
    │     ├── numa_bw_monitor_init()               // 初始化带宽监控
    │     │
    │     └── if (server.numa_migrate_config_file)
    │             │
    │             ├── composite_lru_load_config(path, &cfg)
    │             └── composite_lru_apply_config(strategy, &cfg)
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
    │     │     └── numa_strategy_run_all()            // 运行所有策略
    │     │             │
    │     │             ├── Slot 1: Composite LRU（默认启用）
    │     │             │       │
    │     │             │       ├── composite_lru_execute()
    │     │             │       │       │
    │     │             │       │       ├── 周期衰减
    │     │             │       │       │
    │     │             │       │       ├── 快速通道：处理候选池
    │     │             │       │       │       │
    │     │             │       │       │       └── numa_migrate_key_by_name()
    │     │             │       │       │
    │     │             │       │       └── 兜底通道：渐进扫描
    │     │             │       │               │
    │     │             │       │               └── numa_migrate_key_by_name()
    │     │             │       │
    │     │             │       └── 更新统计
    │     │             │
    │     │             └── Slot 2: TinyLFU（默认禁用，需手动启用）
    │     │                     │
    │     │                     ├── tinylfu_execute()
    │     │                     │       │
    │     │                     │       ├── 遍历候选环形缓冲区
    │     │                     │       ├── 重新估计频率（CMS + Doorkeeper）
    │     │                     │       └── 频率 ≥ 阈值 → numa_migrate_key_by_name()
    │     │                     │
    │     │                     └── 更新统计
    │     │
    │     └── (无 10 秒 compact 任务 — 旧版 numa_pool_try_compact 已移除)
    │
    └── 其他 Redis 内部任务
```

## Key 访问调用链

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
    │             ├── 绑定 db 到 Composite LRU（无条件赋值）
    │             │     composite_lru_data_t *data = clru->private_data;
    │             │     data->db = db;
    │             │
    │             ├── composite_lru_record_access(strategy, key->ptr, val, lru_clock)
    │             │       │                             ↑ SDS string
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
    │             │       ├── 5. 写回 PREFIX + 同步 key_heat_map
    │             │       │     numa_set_hotness(val, hotness)
    │             │       │     numa_set_last_access(val, lru_clock)
    │             │       │
    │             │       └── 6. 判断是否写入候选池
    │             │             if (首次越过阈值 && Key 在远程节点)
    │             │                 sds key_copy = sdsdup(key);
    │             │                 hot_candidates[idx].key = key_copy;
    │             │
    │             └── TinyLFU（Slot 2, 若启用）
    │                   │
    │                   ├── 绑定 db 到 TinyLFU（无条件赋值）
    │                   │     tinylfu_data_t *tlfu_data = tlfu->private_data;
    │                   │     tlfu_data->db = db;
    │                   │
    │                   └── tinylfu_record_access(tlfu, key->ptr, val, data_ptr)
    │                           │
    │                           ├── 1. Doorkeeper 检查
    │                           │     if (!dk_test(&doorkeeper, hash))
    │                           │         dk_add(&doorkeeper, hash)  // 首次访问
    │                           │         统计本地/远程访问
    │                           │
    │                           ├── 2. CMS 递增（通过 Doorkeeper 后）
    │                           │     cms_record(&cms, hash)
    │                           │
    │                           ├── 3. 频率估计
    │                           │     freq = cms_estimate(&cms, hash)
    │                           │
    │                           ├── 4. 迁移条件检查
    │                           │     if (freq >= threshold && 数据在远程节点)
    │                           │         ring_push(key, val, target_node, freq)
    │                           │
    │                           └── 5. 全局衰减检查
    │                                 if (total_ops >= reset_interval)
    │                                     cms_halve + dk_clear
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
    └── (热度在首次 GET 时由 composite_lru_record_access 初始化)
```

## Key 迁移调用链

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

### 自动迁移（Composite LRU 触发）

```
serverCron()  // 每秒
    │
    ▼
numa_strategy_run_all()
    │
    └── composite_lru_execute(strategy)
            │
            ├── 快速通道（候选池处理）
            │     │
            │     └── for each candidate in hot_candidates:
            │             │
            │             ├── 重读 PREFIX 当前热度
            │             │     cur_hotness = numa_get_hotness(cand->val)
            │             │     mem_node = numa_get_node_id(cand->val)
            │             │
            │             ├── 带宽感知门槛调整
            │             │     if (src_bw > 0.7) effective_threshold--
            │             │
            │             ├── 检查资源状态
            │             │     status = check_resource_status(cand->target_node)
            │             │
            │             ├── 满足条件 ──► 迁移
            │             │     numa_migrate_key_by_name(data->db, cand->key, cand->target_node)
            │             │
            │             ├── 带宽饱和 ──► data->migrations_bw_blocked++
            │             ├── 过载/压力 ──► data->migrations_overloaded++
            │             │
            │             └── 释放 SDS 副本
            │                   sdsfree(cand->key)
            │                   cand->key = NULL
            │
            └── 兜底通道（渐进扫描 key_heat_map）
                  │
                  └── composite_lru_scan_once()
                          │
                          ├── 扫描 key_heat_map（每次 batch_size 个条目）
                          │     │
                          │     ├── 热度 >= 阈值 且 在远程
                          │     │     └── numa_migrate_key_by_name(data->db, dictGetKey(de), preferred_node)
                          │     │
                          │     └── 冷 Key 在本地 且 压力高
                          │           └── numa_migrate_key_by_name(data->db, dictGetKey(de), remote_node)
                          │
                          └── 更新扫描统计
```

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

## 内存分配调用链

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
    │     │             ├── 二分查找 size class (24 级)
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

## 内存释放调用链

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

## 配置加载调用链

```
redis-cli NUMA CONFIG LOAD /path/to/composite_lru.json
    │
    ▼
numaCommand(c)
    │
    ├── numa_cmd_config(c)
    │     │
    │     └── numa_cmd_config_load(c)
    │             │
    │             ├── path = c->argv[3]->ptr
    │             │
    │             ├── composite_lru_load_config(path, &cfg)
    │             │     │
    │             │     ├── 打开 JSON 文件
    │             │     ├── 逐行解析 key=value
    │             │     └── 验证参数范围
    │             │
    │             └── composite_lru_apply_config(strategy, &cfg)
    │                     │
    │                     ├── 重建候选池（如大小变化）
    │                     ├── 应用新配置
    │                     └── 重置扫描游标
    │
    └── 返回 OK
```

## 模块依赖关系

```
numa_command.c
    ├── numa_key_migrate.c
    │     ├── numa_migrate.c
    │     └── numa_composite_lru.c
    │           └── numa_strategy_slots.c
    │
    ├── numa_tinylfu.c
    │     └── numa_strategy_slots.c
    │
    ├── numa_configurable_strategy.c
    │     └── numaGetNodePressure() (evict.h)
    │
    └── numa_strategy_slots.c
          ├── numa_composite_lru.c
          ├── numa_tinylfu.c
          └── (自定义策略)

numa_pool.c  (Slab 分配器)
    └── libnuma (系统库)

zmalloc.c
    ├── numa_pool.c (Slab 分配器)
    ├── numa_configurable_strategy.c (节点选择)
    └── numa_composite_lru.c (热度接口)
```

## 数据流总结

### 写路径

```
客户端 SET ──► zmalloc ──► numa_config_get_best_node() ──► Slab/Direct 分配 ──► 写入 PREFIX ──► 存入 DB
```

### 读路径

```
客户端 GET ──► lookupKey ──► Slot 1: composite_lru_record_access() ──► 更新 PREFIX 热度 ──► 可能写入候选池
                          ──► Slot 2: tinylfu_record_access()（若启用）──► Doorkeeper → CMS ──► 可能入队环形缓冲区
```

### 迁移路径

```
serverCron ──► Slot 1: Composite LRU ──► 选择候选 Key (SDS name)
    ──► numa_migrate_key_by_name ──► dictFind ──► 类型适配器 ──► 更新指针 ──► 释放旧内存

serverCron ──► Slot 2: TinyLFU（若启用）──► 遍历候选环形缓冲区 ──► 重估频率
    ──► numa_migrate_key_by_name ──► dictFind ──► 类型适配器 ──► 更新指针 ──► 释放旧内存
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
