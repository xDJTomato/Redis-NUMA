# NUMA MIGRATE DB 小步化开发计划

## 目标

将 `NUMA MIGRATE DB` 从一次性阻塞迁移改为分批小步迁移，利用 `migrated` 字段的三态扩展保证迁移期间的一致性，降低 admin 命令对前台请求的阻塞时间。

## 背景

当前 `NUMA MIGRATE DB` 遍历整个 keyspace 并逐键迁移，全程阻塞 Redis 主线程。对于百万级 key 的数据库，阻塞时间可达秒级。AE Strategy Scheduler 已证明"小步执行"能有效降低尾延迟，但其优化范围仅覆盖后台策略路径（CompositeLRU/TinyLFU），不覆盖前台 admin 命令。

本方案将小步化思想扩展到 `MIGRATE DB`，同时解决迁移期间的读写一致性问题。

## 一、migrated 字段三态扩展

### 1.1 现状

`numa_alloc_prefix_t` 中的 `migrated` 字段（1 字节）当前仅用作布尔值：

```c
// zmalloc.c:188
char migrated;  // 0=未迁移, 1=已迁移
```

`numa_set_migrated(ptr, 1)` 在迁移完成后调用，其他模块未读取该字段。

### 1.2 扩展为三态

```c
// zmalloc.h 新增定义
#define NUMA_MIGRATE_STATE_NONE       0  // 未迁移
#define NUMA_MIGRATE_STATE_DONE       1  // 迁移完成（兼容现有用法）
#define NUMA_MIGRATE_STATE_IN_PROGRESS 2  // 迁移中
```

现有 `numa_set_migrated(ptr, 1)` 调用无需修改，语义不变。

## 二、小步化 MIGRATE DB 实现

### 2.1 新增数据结构

```c
// numa_key_migrate.h

// 迁移期间的挂起写操作
typedef struct {
    sds key_name;           // key 名称副本
    robj *value;            // 新值（SET/覆盖写）
    long long expire_time;  // 过期时间（-1=不设置）
    int db_id;              // 数据库编号
} migrate_pending_write_t;

// MIGRATE DB 执行上下文
typedef struct {
    int target_node;                // 目标 NUMA 节点
    int db_id;                      // 数据库编号
    unsigned long batch_size;       // 每批迁移 key 数量（默认 1000）
    unsigned long migrated_count;   // 已迁移 key 计数
    unsigned long pending_count;    // 挂起写操作计数
    list *pending_writes;           // 挂起写操作链表
    dictIterator *cursor;           // 游标（跨批次保持）
    int done;                       // 是否完成
} migrate_db_context_t;
```

### 2.2 执行流程

```
NUMA MIGRATE DB <node>
  → 创建 migrate_db_context_t
  → 注册为 serverCron 的小步任务（类似 AE 的 time event）
  → 每次回调执行一个批次：
      for i in 0..batch_size:
          key = dictNext(cursor)
          if key == NULL:
              context.done = 1
              break
          // ① 标记迁移中
          numa_set_migrated(val_obj, NUMA_MIGRATE_STATE_IN_PROGRESS)
          // ② 复制数据到目标节点
          migrate_single_key(key, val_obj, target_node)
          // ③ 标记完成
          numa_set_migrated(val_obj, NUMA_MIGRATE_STATE_DONE)
          migrated_count++
      // ④ yield：返回控制权给事件循环
      // ⑤ 下次回调继续处理挂起的写操作
```

### 2.3 serverCron 集成

在 `server.c` 的 `serverCron` 中新增检查：

```c
// server.c - serverCron()
if (server.migrate_db_context && !server.migrate_db_context->done) {
    migrate_db_step(server.migrate_db_context);
}
```

或者利用现有的 AE Strategy Scheduler 框架，将 MIGRATE DB 注册为一个特殊的 strategy slot（slot 15，保留给管理命令）。

## 三、一致性保证

### 3.1 读路径

读操作（`lookupKey`）检查 `migrated` 状态：

```c
// db.c - lookupKey()
robj *val = dictGetVal(de);
int state = numa_get_migrated(val);
if (state == NUMA_MIGRATE_STATE_IN_PROGRESS) {
    // 迁移中，但旧指针仍然有效（数据复制完成才更新指针）
    // 直接返回旧位置的数据，无需特殊处理
}
```

**安全性分析：** 迁移过程是"复制 → 更新指针"，在指针更新前旧数据完整可用。`migrated=2` 期间客户端读到的是旧位置的数据，迁移完成后自动读到新位置。无需额外处理。

### 3.2 写路径

写操作（`setGenericCommand` 等）需要检查迁移状态：

```c
// t_string.c - setGenericCommand() / db.c - dbAdd() / db.c - dbGenericDelete()
robj *existing_val = lookupKeyWriteWithFlags(db, key, LOOKUP_NOEFFECTS);
if (existing_val) {
    int state = numa_get_migrated(existing_val);
    if (state == NUMA_MIGRATE_STATE_IN_PROGRESS) {
        // 挂起写操作，迁移完成后回放
        migrate_pending_write_t *pw = zmalloc(sizeof(*pw));
        pw->key_name = sdsdup(key->ptr);
        pw->value = incrRefCount(new_val);  // 保存新值
        pw->expire_time = expire;  // 或从旧值继承
        pw->db_id = db->id;
        listAddNodeTail(server.migrate_db_context->pending_writes, pw);
        server.migrate_db_context->pending_count++;
        addReplyShared(c, "+OK\r\n");  // 先返回成功
        return;
    }
}
```

