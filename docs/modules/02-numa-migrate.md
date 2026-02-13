# 02-numa-migrate.md - NUMA迁移模块

## 模块概述

**文件**: `src/numa_migrate.h`, `src/numa_migrate.c`

**功能**: 提供内存对象在NUMA节点之间的迁移能力，支持将数据从Node A迁移到Node B。

**设计目标**:
- 支持命令触发和后台自动迁移（双模式）
- 数据完整性保证（复制-验证-切换）
- 迁移统计与性能监控
- 为负载均衡和热数据分层提供基础

---

## 业务调用链

### 基础迁移流程

```
┌─────────────────────────────────────────────────────────────┐
│  调用入口                                                    │
│  Application: numa_migrate_memory(ptr, size, target_node)   │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  Step 1: 参数验证                                            │
│  - 检查模块是否初始化                                        │
│  - 验证目标节点有效性                                        │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  Step 2: 目标节点分配内存                                    │
│  numa_zmalloc_onnode(size, target_node)                     │
│  └──► zmalloc.c: numa_alloc_on_specific_node()              │
│       └──► libnuma: numa_alloc_onnode()                     │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  Step 3: 数据复制                                            │
│  memcpy(new_ptr, old_ptr, size)                             │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  Step 4: 释放原内存                                          │
│  zfree(old_ptr)                                             │
│  └──► numa_free_with_size()                                 │
│       └──► numa_pool_free() / numa_free()                   │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  Step 5: 更新统计                                            │
│  - total_migrations++                                       │
│  - bytes_migrated += size                                   │
│  - migration_time_us += elapsed                             │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  返回新指针                                                  │
└─────────────────────────────────────────────────────────────┘
```

### 完整架构（含未来扩展）

```
┌─────────────────────────────────────────────────────────────┐
│                    Redis Server                              │
├─────────────────────────────────────────────────────────────┤
│  Command Layer          │    Background Thread               │
│  - MIGRATE KEY          │    - 定期检查负载                  │
│  - MIGRATE DB           │    - 热度分析                      │
│  - SHOW NUMA STATS      │    - 自动迁移决策                  │
└───────────┬─────────────┴──────────┬────────────────────────┘
            │                        │
            ▼                        ▼
┌─────────────────────┐    ┌─────────────────────┐
│   numa_migrate.c    │    │  (Future: numa_     │
│  (迁移执行引擎)      │    │   migrate_bg.c)     │
│  - 数据复制          │    │  - 负载监控          │
│  - 指针更新          │    │  - 热度计算          │
│  - 原子切换          │    │  - 迁移调度          │
└──────────┬──────────┘    └──────────┬──────────┘
           │                          │
           └──────────┬───────────────┘
                      ▼
        ┌─────────────────────────────┐
        │        zmalloc.c            │
        │  - numa_zmalloc_onnode()    │
        │  - numa_zfree()             │
        └─────────────┬───────────────┘
                      │
                      ▼
        ┌─────────────────────────────┐
        │       numa_pool.c           │
        │  - 节点粒度内存池            │
        └─────────────────────────────┘
```

---

## 核心数据结构

### 1. 迁移统计结构

```c
typedef struct {
    uint64_t total_migrations;      /* 总迁移次数 */
    uint64_t bytes_migrated;        /* 总迁移字节数 */
    uint64_t failed_migrations;     /* 失败次数 */
    uint64_t migration_time_us;     /* 总耗时（微秒） */
} numa_migrate_stats_t;
```

### 2. 模块内部状态

```c
static numa_migrate_stats_t migrate_stats = {0};
static int migrate_initialized = 0;
```

---

## 核心算法

### 1. 基础迁移算法

```c
void *numa_migrate_memory(void *ptr, size_t size, int target_node)
{
    /* 1. 参数验证 */
    if (!migrate_initialized || ptr == NULL || size == 0)
        return NULL;
    if (target_node < 0 || target_node > numa_max_node())
        return NULL;
    
    /* 2. 记录开始时间 */
    uint64_t start_time = get_time_us();
    
    /* 3. 在目标节点分配新内存 */
    void *new_ptr = numa_zmalloc_onnode(size, target_node);
    if (!new_ptr) {
        migrate_stats.failed_migrations++;
        return NULL;
    }
    
    /* 4. 复制数据 */
    memcpy(new_ptr, ptr, size);
    
    /* 5. 释放原内存 */
    zfree(ptr);
    
    /* 6. 更新统计 */
    migrate_stats.total_migrations++;
    migrate_stats.bytes_migrated += size;
    migrate_stats.migration_time_us += (get_time_us() - start_time);
    
    return new_ptr;
}
```

