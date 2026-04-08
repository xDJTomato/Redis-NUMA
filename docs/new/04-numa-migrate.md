# NUMA 内存迁移模块

## 模块概述

`numa_migrate.c/h` 提供底层的块级内存跨节点迁移功能。它是所有上层迁移策略（Key 级别迁移、Composite LRU 自动迁移）的基础设施。

## 核心功能

将已分配的内存块从一个 NUMA 节点完整迁移到另一个节点，同时：
- 保持数据一致性（迁移过程原子性）
- 统计迁移性能指标
- 处理迁移失败的回退

## 数据结构

### 迁移统计

```c
typedef struct {
    uint64_t total_migrations;      // 已完成的迁移次数
    uint64_t bytes_migrated;        // 已迁移的总字节数
    uint64_t failed_migrations;     // 失败的迁移次数
    uint64_t migration_time_us;     // 迁移消耗的总时间（微秒）
} numa_migrate_stats_t;
```

## 核心函数

### numa_migrate_memory()

将内存块从当前节点迁移到目标节点：

```c
void *numa_migrate_memory(void *ptr, size_t size, int target_node);
```

**参数**：
- `ptr`: 内存块的用户指针（非 raw 指针，即跳过 PREFIX 后的指针）
- `size`: 内存块大小
- `target_node`: 目标 NUMA 节点 ID

**返回**：
- 成功：返回新地址上的指针（旧指针失效）
- 失败：返回 NULL

**实现原理**：

```
numa_migrate_memory(ptr, size, target_node)
    │
    ├── 1. 记录开始时间
    │
    ├── 2. 在目标节点分配新内存
    │     new_ptr = numa_alloc_onnode(size, target_node)
    │     │
    │     └── 分配失败 ──► 返回 NULL
    │
    ├── 3. 复制数据
    │     memcpy(new_ptr, old_ptr, size)
    │
    ├── 4. 释放旧内存
    │     numa_free(old_ptr, size)
    │
    ├── 5. 更新统计
    │     stats.total_migrations++
    │     stats.bytes_migrated += size
    │     stats.migration_time_us += elapsed
    │
    └── 6. 返回新指针
```

**注意**：迁移成功后旧指针立即失效，调用者必须更新所有引用。

## 迁移的原子性保证

### 问题

迁移过程中如果发生并发访问，可能导致数据不一致。

### 解决方案

Redis 是单线程处理客户端命令，因此：
- 迁移操作在主线程执行
- 迁移期间不会有其他命令访问该 Key
- 数据一致性天然保证

对于后台自动迁移（Composite LRU 触发）：
- 迁移决策和执行都在 serverCron 中串行执行
- 不存在并发迁移同一 Key 的可能

## 错误处理

```c
void *numa_migrate_memory(void *ptr, size_t size, int target_node) {
    // 1. 参数校验
    if (!ptr || size == 0) return NULL;
    if (target_node < 0 || target_node >= numa_num_configured_nodes()) {
        stats.failed_migrations++;
        return NULL;
    }

    // 2. 目标节点分配
    void *new_ptr = numa_alloc_onnode(size, target_node);
    if (!new_ptr) {
        stats.failed_migrations++;
        return NULL;  // 旧内存保持不变
    }

    // 3. 数据复制
    memcpy(new_ptr, ptr, size);

    // 4. 释放旧内存
    numa_free(ptr, size);

    // 5. 更新统计
    stats.total_migrations++;
    stats.bytes_migrated += size;

    return new_ptr;
}
```

## 统计查询

```c
// 获取统计信息
void numa_migrate_get_stats(numa_migrate_stats_t *stats) {
    *stats = numa_migrate_stats;
}

// 重置统计
void numa_migrate_reset_stats(void) {
    memset(&numa_migrate_stats, 0, sizeof(numa_migrate_stats_t));
}
```

## 与上层模块的关系

### 被 Key 迁移模块调用

```
numa_migrate_single_key(db, key, target_node)
    │
    ├── 1. 定位 Key 的 value 对象
    ├── 2. 根据数据类型调用对应适配器
    │     ├── migrate_string_type()
    │     ├── migrate_hash_type()
    │     └── ...
    │
    └── 3. 适配器内部调用 numa_migrate_memory()
```

### 被 Composite LRU 调用

```
composite_lru_execute(strategy)
    │
    ├── 遍历候选池
    │     │
    │     └── 满足条件 ──► numa_migrate_single_key()
    │
    └── 渐进扫描
          │
          └── 满足条件 ──► numa_migrate_single_key()
```

## 性能优化

### 批量迁移

当需要迁移多个 Key 时，使用批量接口减少函数调用开销：

```c
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node) {
    int migrated = 0;
    listNode *ln;

    for (ln = key_list->head; ln; ln = ln->next) {
        robj *key = ln->value;
        if (numa_migrate_single_key(db, key, target_node) == 0) {
            migrated++;
        }
    }
    return migrated;
}
```

### 内存预分配

对于已知大小的对象，可以在目标节点预分配内存，减少迁移时的分配延迟：

```c
// 预分配阶段
void *new_ptr = numa_alloc_onnode(size, target_node);

// 迁移阶段（只需 memcpy + 指针切换）
memcpy(new_ptr, old_ptr, size);
dictReplace(db->dict, key, new_ptr);
numa_free(old_ptr, size);
```

## 模块初始化

```c
int numa_migrate_init(void) {
    memset(&numa_migrate_stats, 0, sizeof(numa_migrate_stats_t));
    return NUMA_MIGRATE_OK;
}

void numa_migrate_cleanup(void) {
    // 当前无需要清理的资源
}
```

## 使用示例

### 手动迁移

```c
// 将 ptr 指向的 size 字节内存迁移到节点 1
void *new_ptr = numa_migrate_memory(ptr, size, 1);
if (new_ptr) {
    // 更新引用
    ptr = new_ptr;
} else {
    // 处理失败
}
```

### 查询统计

```c
numa_migrate_stats_t stats;
numa_migrate_get_stats(&stats);

printf("Total migrations: %lu\n", stats.total_migrations);
printf("Bytes migrated: %lu\n", stats.bytes_migrated);
printf("Failed migrations: %lu\n", stats.failed_migrations);
printf("Total time: %lu us\n", stats.migration_time_us);
```