### 3.3 挂起写回放

每批迁移完成后、yield 前，回放挂起的写操作：

```c
void migrate_db_replay_pending(migrate_db_context_t *ctx) {
    listIter li;
    listNode *ln;
    listRewind(ctx->pending_writes, &li);
    while ((ln = listNext(&li))) {
        migrate_pending_write_t *pw = listNodeValue(ln);
        robj *key = createStringObject(pw->key_name, sdslen(pw->key_name));
        robj *val = pw->value;

        // 检查 key 是否仍在迁移中
        robj *existing = lookupKeyWrite(server.db + pw->db_id, key);
        if (existing && numa_get_migrated(existing) == NUMA_MIGRATE_STATE_IN_PROGRESS) {
            // 仍在迁移中，保留在 pending 列表
            continue;
        }

        // 执行写操作
        setGenericCommand(NULL, CMD_SET_GENERAL, key, val, ...);
        sdsfree(pw->key_name);
        decrRefCount(val);
        zfree(pw);
        listDelNode(ctx->pending_writes, ln);
        ctx->pending_count--;
    }
}
```

### 3.4 过期键处理

过期键在迁移前检查，已过期的 key 直接跳过不迁移：

```c
if (keyIsExpired(db, key)) {
    dbAsyncDelete(db, key);  // 清理过期 key
    continue;
}
```

## 四、与 AE Strategy Scheduler 的关系

| 维度 | AE Strategy Scheduler | 小步化 MIGRATE DB |
|------|----------------------|-------------------|
| 优化对象 | 后台策略（CompositeLRU/TinyLFU） | 前台 admin 命令 |
| 执行路径 | `numa_strategy_run_all()` → `execute_step()` | `migrate_db_step()` |
| 调度方式 | AE time event（可配时间预算） | serverCron 回调（固定步长） |
| 一致性 | 无需特殊处理（操作候选池快照） | 需要 migrated 三态 + pending buffer |
| 触发方式 | 自动（serverCron/ae 定时） | 用户手动 `NUMA MIGRATE DB` |

两者独立实现，互不干扰。未来可考虑将 MIGRATE DB 也注册为 AE time event，统一调度框架。

## 五、实现步骤

### Phase 1：基础框架（预计 2-3 小时）

1. **`zmalloc.h`** — 新增 `NUMA_MIGRATE_STATE_*` 定义
2. **`numa_key_migrate.h`** — 新增 `migrate_pending_write_t` 和 `migrate_db_context_t` 结构体
3. **`numa_key_migrate.c`** — 实现 `migrate_db_context_create()` / `migrate_db_context_destroy()`
4. **`numa_key_migrate.c`** — 实现 `migrate_db_step()` 小步执行函数
5. **`server.h`** — 在 `redisServer` 中添加 `migrate_db_context` 字段
6. **`server.c`** — 在 `serverCron` 中添加小步回调检查

### Phase 2：一致性保证（预计 3-4 小时）

7. **`db.c`** — 修改 `lookupKeyReadWithFlags()`，迁移中状态无需特殊处理（确认安全性）
8. **`t_string.c`** — 修改 `setGenericCommand()`，检查迁移中状态并挂起写操作
9. **`db.c`** — 修改 `dbAdd()` / `dbGenericDelete()`，同上
10. **`numa_key_migrate.c`** — 实现 `migrate_db_replay_pending()` 挂起写回放
11. **`numa_key_migrate.c`** — 在 `migrate_db_step()` 中集成回放逻辑

### Phase 3：命令接口（预计 1-2 小时）

12. **`numa_command.c`** — 修改 `NUMA MIGRATE DB` 命令处理，改为启动小步任务而非同步执行
13. **`numa_command.c`** — 新增 `NUMA MIGRATE DB STATUS` 子命令，查询迁移进度
14. **`numa_command.c`** — 新增 `NUMA MIGRATE DB CANCEL` 子命令，取消进行中的迁移

### Phase 4：测试与验证（预计 2-3 小时）

15. 编写单元测试：验证三态切换的正确性
16. 编写集成测试：迁移期间写入不丢失
17. 性能测试：对比同步 vs 小步化的阻塞时间
18. 压力测试：迁移期间持续写入，验证一致性

## 六、风险与缓解

| 风险 | 影响 | 缓解措施 |
|------|------|----------|
| pending buffer 内存增长 | 高写入负载下挂起写操作堆积 | 设置上限（如 10000 条），超过则阻塞等待回放完成 |
| 游标失效 | 迁移期间 key 被删除，dictIterator 可能失效 | 使用 `dictGetSafeIterator`，删除时标记跳过 |
| 回放顺序 | 挂起写操作的回放顺序可能影响最终状态 | 按 key 分组回放，同一 key 的写操作保序 |
| 迁移中断 | 服务重启导致部分迁移丢失 | 迁移是幂等操作，重启后重新执行即可 |

## 七、验证标准

1. **功能正确性**：MIGRATE DB 完成后，所有 key 的 `migrated` 状态为 1，节点分配正确
2. **一致性**：迁移期间执行 SET/DEL/EXPIRE，数据最终一致
3. **阻塞时间**：单批迁移耗时 < 10ms（相比同步的秒级阻塞）
4. **吞吐量影响**：迁移期间前台吞吐量下降 < 5%
5. **内存开销**：pending buffer 内存占用 < 10MB（10000 条 × 1KB 平均）
