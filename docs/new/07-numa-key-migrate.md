# Key 级别迁移

## 模块概述

`numa_key_migrate.c/h` 实现 Redis Key 在 NUMA 节点间的细粒度迁移功能。它以 `robj` 为迁移单元，支持 5 种 Redis 数据类型的专用迁移适配器，通过原子指针切换确保迁移过程的一致性。

## 核心特性

1. **Key 级别粒度**：单个 Key 即可迁移，无需整库迁移
2. **多类型适配**：String、Hash、List、Set、ZSet 各有专用迁移函数
3. **原子指针切换**：迁移过程对客户端透明
4. **完整统计追踪**：记录迁移次数、字节数、耗时等指标

## 数据结构

### Key 的 NUMA 元数据

```c
typedef struct {
    int current_node;               // 当前所在 NUMA 节点
    uint8_t hotness_level;          // 热度级别（0-7）
    uint16_t last_access_time;      // 上次访问时间（LRU 时钟）
    size_t memory_footprint;        // 内存占用大小（字节）
    uint64_t access_count;          // 累计访问次数
} key_numa_metadata_t;
```

### 迁移请求

```c
typedef struct {
    robj *key_obj;                  // 目标 Key 对象
    int source_node;                // 源 NUMA 节点
    int target_node;                // 目标 NUMA 节点
    size_t data_size;               // 待迁移数据大小
    uint64_t start_time;            // 迁移开始时间（微秒）
} migration_request_t;
```

### 迁移统计

```c
typedef struct {
    uint64_t total_migrations;              // 总迁移次数
    uint64_t successful_migrations;         // 成功迁移次数
    uint64_t failed_migrations;             // 失败迁移次数
    uint64_t total_bytes_migrated;          // 总迁移字节数
    uint64_t total_migration_time_us;       // 总迁移耗时（微秒）
    uint64_t peak_concurrent_migrations;    // 峰值并发迁移数
} numa_key_migrate_stats_t;
```

### 模块全局上下文

```c
typedef struct {
    int initialized;                        // 初始化标志
    dict *key_metadata;                     // Key 元数据哈希表
    pthread_mutex_t mutex;                  // 并发控制锁
    numa_key_migrate_stats_t stats;         // 迁移统计
} numa_key_migrate_ctx_t;
```

## 核心接口

### 单 Key 迁移流程图

```mermaid
graph TB
    A[numa_migrate_single_key] --> B[lookupKeyRead db, key]
    B --> C{Key 存在?}
    C -->|否| D[返回 ENOENT]
    C -->|是| E[numa_get_key_current_node]
    E --> F{已在目标节点?}
    F -->|是| G[返回 OK 无需迁移]
    F -->|否| H[getObjectSize]
    H --> I{val->type}
    
    I -->|STRING| J[migrate_string_type]
    I -->|HASH| K[migrate_hash_type]
    I -->|LIST| L[migrate_list_type]
    I -->|SET| M[migrate_set_type]
    I -->|ZSET| N[migrate_zset_type]
    
    J --> O[目标节点分配内存]
    K --> O
    L --> O
    M --> O
    N --> O
    
    O --> P[复制数据]
    P --> Q[原子指针切换]
    Q --> R[释放旧内存]
    R --> S[更新 PREFIX node_id]
    S --> T[更新统计]
    T --> U[返回 OK]
```

### 模块初始化

```c
int numa_key_migrate_init(void);
void numa_key_migrate_cleanup(void);
```

### 单 Key 迁移

```c
int numa_migrate_single_key(redisDb *db, robj *key, int target_node);
```

**参数**：
- `db`: Redis 数据库实例
- `key`: Key 对象（robj*）
- `target_node`: 目标 NUMA 节点 ID

**返回**：`NUMA_KEY_MIGRATE_OK` 或错误码

### 批量迁移

```c
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node);
```

### 模式迁移

```c
int numa_migrate_keys_by_pattern(redisDb *db, const char *pattern, int target_node);
```

支持通配符：`user:*`、`order:1???`

### 全库迁移

```c
int numa_migrate_entire_database(redisDb *db, int target_node);
```

## 数据类型迁移适配器

### String 类型

```c
int migrate_string_type(robj *key_obj, robj *val_obj, int target_node) {
    // 1. 计算内存大小
    size_t size = sdslen(val_obj->ptr) + 1;

    // 2. 在目标节点分配新内存
    void *new_ptr = zmalloc_onnode(size, target_node);

    // 3. 复制数据
    memcpy(new_ptr, val_obj->ptr, size);

    // 4. 原子指针切换
    val_obj->ptr = new_ptr;

    // 5. 释放旧内存
    zfree(old_ptr);

    // 6. 更新 PREFIX 中的 node_id
    numa_set_key_node(val_obj, target_node);

    return NUMA_KEY_MIGRATE_OK;
}
```

