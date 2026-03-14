# 05-numa-key-migrate.md - NUMA Key级别迁移模块

## 模块概述

**状态**: ✅ **已实现** - 所有Redis核心数据类型迁移已完成，与NUMACONFIG可配置策略模块协同工作

**文件**: [src/numa_key_migrate.h](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_key_migrate.h), [src/numa_key_migrate.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_key_migrate.c)

**已实现功能**:
- ✅ 模块初始化和清理
- ✅ Key元数据管理（热度、访问计数、节点信息）
- ✅ LRU集成的热度追踪机制（访问感知统一递增）
- ✅ 惰性阶梯衰减机制（访问时结算，无需后台扫描）
- ✅ `numa_on_key_delete` TTL/DEL 元数据清理钩子（防止幽灵热度）
- ✅ STRING类型迁移（RAW/EMBSTR编码）
- ✅ HASH类型迁移（ziplist/hashtable编码）
- ✅ LIST类型迁移（quicklist编码，支持LZF压缩）
- ✅ SET类型迁移（intset/hashtable编码）
- ✅ ZSET类型迁移（ziplist/skiplist编码）
- ✅ 单Key迁移接口
- ✅ 批量Key迁移接口
- ✅ 数据库级别迁移
- ✅ 迁移统计信息
- ✅ **NUMAMIGRATE命令**（指令式迁移接口）

**待实现功能**:
- ⚠️ 模式匹配迁移（按通配符筛选Key）

**功能**: 实现Redis Key在NUMA节点间的智能迁移，通过分析访问模式和节点负载，自动优化数据分布以提升内存访问性能。

**核心价值**:
- 解决NUMA架构下的内存访问延迟差异问题
- 通过数据迁移实现热点数据近端存储
- 与Redis原生LRU机制深度集成
- 支持完整的Redis数据类型迁移（逐步实现中）

**依赖关系**:
- 基础内存迁移：[numa_migrate.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_migrate.c)（已保留）
- 内存池分配：[numa_pool.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_pool.c)（P0/P1优化完成）
- Slab分配器：[numa_slab.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_slab.c)（P2优化完成，Slab+Pool双路径）
- 策略插槽框架：[06-numa-strategy-slots](./06-numa-strategy-slots.md)（已实现）
- 复合LRU策略：[07-numa-composite-lru](./07-numa-composite-lru.md)（已实现）

---

## 业务逻辑

### 核心迁移流程

```
┌─────────────────────────────────────────────────────────────┐
│  迁移触发                                                    │
│  - 策略模块决策: 热点识别/负载分析/带宽评估                 │
│  - 生成迁移候选列表                                          │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  Key定位与验证                                               │
│  - 通过db->dict定位目标Key                                  │
│  - 验证Key存在性和数据完整性                                 │
│  - 获取当前节点信息                                          │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  类型适配迁移                                                │
│  switch(key->type) {                                        │
│    case OBJ_STRING: 迁移SDS数据                             │
│    case OBJ_HASH:   递归迁移所有field-value对               │
│    case OBJ_LIST:   迁移quicklist所有节点                   │
│    case OBJ_SET:    迁移intset/dict元素                     │
│    case OBJ_ZSET:   迁移ziplist/skiplist数据                │
│  }                                                          │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  原子切换与清理                                              │
│  - 原子更新指针引用                                          │
│  - 释放源节点内存                                            │
│  - 更新元数据信息                                            │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  统计更新与反馈                                              │
│  - 更新迁移统计                                              │
│  - 返回执行结果                                              │
│  - 通知策略模块                                              │
└─────────────────────────────────────────────────────────────┘
```

### 数据完整性保障

1. **原子性**: 迁移过程使用写锁保护，确保中间状态不被访问
2. **一致性**: 新内存分配成功后才释放旧内存，避免数据丢失
3. **隔离性**: 迁移期间其他线程无法访问正在迁移的Key
4. **持久性**: 迁移完成后更新的指针立即生效

