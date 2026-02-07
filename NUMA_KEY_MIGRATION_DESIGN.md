# NUMA Key迁移模块设计文档

## 1. 概述

本模块实现了Redis key级别的NUMA（Non-Uniform Memory Access）节点迁移功能，支持：
- **指定key迁移**：将特定key及其数据迁移到指定NUMA节点
- **冷热数据分层**：自动/手动标记热/冷数据并迁移到相应节点
- **访问追踪**：记录key访问频率，支持自动分类
- **批量迁移**：支持pattern匹配和批量操作

## 2. 架构设计

### 2.1 整体架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     Redis Server                                 │
├─────────────────────────────────────────────────────────────────┤
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐ │
│  │  Redis命令  │  │  自动迁移   │  │      访问追踪           │ │
│  │  处理层     │  │  调度器     │  │                         │ │
│  └──────┬──────┘  └──────┬──────┘  └───────────┬─────────────┘ │
│         │                │                      │               │
│         └────────────────┼──────────────────────┘               │
│                          │                                      │
│  ┌───────────────────────▼──────────────────────────┐           │
│  │           NUMA Key迁移核心层                      │           │
│  │  ┌─────────────┐  ┌─────────────┐  ┌──────────┐ │           │
│  │  │ 元数据管理  │  │ 迁移执行器  │  │ 策略引擎 │ │           │
│  │  │ (dict)      │  │             │  │          │ │           │
│  │  └─────────────┘  └──────┬──────┘  └──────────┘ │           │
│  │                          │                      │           │
│  └──────────────────────────┼──────────────────────┘           │
│                             │                                  │
│  ┌──────────────────────────▼──────────────────────────┐       │
│  │              NUMA内存分配扩展层                      │       │
│  │  ┌─────────────┐  ┌─────────────┐  ┌─────────────┐ │       │
│  │  │ 指定节点分配 │  │ SDS NUMA扩展 │  │ 内存池管理  │ │       │
│  │  │             │  │             │  │             │ │       │
│  │  └─────────────┘  └─────────────┘  └─────────────┘ │       │
│  └─────────────────────────────────────────────────────┘       │
│                             │                                  │
│  ┌──────────────────────────▼──────────────────────────┐       │
│  │                   libnuma接口层                      │       │
│  │              (numa_alloc_onnode等)                  │       │
│  └─────────────────────────────────────────────────────┘       │
└─────────────────────────────────────────────────────────────────┘
```

### 2.2 核心组件

#### 2.2.1 元数据管理 (`numa_key_meta_t`)

```c
typedef struct numa_key_meta {
    int target_node;              /* 目标NUMA节点 */
    int current_node;             /* 当前实际节点 */
    uint64_t migrate_timestamp;   /* 上次迁移时间 */
    uint32_t access_count;        /* 访问计数 */
    uint32_t flags;               /* 标志位 */
} numa_key_meta_t;
```

**存储方式**：使用独立的`dict`存储key到元数据的映射，避免修改`robj`结构。

#### 2.2.2 迁移执行器

**迁移流程**：
```
1. 查找key对应的robj
2. 根据对象类型估算大小
3. 在目标节点分配新内存
4. 复制数据到新内存
5. 更新元数据
6. 释放旧内存
```

**支持的类型**：
- String（RAW编码）：完全支持
- 其他类型：基础支持（需要进一步扩展）

#### 2.2.3 策略引擎

```c
typedef struct numa_migrate_policy {
    int enabled;                  /* 启用自动迁移 */
    int hot_node;                 /* 热数据节点 */
    int cold_node;                /* 冷数据节点 */
    uint32_t hot_threshold;       /* 热数据阈值 */
    uint32_t cold_threshold;      /* 冷数据阈值 */
    uint64_t migrate_interval_ms; /* 检查间隔 */
} numa_migrate_policy_t;
```

## 3. 接口设计

### 3.1 Redis命令

| 命令 | 语法 | 说明 |
|------|------|------|
| NUMA.MIGRATE | `NUMA.MIGRATE key target_node` | 迁移指定key到目标节点 |
| NUMA.MARKHOT | `NUMA.MARKHOT key` | 标记为热数据并迁移 |
| NUMA.MARKCOLD | `NUMA.MARKCOLD key` | 标记为冷数据并迁移 |
| NUMA.KEYINFO | `NUMA.KEYINFO key` | 查看key的NUMA信息 |
| NUMA.NODESTATS | `NUMA.NODESTATS` | 查看节点统计 |

### 3.2 C API

```c
/* 基础迁移 */
int numa_key_migrate(redisDb *db, sds key, int target_node);
int numa_key_migrate_batch(redisDb *db, int source_node, int target_node, 
                           int max_keys, uint64_t *migrated_bytes);

/* 冷热管理 */
int numa_key_mark_hot(redisDb *db, sds key);
int numa_key_mark_cold(redisDb *db, sds key);

/* 访问追踪 */
void numa_key_record_access(redisDb *db, sds key);

