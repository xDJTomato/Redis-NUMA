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
│  │  │ ├─ List 适配器  │        │   │ Slot 2-15:   │   │     │   │   │
│  │  │ ├─ Set 适配器   │        │   │   自定义扩展  │   │     │   │   │
│  │  │ └─ ZSet 适配器  │        │   └──────────────┘   │     │   │   │
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
│  │  │    NUMA 内存池 (numa_pool)   │                          │   │   │
│  │  │  ├─ Slab 分配器 (≤128B)     │                          │   │   │
│  │  │  ├─ Pool 分配器 (≤4KB)      │                          │   │   │
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
    │     ├── zmalloc_init()
    │     │
    │     └── #ifdef HAVE_NUMA
    │             │
    │             ├── numa_pool_init()              // 初始化内存池
    │             ├── numa_slab_init()              // 初始化 Slab 分配器
    │             ├── numa_migrate_init()           // 初始化迁移模块
    │             ├── numa_key_migrate_init()       // 初始化 Key 迁移
    │             ├── numa_config_strategy_init()   // 初始化可配置策略
    │             │
    │             └── numa_strategy_init()          // 初始化策略插槽
    │                     │
    │                     ├── numa_strategy_register_noop()
    │                     │       └── 注册 Slot 0
    │                     │
    │                     ├── numa_strategy_register_composite_lru()
    │                     │       └── 注册 Slot 1
    │                     │
    │                     ├── numa_strategy_slot_insert(0, "noop")
    │                     └── numa_strategy_slot_insert(1, "composite-lru")
    │
    ├── #ifdef HAVE_NUMA
    │     │
    │     └── if (server.numa_migrate_config_file)
    │             │
    │             ├── composite_lru_load_config(path, &cfg)
    │             └── composite_lru_apply_config(strategy, &cfg)
    │
    └── aeMain()  // 进入事件循环
```

## serverCron 调用链

```
serverCron()  // 每 100ms 执行一次
    │
    ├── #ifdef HAVE_NUMA
    │     │
    │     ├── run_with_period(1000)  // 每秒
    │     │     │
    │     │     └── numa_strategy_run_all()
    │     │             │
    │     │             ├── Slot 0: Noop (跳过)
    │     │             │
    │     │             └── Slot 1: Composite LRU
    │     │                     │
    │     │                     ├── composite_lru_execute()
    │     │                     │       │
    │     │                     │       ├── 周期衰减
    │     │                     │       │
    │     │                     │       ├── 快速通道：处理候选池
    │     │                     │       │       │
    │     │                     │       │       └── numa_migrate_single_key()
    │     │                     │       │
    │     │                     │       └── 兜底通道：渐进扫描
    │     │                     │               │
    │     │                     │               └── numa_migrate_single_key()
    │     │                     │
    │     │                     └── 更新统计
    │     │
    │     └── run_with_period(10000)  // 每 10 秒
    │             │
    │             └── numa_pool_try_compact()  // 压缩低利用率 Chunk
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
    │             └── composite_lru_record_access(strategy, key, val)
    │                     │
    │                     ├── 1. 读取 PREFIX 热度
    │                     │     hotness = numa_get_hotness(val)
    │                     │
    │                     ├── 2. 计算空闲时间
    │                     │     idle = now - numa_get_last_access(val)
    │                     │
    │                     ├── 3. 阶梯式惰性衰减
    │                     │     decay = calculate_decay(idle)
    │                     │     if (hotness > decay) hotness -= decay
    │                     │     else hotness = 0
    │                     │
    │                     ├── 4. 热度 +1
    │                     │     if (hotness < 7) hotness++
    │                     │
    │                     ├── 5. 写回 PREFIX
    │                     │     numa_set_hotness(val, hotness)
    │                     │     numa_set_last_access(val, now)
    │                     │
    │                     └── 6. 判断是否写入候选池
    │                           if (首次越过阈值 && 远程节点)
    │                               add_to_candidates(key, val, target_node, hotness)
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
    │     │     ├── node = get_current_numa_node()
    │     │     │
    │     │     ├── if (should_use_slab(size))
    │     │     │       └── numa_slab_alloc(size, node, &total_size)
    │     │     │
    │     │     ├── else if (size <= NUMA_POOL_MAX_ALLOC)
    │     │     │       └── numa_pool_alloc(size, node, &total_size)
    │     │     │
    │     │     └── else
    │     │             └── numa_alloc_onnode(size + PREFIX, node)
    │     │
    │     ├── 写入 PREFIX
    │     │     prefix->size = size
    │     │     prefix->node_id = node
    │     │     prefix->hotness = 0
    │     │
    │     └── 设置 Key-Value
    │           dbAdd(db, key, val)
    │
    └── #ifdef HAVE_NUMA
            │
            └── numa_record_key_access(key, val)  // 记录访问
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
    │                     │     ├── 目标节点分配新内存
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
            ├── 快速通道
            │     │
            │     └── process_candidates(data)
            │             │
            │             ├── 遍历候选池
            │             │     │
            │             │     ├── 重读 PREFIX 当前热度
            │             │     │
            │             │     ├── 检查资源状态
            │             │     │
            │             │     └── 满足条件 ──► numa_migrate_single_key()
            │             │
            │             └── 清空候选池
            │
            └── 兜底通道
                  │
                  └── composite_lru_scan_once()
                          │
                          ├── 扫描 key_heat_map
                          │     │
                          │     ├── 评估热度
                          │     │
                          │     └── 满足条件 ──► numa_migrate_single_key()
                          │
                          └── 更新扫描统计