### 2. 数据完整性保证

迁移过程中保证数据完整性的关键步骤：

1. **先分配后释放**: 新内存分配成功后才释放旧内存
2. **完整复制**: 使用memcpy复制全部数据
3. **返回值检查**: 调用者需要更新指针引用

```c
/* 正确的迁移使用模式 */
void *new_ptr = numa_migrate_memory(old_ptr, size, target_node);
if (new_ptr) {
    /* 成功：更新所有引用 */
    obj->data = new_ptr;
} else {
    /* 失败：旧指针仍然有效 */
    /* 处理错误 */
}
```

---

## 接口函数

### 初始化与清理

```c
int numa_migrate_init(void);        /* 初始化迁移模块 */
void numa_migrate_cleanup(void);    /* 清理迁移模块 */
```

### 核心迁移函数

```c
void *numa_migrate_memory(void *ptr, size_t size, int target_node);
```

**参数**:
- `ptr`: 待迁移的内存指针（用户指针）
- `size`: 内存块大小
- `target_node`: 目标NUMA节点ID

**返回值**:
- 成功：新内存指针（位于target_node）
- 失败：NULL（原指针仍然有效）

### 统计信息

```c
void numa_migrate_get_stats(numa_migrate_stats_t *stats);
void numa_migrate_reset_stats(void);
```

---

## 性能特征

| 操作 | 时间复杂度 | 说明 |
|------|-----------|------|
| 内存分配 | O(1) | numa_zmalloc_onnode |
| 数据复制 | O(n) | memcpy，n为数据大小 |
| 内存释放 | O(1) | zfree |
| 总计 | O(n) | 主要开销在数据复制 |

**典型性能**（1KB数据）：
- 分配：~1-2 μs
- 复制：~0.5 μs
- 释放：~1 μs
- 总计：~2.5-3.5 μs

---

## 使用示例

### 基础迁移示例

```c
#include "numa_migrate.h"

/* 初始化 */
numa_migrate_init();

/* 分配内存 */
void *data = zmalloc(1024);

/* 填充数据 */
strcpy(data, "Hello, NUMA!");

/* 迁移到节点1 */
void *migrated = numa_migrate_memory(data, 1024, 1);
if (migrated) {
    printf("Migration successful!\n");
    printf("Data: %s\n", (char*)migrated);
    data = migrated;  /* 更新指针 */
}

/* 清理 */
zfree(data);
numa_migrate_cleanup();
```

### 批量迁移示例

```c
/* 迁移多个对象 */
for (int i = 0; i < num_objects; i++) {
    void *new_ptr = numa_migrate_memory(
        objects[i].data, 
        objects[i].size, 
        target_node
    );
    if (new_ptr) {
        objects[i].data = new_ptr;
        objects[i].node = target_node;
    }
}
```

---

## 设计决策

### 为什么不在模块内部更新Redis对象指针？

- **职责分离**: 迁移模块只负责内存迁移，不感知上层数据结构
- **灵活性**: 调用者决定如何更新引用
- **错误处理**: 失败时调用者可以选择回滚或重试

### 为什么使用numa_zmalloc_onnode而不是numa_pool_alloc？

- **精确控制**: 必须确保内存在指定节点分配
- **绕过池**: 迁移通常针对大对象或特定需求，不经过内存池
- **简化实现**: 直接使用libNUMA接口

### 未来扩展：热度感知迁移

```c
/* 计划中的扩展接口 */
typedef struct {
    uint8_t hotness;        /* 热度等级 0-7 */
    uint8_t node_id;        /* 当前节点 */
    uint8_t access_count;   /* 访问计数 */
    uint16_t last_access;   /* 最后访问时间 */
} numa_object_meta_t;

/* 自动迁移决策 */
int numa_migrate_auto(void *ptr, size_t size);
```

---

## 限制与注意事项

1. **单线程迁移**: 当前实现非线程安全，需要调用者加锁
2. **指针更新**: 调用者必须更新所有引用该内存的指针
3. **失败处理**: 迁移失败时原内存保持不变，需要正确处理错误
4. **大对象**: 大对象迁移耗时较长，建议在低峰期进行