/* 自动迁移 */
void numa_key_migrate_cron(void);
```

## 4. 内存分配扩展

### 4.1 指定节点分配

```c
/* 在指定NUMA节点分配内存 */
void *numa_zmalloc_onnode(size_t size, int node);
void *numa_zcalloc_onnode(size_t size, int node);
```

**实现要点**：
- 绕过内存池，直接使用`numa_alloc_onnode`
- 确保内存分配到指定节点
- 保持PREFIX机制一致性

### 4.2 SDS NUMA扩展

```c
/* 在指定节点创建SDS */
sds sdsnumanew(const char *init, int node);
sds sdsnumadup(const sds s, int node);
sds sdsnumacatlen(sds s, const void *t, size_t len, int node);
```

**迁移策略**：
- 在新节点分配足够空间
- 复制旧数据
- 释放旧SDS
- 返回新SDS

## 5. 冷热数据分层

### 5.1 分层策略

```
┌─────────────────────────────────────────────────────────────┐
│                    冷热数据分层模型                          │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│   热数据（Hot）                                              │
│   ├── 访问频率 >= hot_threshold                             │
│   ├── 目标节点：本地NUMA节点（低延迟）                        │
│   └── 迁移策略：立即迁移                                     │
│                                                             │
│   温数据（Warm）                                             │
│   ├── cold_threshold < 访问频率 < hot_threshold            │
│   ├── 目标节点：不指定（使用默认策略）                        │
│   └── 迁移策略：保持现状                                     │
│                                                             │
│   冷数据（Cold）                                             │
│   ├── 访问频率 <= cold_threshold                           │
│   ├── 目标节点：远程NUMA节点（大容量）                        │
│   └── 迁移策略：定期批量迁移                                 │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

### 5.2 自动分类算法

```c
int numa_key_auto_classify(redisDb *db, sds key)
{
    uint32_t access = get_access_count(key);
    
    if (access >= hot_threshold)
        return numa_key_mark_hot(db, key);
    else if (access <= cold_threshold)
        return numa_key_mark_cold(db, key);
    
    return 0;  /* 保持现状 */
}
```

## 6. 线程安全

### 6.1 锁策略

```c
static pthread_rwlock_t migrate_ctx.lock;

/* 读操作使用读锁 */
pthread_rwlock_rdlock(&migrate_ctx.lock);
meta = dictFetchValue(migrate_ctx.key_meta, key);
pthread_rwlock_unlock(&migrate_ctx.lock);

/* 写操作使用写锁 */
pthread_rwlock_wrlock(&migrate_ctx.lock);
meta->target_node = target_node;
pthread_rwlock_unlock(&migrate_ctx.lock);
```

### 6.2 并发考虑

- **迁移中标志**：防止重复迁移
- **原子更新**：元数据更新使用写锁
- **无锁读**：统计信息读取使用读锁

## 7. 性能考虑

### 7.1 迁移开销

| 操作 | 开销 | 优化策略 |
|------|------|----------|
| 内存分配 | 高（系统调用） | 批量迁移摊销 |
| 数据复制 | 中（memcpy） | 零拷贝（未来） |
| 元数据更新 | 低 | 读写锁分离 |

### 7.2 建议

- **批量迁移**：避免频繁单个key迁移
- **异步迁移**：考虑后台线程迁移
- **预测迁移**：基于访问模式预测

## 8. 使用示例

### 8.1 手动迁移

```bash
# 将mykey迁移到节点1
127.0.0.1:6379> NUMA.MIGRATE mykey 1
OK

# 查看key信息
127.0.0.1:6379> NUMA.KEYINFO mykey
1) "target_node"
2) (integer) 1
3) "current_node"
4) (integer) 1
5) "access_count"
6) (integer) 42
```

### 8.2 冷热标记

```bash
# 标记热数据
127.0.0.1:6379> NUMA.MARKHOT hotkey
OK

# 标记冷数据
127.0.0.1:6379> NUMA.MARKCOLD coldkey
OK
```

### 8.3 编程接口

```c
/* 初始化 */
numa_key_migrate_init();

/* 设置策略 */
numa_migrate_policy_t policy = {
    .enabled = 1,
    .hot_node = 0,
    .cold_node = 1,
    .hot_threshold = 1000,
    .cold_threshold = 100,
    .migrate_interval_ms = 60000
};
numa_key_set_policy(&policy);

/* 定期执行 */
numa_key_migrate_cron();
```

## 9. 未来扩展

### 9.1 计划功能

- [ ] 支持更多数据类型（List、Hash、Set等）
- [ ] 异步迁移（后台线程）
- [ ] 迁移进度查询
- [ ] 自动负载均衡
- [ ] 跨节点复制支持

### 9.2 优化方向

- 零拷贝迁移
- 预取策略
- 机器学习预测

## 10. 文件清单

| 文件 | 说明 |
|------|------|
| `numa_key_migrate.h` | 模块头文件 |
| `numa_key_migrate.c` | 核心实现 |
| `sdsnuma.h` | SDS NUMA扩展头 |
| `sdsnuma.c` | SDS NUMA扩展实现 |
| `zmalloc.c/h` | 扩展指定节点分配API |

---

**版本**: 1.0  
**创建日期**: 2025年2月  
**作者**: Redis NUMA开发团队