### Hash 类型

```c
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node) {
    if (val_obj->encoding == OBJ_ENCODING_HT) {
        // 哈希表编码：迁移整个 dict
        dict *old_dict = val_obj->ptr;
        dict *new_dict = dictCreate(&hashDictType);

        // 在新节点重新分配
        dictIterator *iter = dictGetIterator(old_dict);
        dictEntry *entry;
        while ((entry = dictNext(iter)) != NULL) {
            sds key = sdsdup(dictGetKey(entry));
            sds val = sdsdup(dictGetVal(entry));
            // 新分配的 key/val 会在目标节点
            dictAdd(new_dict, key, val);
        }
        dictReleaseIterator(iter);

        // 原子切换
        val_obj->ptr = new_dict;
        dictRelease(old_dict);
    } else if (val_obj->encoding == OBJ_ENCODING_ZIPLIST) {
        // 压缩列表：整体迁移
        unsigned char *old_zl = val_obj->ptr;
        unsigned char *new_zl = zmalloc_onnode(ziplistBlobLen(old_zl), target_node);
        memcpy(new_zl, old_zl, ziplistBlobLen(old_zl));
        val_obj->ptr = new_zl;
        zfree(old_zl);
    }
    return NUMA_KEY_MIGRATE_OK;
}
```

### List 类型

```c
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node) {
    if (val_obj->encoding == OBJ_ENCODING_QUICKLIST) {
        // QuickList：遍历节点，逐个迁移
        quicklist *old_ql = val_obj->ptr;
        // ... 迁移逻辑
    }
    return NUMA_KEY_MIGRATE_OK;
}
```

### Set 类型

```c
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node) {
    if (val_obj->encoding == OBJ_ENCODING_HT) {
        // 哈希表编码的 Set
    } else if (val_obj->encoding == OBJ_ENCODING_INTSET) {
        // 整数集合：整体迁移
    }
    return NUMA_KEY_MIGRATE_OK;
}
```

### ZSet 类型

```c
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node) {
    if (val_obj->encoding == OBJ_ENCODING_SKIPLIST) {
        // 跳表编码的 ZSet
    } else if (val_obj->encoding == OBJ_ENCODING_ZIPLIST) {
        // 压缩列表编码的 ZSet
    }
    return NUMA_KEY_MIGRATE_OK;
}
```

## 迁移流程

### numa_migrate_single_key() 完整流程

```
numa_migrate_single_key(db, key, target_node)
    │
    ├── 1. 获取 Key 的 value 对象
    │     robj *val = lookupKeyRead(db, key);
    │     └── 不存在 ──► 返回 ENOENT
    │
    ├── 2. 获取当前节点
    │     int current_node = numa_get_key_current_node(val);
    │     └── 已在目标节点 ──► 返回 OK（无需迁移）
    │
    ├── 3. 计算内存占用
    │     size_t size = getObjectSize(val);
    │
    ├── 4. 根据编码类型选择适配器
    │     switch (val->type) {
    │         case OBJ_STRING:  migrate_string_type()
    │         case OBJ_HASH:    migrate_hash_type()
    │         case OBJ_LIST:    migrate_list_type()
    │         case OBJ_SET:     migrate_set_type()
    │         case OBJ_ZSET:    migrate_zset_type()
    │     }
    │
    ├── 5. 执行迁移
    │     ├── 目标节点分配新内存
    │     ├── 复制数据
    │     ├── 原子指针切换
    │     └── 释放旧内存
    │
    ├── 6. 更新元数据
    │     numa_set_key_node(val, target_node);
    │
    ├── 7. 更新统计
    │     stats.successful_migrations++
    │     stats.total_bytes_migrated += size
    │
    └── 8. 返回 OK
```

## 热度追踪

### 记录访问

```c
void numa_record_key_access(robj *key, robj *val) {
    // 调用 Composite LRU 的 record_access
    composite_lru_record_access(strategy, key, val);

    // 同时更新元数据字典（兼容路径）
    key_numa_metadata_t *meta = get_metadata(key);
    if (meta) {
        meta->access_count++;
        meta->last_access_time = LRU_CLOCK();
    }
}
```

### 周期性衰减

```c
void numa_perform_heat_decay(void) {
    // 遍历元数据字典，对长时间未访问的 Key 衰减热度
    dictIterator *iter = dictGetIterator(key_metadata);
    dictEntry *entry;
    while ((entry = dictNext(iter)) != NULL) {
        key_numa_metadata_t *meta = dictGetVal(entry);
        // 应用衰减逻辑
        // ...
    }
    dictReleaseIterator(iter);
}
```

## 元数据管理

### 获取 Key 元数据

```c
key_numa_metadata_t* numa_get_key_metadata(robj *key) {
    return dictFetchValue(key_metadata, key);
}
```