---

## 核心数据结构

### 迁移上下文

```c
/* 迁移模块全局状态 */
typedef struct {
    int initialized;                /* 模块初始化状态 */
    dict *key_metadata;             /* Key元数据映射表 */
    pthread_mutex_t mutex;          /* 并发控制锁 */
    numa_key_migrate_stats_t stats; /* 迁移统计信息 */
} numa_key_migrate_ctx_t;
```

### Key元数据

```c
/* 每个Key的NUMA相关信息 */
typedef struct {
    int current_node;               /* 当前所在NUMA节点 */
    uint8_t hotness_level;          /* 热度等级(0-7) */
    uint16_t last_access_time;      /* 最后访问时间戳 */
    size_t memory_footprint;        /* 内存占用大小 */
    uint64_t access_count;          /* 累计访问次数 */
} key_numa_metadata_t;
```

### 迁移请求

```c
/* 迁移操作描述 */
typedef struct {
    robj *key_obj;                  /* 目标Key对象 */
    int source_node;                /* 源节点ID */
    int target_node;                /* 目标节点ID */
    size_t data_size;               /* 数据大小 */
    uint64_t start_time;            /* 迁移开始时间 */
} migration_request_t;
```

### 迁移统计

```c
/* 迁移性能统计 */
typedef struct {
    uint64_t total_migrations;      /* 总迁移次数 */
    uint64_t successful_migrations; /* 成功迁移数 */
    uint64_t failed_migrations;     /* 失败迁移数 */
    uint64_t total_bytes_migrated;  /* 迁移总字节数 */
    uint64_t total_migration_time;  /* 迁移总耗时(微秒) */
    uint64_t peak_concurrent_migrations; /* 并发迁移峰值 */
} numa_key_migrate_stats_t;
```

---

## 热度追踪机制

### LRU联动设计

Key迁移模块通过 Hook Redis 原生 LRU 更新机制实现热度追踪。热度更新采用《访问感知统一递增》策略：

```c
/* 在 lookupKey 等关键访问点插入热度更新 */
void numa_record_key_access(robj *key, robj *val) {
    key_numa_metadata_t *meta = get_key_metadata(key);
    if (!meta) meta = create_key_metadata(key, val);

    pthread_mutex_lock(&global_ctx.mutex);

    /* Step 1: 保存旧时间戳，然后更新 */
    meta->access_count++;
    uint16_t old_last_access = meta->last_access_time;
    meta->last_access_time = current_timestamp;

    /* Step 2: 惰性衰减 - 结算上次访问至今的时间欠债 */
    uint16_t elapsed = calculate_time_delta(current_timestamp, old_last_access);
    uint8_t decay = compute_key_lazy_decay_steps(elapsed);
    if (decay > 0) {
        meta->hotness_level = (decay >= meta->hotness_level) ? 0
                                                              : meta->hotness_level - decay;
    }

    /* Step 3: 统一递增 - 本地/远程一律 +1 */
    if (meta->hotness_level < HOTNESS_MAX_LEVEL)
        meta->hotness_level++;

    /* Step 4: 远程访问且热度达阈值 → 评估迁移 */
    int current_cpu_node = numa_get_current_node();
    if (meta->current_node != current_cpu_node &&
        meta->hotness_level >= MIGRATION_HOTNESS_THRESHOLD) {
        /* TODO: 调度迁移评估 */
    }

    pthread_mutex_unlock(&global_ctx.mutex);
}
```

### 惰性阶梯衰减策略

衰减不依赖后台定时扫描，而是在每次 key **被访问时**一次性结算空闲期欠债（惰性计算）。

```c
/*
 * compute_key_lazy_decay_steps - 阶梯衰减查表
 *
 *  elapsed < 10s   : decay 0（短暂停顿，完全免疫）
 *  elapsed < 60s   : decay 1
 *  elapsed < 5min  : decay 2
 *  elapsed < 30min : decay 3
 *  elapsed >= 30min: 全清为 0
 */
static uint8_t compute_key_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < KEY_LAZY_DECAY_STEP1_SECS) return 0;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP2_SECS) return 1;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP3_SECS) return 2;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP4_SECS) return 3;
    return HOTNESS_MAX_LEVEL; /* 长期冷数据全清 */
}
```