```

## 内存分配调用链

```
zmalloc(size)
    │
    ├── #ifdef HAVE_NUMA
    │     │
    │     ├── node = get_current_numa_node()
    │     │     │
    │     │     ├── t_numa_node (线程本地)
    │     │     ├── sched_getcpu() → cpu_to_node()
    │     │     └── numa_interleave_node()
    │     │
    │     ├── if (should_use_slab(size))
    │     │     │
    │     │     └── numa_slab_alloc(size, node, &total_size)
    │     │             │
    │     │             ├── 计算 size_class
    │     │             ├── 查找空闲 slab
    │     │             ├── 原子位图分配
    │     │             └── 写入 PREFIX
    │     │
    │     ├── else if (size <= NUMA_POOL_MAX_ALLOC)
    │     │     │
    │     │     └── numa_pool_alloc(size, node, &total_size)
    │     │             │
    │     │             ├── 检查 Free List
    │     │             ├── Bump Pointer 分配
    │     │             ├── 或分配新 Chunk
    │     │             └── 写入 PREFIX
    │     │
    │     └── else
    │             │
    │             └── numa_alloc_onnode(size + PREFIX, node)
    │                     │
    │                     └── 写入 PREFIX
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
    │     from_pool = prefix->from_pool
    │     node_id = prefix->node_id
    │     size = prefix->size
    │
    ├── switch (from_pool)
    │     │
    │     ├── case 2 (Slab):
    │     │     └── numa_slab_free(ptr, size, node_id)
    │     │             │
    │     │             └── 原子位图标记空闲
    │     │
    │     ├── case 1 (Pool):
    │     │     └── numa_pool_free(ptr, size, 1)
    │     │             │
    │     │             └── 加入 Free List
    │     │
    │     └── case 0 (Direct):
    │             └── numa_free(prefix, size + PREFIX)
    │
    └── 更新统计
          update_zmalloc_stat_free(size + PREFIX)
```

## Key 删除调用链

```
客户端: DEL user:100
    │
    ▼
delCommand(c)
    │
    └── dbDelete(db, key)
            │
            ├── #ifdef HAVE_NUMA
            │     │
            │     └── numa_on_key_delete(key)
            │             │
            │             └── dictDelete(key_metadata, key)
            │
            ├── dbGenericDelete(db, key, 0)
            │     │
            │     └── dictDelete(db->dict, key)
            │             │
            │             └── dictFreeVal(val)  // 释放 value
            │                     │
            │                     └── zfree(val->ptr)
            │
            └── 通知从节点
```

## 配置加载调用链

```
redis-cli NUMA CONFIG LOAD /path/to/config.json
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
    │             │     ├── 逐行解析
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
    ├── numa_configurable_strategy.c
    │     └── numa_pool.c
    │
    └── numa_strategy_slots.c
          ├── numa_composite_lru.c
          └── (自定义策略)

numa_pool.c
    └── libnuma (系统库)

zmalloc.c
    ├── numa_pool.c
    └── numa_composite_lru.c (热度接口)
```

## 数据流总结

### 写路径

```
客户端 SET ──► zmalloc ──► 选择分配路径 ──► 写入 PREFIX ──► 存入 DB
```

### 读路径

```
客户端 GET ──► lookupKey ──► record_access ──► 更新热度 ──► 可能写入候选池
```

### 迁移路径

```
serverCron ──► Composite LRU ──► 选择候选 Key ──► 迁移适配器 ──► 更新指针 ──► 释放旧内存
```

### 监控路径

```
客户端 NUMA MIGRATE STATS ──► 读取全局统计 ──► 返回结果
```

## 线程安全分析

### 单线程部分

- 所有 Redis 命令处理（主线程）
- serverCron 调用
- Key 迁移执行

### 多线程部分

- 内存池的每个 size_class 有独立锁
- 策略管理器有全局锁
- Key 元数据字典有独立锁

### 并发安全

```
线程 A (主线程): 处理客户端命令 ──► lookupKey ──► record_access
                                                     │
线程 B (主线程): 执行 serverCron ──► numa_strategy_run_all ──► migrate
                                                     │
                                                     └── 安全：串行执行
```

由于 Redis 是单线程模型，所有操作在主线程中串行执行，不存在并发冲突。