### 获取当前节点

```c
int numa_get_key_current_node(robj *key) {
    // 优先从 PREFIX 读取
    robj *val = lookupKeyRead(db, key);
    if (val) {
        return numa_get_key_current_node_from_prefix(val);
    }

    // 回退到元数据字典
    key_numa_metadata_t *meta = numa_get_key_metadata(key);
    return meta ? meta->current_node : -1;
}
```

### Key 删除通知

防止内存泄漏：

```c
void numa_on_key_delete(robj *key) {
    // 从元数据字典中移除
    dictDelete(key_metadata, key);
}
```

## 错误码

```c
#define NUMA_KEY_MIGRATE_OK       0    // 操作成功
#define NUMA_KEY_MIGRATE_ERR     -1    // 一般错误
#define NUMA_KEY_MIGRATE_ENOENT  -2    // Key 不存在
#define NUMA_KEY_MIGRATE_EINVAL  -3    // 参数无效
#define NUMA_KEY_MIGRATE_ENOMEM  -4    // 内存不足
#define NUMA_KEY_MIGRATE_ETYPE   -5    // 不支持的数据类型
```

## 统计查询

```c
void numa_get_migration_statistics(numa_key_migrate_stats_t *stats) {
    pthread_mutex_lock(&ctx.mutex);
    *stats = ctx.stats;
    pthread_mutex_unlock(&ctx.mutex);
}

void numa_reset_migration_statistics(void) {
    pthread_mutex_lock(&ctx.mutex);
    memset(&ctx.stats, 0, sizeof(numa_key_migrate_stats_t));
    pthread_mutex_unlock(&ctx.mutex);
}
```

## 原子性保证

### 单线程保障

Redis 主线程处理所有客户端命令，迁移操作：
1. 在主线程执行
2. 迁移期间不会有其他命令访问该 Key
3. 指针切换是原子操作（赋值即生效）

### 迁移过程

```
1. 查找 Key ──► 获得 robj *val
2. 分配新内存 ──► void *new_ptr
3. 复制数据 ──► memcpy(new_ptr, val->ptr, size)
4. 指针切换 ──► val->ptr = new_ptr  （原子操作）
5. 释放旧内存 ──► zfree(old_ptr)
```

步骤 4 是关键：指针切换后立即生效，后续对该 Key 的访问都使用新内存。

## 与其他模块的关系

### 被 Composite LRU 调用

```
composite_lru_execute()
    │
    ├── 快速通道 ──► numa_migrate_single_key()
    │
    └── 兜底通道 ──► numa_migrate_single_key()
```

### 被统一命令接口调用

```
numa_command.c
    │
    ├── NUMA MIGRATE KEY ──► numa_migrate_single_key()
    ├── NUMA MIGRATE DB  ──► numa_migrate_entire_database()
    └── NUMA MIGRATE SCAN ──► composite_lru_scan_once()
```

### 与 zmalloc 的关系

迁移时使用 `zmalloc_onnode()` 在目标节点分配新内存：

```c
void *new_ptr = zmalloc_onnode(size, target_node);
```

### 与 Key 删除的关系

当 Key 被删除时，通知 NUMA 模块清理元数据：

```c
// db.c 中
void dbDelete(redisDb *db, robj *key) {
    // ...
    numa_on_key_delete(key);
}
```

## 性能特征

| 操作 | 时间复杂度 | 说明 |
|------|-----------|------|
| 单 Key 迁移 | O(data_size) | 主要耗时在 memcpy |
| 批量迁移 | O(n × avg_size) | n = Key 数量 |
| 模式迁移 | O(N) | N = 匹配 Key 数 |
| 全库迁移 | O(db_size) | 全库 Key 数量 |

### 优化建议

1. **批量迁移**：多个 Key 迁移到同一节点时，使用批量接口
2. **错峰迁移**：避免在高峰期执行大量迁移
3. **监控统计**：通过 `NUMA MIGRATE STATS` 观察迁移频率

## 使用示例

### 手动迁移单个 Key

```bash
redis-cli NUMA MIGRATE KEY user:100 1
```

### 迁移匹配模式的 Key

```bash
redis-cli NUMA MIGRATE PATTERN "session:*" 1
```

### 查询 Key 元数据

```bash
redis-cli NUMA MIGRATE INFO user:100
```

返回：
```
type: string
current_node: 0
hotness_level: 5
access_count: 1234
numa_nodes_available: 2
current_cpu_node: 0
```

## 配置参数

```c
#define DEFAULT_MIGRATE_THRESHOLD   5    // 默认迁移热度阈值
#define DEFAULT_BATCH_SIZE          50   // 默认批量迁移数量
```

这些参数可通过 Composite LRU 的 JSON 配置文件调整。