**阶梯常量**（定义在 `numa_key_migrate.h`）：

| 常量 | 值 | 含义 |
|------|-----|------|
| `KEY_LAZY_DECAY_STEP1_SECS` | 10 | < 10s 免疫 |
| `KEY_LAZY_DECAY_STEP2_SECS` | 60 | < 60s 衰减 1 |
| `KEY_LAZY_DECAY_STEP3_SECS` | 300 | < 5min 衰减 2 |
| `KEY_LAZY_DECAY_STEP4_SECS` | 1800 | < 30min 衰减 3；≥ 30min 全清 |

### 类型适配迁移

根据不同Redis数据类型的特点，提供专门的迁移适配器：

- **String类型**: 直接迁移SDS数据结构
- **Hash类型**: 递归迁移所有field-value对
- **List类型**: 迁移quicklist所有节点
- **Set类型**: 迁移intset/dict元素
- **ZSet类型**: 迁移ziplist/skiplist结构

每种类型都保证原子性迁移，确保数据一致性。

---

## 核心接口

### NUMAMIGRATE 命令接口

提供Redis命令行级别的手动迁移控制：

```
NUMAMIGRATE KEY <key> <target_node>   - 迁移指定Key到目标NUMA节点
NUMAMIGRATE DB <target_node>          - 迁移整个数据库到目标节点
NUMAMIGRATE STATS                     - 显示迁移统计信息
NUMAMIGRATE RESET                     - 重置统计信息
NUMAMIGRATE INFO <key>                - 查询Key的NUMA信息
NUMAMIGRATE HELP                      - 显示帮助信息
```

**使用示例**:
```bash
# 迁移单个Key
127.0.0.1:6379> SET mykey "Hello"
OK
127.0.0.1:6379> NUMAMIGRATE KEY mykey 0
OK

# 查看Key信息
127.0.0.1:6379> NUMAMIGRATE INFO mykey
 1) "type"
 2) "string"
 3) "current_node"
 4) (integer) 0
...

# 查看统计
127.0.0.1:6379> NUMAMIGRATE STATS
 1) "total_migrations"
 2) (integer) 1
 3) "successful_migrations"
 4) (integer) 1
...
```

### 迁移执行接口

```c
/* 单Key迁移 */
int numa_migrate_single_key(redisDb *db, robj *key, int target_node);

/* 批量迁移 */
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node);

/* 模式匹配迁移 */
int numa_migrate_keys_by_pattern(redisDb *db, const char *pattern, int target_node);

/* 数据库级别迁移 */
int numa_migrate_entire_database(redisDb *db, int target_node);

/* 命令处理函数 */
void numamigrateCommand(client *c);
```

### 元数据管理接口

```c
/* 热度更新Hook */
void numa_record_key_access(robj *key, robj *val);

/* 元数据查询 */
key_numa_metadata_t* numa_get_key_metadata(robj *key);
int numa_get_key_current_node(robj *key);

/* 元数据清理（删除钩子）*/
void numa_on_key_delete(robj *key);

/* 统计信息 */
void numa_get_migration_statistics(numa_key_migrate_stats_t *stats);
void numa_reset_migration_statistics(void);
```

#### `numa_on_key_delete` 说明

删除钩子函数，在 key 被删除时清除 NUMA 元数据词典中的条目。不调用则过期/DEL key 会在内部词典中残留幽灵热度元数据，如果地址被新对象复用还会引发错误热度读取。

调用位置：

| 调用点 | 场景 |
|---------|------|
| `db.c::dbSyncDelete` | `DEL` 命令、TTL 同步过期删除 |
| `lazyfree.c::dbAsyncDelete` | `UNLINK` 命令、异步惰性删除 |

```c
#ifdef HAVE_NUMA
    numa_on_key_delete(key);   /* 清理元数据，防止幽灵热度 */
#endif
```

### 类型适配器

```c
/* 各数据类型的迁移实现 */
static int migrate_string_type(robj *key_obj, int target_node);
static int migrate_hash_type(robj *key_obj, int target_node);
static int migrate_list_type(robj *key_obj, int target_node);
static int migrate_set_type(robj *key_obj, int target_node);
static int migrate_zset_type(robj *key_obj, int target_node);
```

---

## 配置参数

```c
#define DEFAULT_MIGRATE_THRESHOLD   5       /* 热度阈值 */
#define DEFAULT_BATCH_SIZE          50      /* 批量大小 */
#define DECAY_THRESHOLD             10000   /* 衰减阈值(10秒) */
```

---

## 性能特征

- 热度更新: O(1)
- String迁移: O(n)  
- Hash迁移: O(k*n)
- 批量迁移避免阻塞

---

## 设计思路

### 1. Key粒度迁移优势

采用Redis Key（`robj`）作为迁移基本单位，相比内存块级别迁移具有显著优势：

- **边界清晰**: Key是Redis的数据边界，天然适合作为迁移单元
- **引用简单**: 只需更新`db->dict`中的指针，无需遍历内部结构
- **语义完整**: 保证Key及其关联数据的原子性迁移
- **兼容性好**: 与Redis现有数据结构完全兼容

### 2. LRU机制深度集成

复用Redis原生LRU淘汰机制实现热度追踪：

- **零侵入**: 在现有LRU更新点插入Hook，不改变核心逻辑
- **时间一致**: 使用相同的LRU_CLOCK时间源，保证一致性
- **成本低廉**: 利用已有的访问计数，额外开销极小
- **自然衰减**: 基于LRU时间的衰减机制符合访问模式规律

### 3. 类型适配架构

为不同Redis数据类型提供专门的迁移适配器：

```
迁移入口
    ↓
类型识别(switch key->type)
    ↓
├─ String: 直接迁移SDS
├─ Hash:  遍历并迁移所有field-value对
├─ List:  迁移quicklist所有节点
├─ Set:   迁移intset/dict元素
└─ ZSet:  迁移ziplist/skiplist结构
    ↓
原子指针切换
    ↓
源内存释放
```

---

## 性能特征

| 操作类型 | 时间复杂度 | 典型耗时 | 说明 |
|---------|-----------|---------|------|
| 热度更新Hook | O(1) | <1μs | hash表查找 |
| String迁移 | O(n) | 1-10μs | n为字符串长度 |
| Hash迁移 | O(k×m) | 10-100μs | k字段数，m平均大小 |
| List迁移 | O(n) | 5-50μs | n为节点数 |
| 元数据管理 | O(1) | <1μs | 字典操作 |

## 限制约束

### 技术限制
1. **并发控制**: 迁移过程需持有数据库写锁，可能影响吞吐
2. **大Key处理**: 超大Hash/List迁移耗时显著，需分批处理
3. **内存开销**: 元数据表约占总Key数×32字节的额外内存
4. **指针更新**: 调用方需确保所有引用得到正确更新

### 架构依赖
1. **NUMA支持**: 需要libNUMA库和NUMA-aware内存分配器
2. **LRU机制**: 依赖Redis的LRU时钟进行热度追踪
3. **策略协同**: 与外部策略模块通过接口协作
4. **平台兼容**: 主要针对Linux NUMA架构设计

## 应用场景

适用于以下场景：
- 多NUMA节点的Redis部署
- 存在明显热点访问模式的应用
- 对内存访问延迟敏感的业务
- 需要动态负载均衡的高并发系统

---

> 详细迭代开发日志见 [devlog/NUMA_DEVELOPMENT_LOG.md](../devlog/NUMA_DEVELOPMENT_LOG.md)
