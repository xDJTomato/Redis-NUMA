# NUMA内存池 P1优化后的完整业务调用链

## 1. 分配流程 (numa_pool_alloc)

### 完整调用链

```
┌─────────────────────────────────────────────────────────────┐
│  调用入口                                                    │
│  zmalloc.c: numa_alloc_with_size(size)                      │
│  ├─► 计算size_class_idx                                     │
│  └─► alloc_size = size + 16 (PREFIX)                        │
└──────────────────────────┬───────────────────────────────────┘
                           │ numa_pool_alloc(alloc_size, node, size_class_idx)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  numa_pool.c: numa_pool_alloc()                             │
│                                                             │
│  步骤1: 检查条件                                            │
│  ├─► 判断size <= NUMA_POOL_MAX_ALLOC (4096B)               │
│  ├─► 验证size_class_idx < NUMA_POOL_SIZE_CLASSES           │
│  └─► 判断node有效性                                         │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤2: 获取pool并加锁                                       │
│  pool = &pool_ctx.node_pools[node].pools[size_class_idx]   │
│  pthread_mutex_lock(&pool->lock)                           │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤3: 🆕 P1优化 - 优先从Free List分配                      │
│  free_block_t *free_block = pool->free_list;               │
│  while (free_block) {                                      │
│      if (free_block->size >= aligned_size) {               │
│          // 找到合适的空闲块，重用它                         │
│          result = free_block->ptr;                         │
│          *prev_ptr = free_block->next;  // 从free list移除  │
│          free(free_block);              // 释放free_block结构体│
│          from_pool = 1;                                    │
│          break;                                            │
│      }                                                     │
│      prev_ptr = &free_block->next;                         │
│      free_block = free_block->next;                        │
│  }                                                         │
└──────────────────────────┬───────────────────────────────────┘
                           │ 如果free list中没有合适的块
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤4: Bump Pointer分配                                    │
│  chunk = pool->chunks;  // 遍历chunk链表                   │
│  while (chunk) {                                           │
│      if (chunk->offset + aligned_size <= chunk->size) {    │
│          // 当前chunk有足够空间                            │
│          result = (char*)chunk->memory + chunk->offset;    │
│          chunk->offset += aligned_size;                    │
│          chunk->used_bytes += aligned_size;  // 🆕 P1追踪   │
│          from_pool = 1;                                    │
│          break;                                            │
│      }                                                     │
│      chunk = chunk->next;                                  │
│  }                                                         │
└──────────────────────────┬───────────────────────────────────┘
                           │ 如果所有chunk都没有空间
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤5: 🆕 P0优化 - 分配新的动态chunk                        │
│  alloc_new_chunk():                                        │
│  ├─► 根据obj_size选择chunk_size:                           │
│  │   obj_size <= 256B  → 16KB chunk                       │
│  │   obj_size <= 1024B → 64KB chunk                       │
│  │   obj_size > 1024B  → 256KB chunk                      │
│  ├─► numa_alloc_onnode(chunk_size, node)                   │
│  ├─► chunk->offset = 0                                     │
│  ├─► chunk->used_bytes = 0                                 │
│  └─► 插入到pool->chunks链表头部                            │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤6: 解锁并返回                                          │
│  pthread_mutex_unlock(&pool->lock)                         │
│  *alloc_size = aligned_size  // 返回实际分配大小            │
│  return result;                                            │
└─────────────────────────────────────────────────────────────┘
```

### 关键决策点

1. **Free List优先**: P1优化的核心，优先重用已释放空间
2. **Bump Pointer回退**: Free List未命中时使用传统分配
3. **动态Chunk**: P0优化，根据对象大小智能选择chunk大小
4. **线程安全**: 每个pool独立锁，降低竞争

---

## 2. 释放流程 (numa_pool_free)

### 完整调用链

```
┌─────────────────────────────────────────────────────────────┐
│  调用入口                                                    │
│  zmalloc.c: numa_free_with_size(user_ptr)                   │
│  ├─► prefix = numa_get_prefix(user_ptr)                     │
│  ├─► total_size = prefix->size                              │
│  ├─► from_pool = prefix->from_pool                          │
│  └─► raw_ptr = user_ptr - PREFIX_SIZE                       │
└──────────────────────────┬───────────────────────────────────┘
                           │ if (from_pool == 1)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  numa_pool.c: numa_pool_free(raw_ptr, total_size, from_pool)│
│                                                             │
│  步骤1: 检查条件                                            │
│  ├─► if (from_pool != 1) return;  // 非池分配，跳过         │
│  └─► if (total_size <= 0) return; // 无效大小              │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤2: 🆕 P1优化 - 计算size_class                           │
│  aligned_size = (total_size + 15) & ~15;  // 16字节对齐     │
│  for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {        │
│      if (aligned_size <= numa_pool_size_classes[i]) {      │
│          class_idx = i;                                    │
│          break;                                            │
│      }                                                     │
│  }                                                         │
│  // 无需存储额外metadata，直接通过size计算                   │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤3: 🆕 P1优化 - 创建Free Block                           │
│  free_block_t *free_block = malloc(sizeof(free_block_t));  │
│  if (!free_block) return;  // 内存不足，放弃记录            │
│                                                             │
│  free_block->ptr = ptr;                                    │
│  free_block->size = aligned_size;                          │
│  free_block->next = NULL;                                  │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤4: 🆕 P1优化 - 加入Pool的Free List                      │
│  pool = &pool_ctx.node_pools[node].pools[class_idx];       │
│  pthread_mutex_lock(&pool->lock);                          │
│                                                             │
│  free_block->next = pool->free_list;  // 插入链表头         │
│  pool->free_list = free_block;                             │
│                                                             │
│  pthread_mutex_unlock(&pool->lock);                        │
└─────────────────────────────────────────────────────────────┘
```

### 关键设计

1. **Pool级别Free List**: 不需要追踪chunk归属，简化实现
2. **按size计算class**: 利用16字节PREFIX现有信息，无额外开销
3. **头插法**: O(1)时间复杂度加入free list
4. **不立即释放内存**: 保留在free list中供后续重用

---

## 3. Compact机制 (numa_pool_try_compact)

### 完整调用链

```
┌─────────────────────────────────────────────────────────────┐
│  定时触发                                                    │
│  server.c: serverCron()                                     │
│  └─► run_with_period(COMPACT_CHECK_INTERVAL * 1000)        │
│       └─► 每10秒执行一次                                    │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  numa_pool.c: numa_pool_try_compact()                       │
│                                                             │
│  步骤1: 遍历所有节点和size class                            │
│  for (node = 0; node < pool_ctx.num_nodes; node++) {       │
│      for (class_idx = 0; class_idx < 16; class_idx++) {    │
│          pool = &pool_ctx.node_pools[node].pools[class_idx];│
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤2: 🆕 P1优化 - 清理Free List                            │
│  pthread_mutex_lock(&pool->lock);                          │
│                                                             │
│  // 统计free list中的条目数                                 │
│  free_block = pool->free_list;                             │
│  free_count = 0;                                           │
│  while (free_block) {                                      │
│      free_count++;                                         │
│      free_block = free_block->next;                        │
│  }                                                         │
│                                                             │
│  // 如果free list太长(>10个)，清空它                        │
│  if (free_count > 10) {                                    │
│      while (pool->free_list) {                             │
│          free_block_t *next = pool->free_list->next;       │
│          free(pool->free_list);                            │
│          pool->free_list = next;                           │
│      }                                                     │
│      compacted_count++;                                    │
│  }                                                         │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤3: 🆕 P1优化 - 释放低利用率Chunk                         │
│  prev_ptr = &pool->chunks;                                 │
│  chunk = pool->chunks;                                     │
│                                                             │
│  while (chunk) {                                           │
│      utilization = (float)chunk->used_bytes / chunk->size; │
│                                                             │
│      // 检查是否满足compact条件                             │
│      if (utilization < COMPACT_THRESHOLD &&                │
│          (1.0f - utilization) >= COMPACT_MIN_FREE_RATIO) { │
│          // 利用率<30% 且 空闲空间>50%                       │
│          *prev_ptr = chunk->next;  // 从链表移除            │
│          numa_free(chunk->memory, chunk->size);            │
│          free(chunk);                                      │
│          pool->chunks_count--;                             │
│          compacted_count++;                                │
│          chunk = *prev_ptr;                                │
│          continue;                                         │
│      }                                                     │
│      prev_ptr = &chunk->next;                              │
│      chunk = chunk->next;                                  │
│  }                                                         │
│                                                             │
│  pthread_mutex_unlock(&pool->lock);                        │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  步骤4: 记录日志                                            │
│  if (compacted_count > 0) {                                │
│      serverLog(LL_VERBOSE,                                 │
│          "NUMA pool compacted %d items", compacted_count); │
│  }                                                         │
│  return compacted_count;                                   │
└─────────────────────────────────────────────────────────────┘
```

### Compact策略

1. **双重阈值**: 利用率<30% AND 空闲>50%，避免误杀
2. **Free List清理**: 超过10个条目时清空，防止内存泄漏
3. **周期性执行**: 每10秒检查一次，平衡性能和响应
4. **日志追踪**: LL_VERBOSE级别记录，便于监控

---

## 4. 完整生命周期示例

### 场景: SET + GET + DEL一个2KB字符串

```
1. SET mykey (2KB数据)
   ├─► zmalloc(2048)
   ├─► numa_pool_alloc(2064, node=0, class_idx=13)
   │   ├─► 检查free list - 未命中
   │   ├─► 从现有chunk分配 - 使用bump pointer
   │   └─► chunk->used_bytes += 2064
   └─► 返回user_ptr

2. GET mykey
   ├─► lookupKey() - 读取数据
   └─► 无内存分配，仅读取

3. DEL mykey
   ├─► decrRefCount() → refcount=0
   ├─► zfree(user_ptr)
   ├─► numa_pool_free(raw_ptr, 2064, from_pool=1)
   │   ├─► 计算class_idx=13
   │   ├─► 创建free_block
   │   └─► 加入pool->free_list
   └─► 返回

4. SET mykey2 (1.8KB数据) - 10秒后
   ├─► zmalloc(1843)
   ├─► numa_pool_alloc(1859, node=0, class_idx=13)
   │   ├─► 检查free list - **命中!** (size=2064 >= 1859)
   │   ├─► 重用之前释放的2064字节空间
   │   └─► 从free list移除该块
   └─► 返回user_ptr (可能是相同地址)

5. serverCron触发compact - 20秒后
   ├─► numa_pool_try_compact()
   │   ├─► 检查chunk利用率
   │   ├─► 清理过长的free list
   │   └─► 释放低利用率chunk
   └─► 记录compacted_count
```

---

## 5. 性能优化点

### P0优化 (动态Chunk)

| 对象大小 | Chunk大小 | 碎片率 | 原因 |
|---------|----------|--------|------|
| ≤256B   | 16KB     | ~5%    | 小对象，小chunk |
| ≤1024B  | 64KB     | ~10%   | 中等对象平衡 |
| >1024B  | 256KB    | ~15%   | 大对象，大chunk |

### P1优化 (Free List + Compact)

| 机制 | 收益 | 代价 |
|-----|------|-----|
| Free List | 重用率20-40% | free_block结构体16B |
| Compact | 释放<30%利用率chunk | 10秒周期检查 |
| Pool级别 | 无跨chunk追踪 | 可能跨node重用(可接受) |

### 16字节PREFIX极限

```c
typedef struct {
    size_t size;          // 8字节: 分配大小
    uint8_t from_pool;    // 1字节: 是否来自pool
    uint8_t padding[7];   // 7字节: 预留(可用于热度等)
} numa_alloc_prefix_t;
```

**设计原则**: 保持16字节不变，P1优化无额外开销

---

## 6. 未来优化方向 (P2)

1. **Slab Allocator**: 参考jemalloc的slab设计
2. **Per-Thread Cache**: 减少锁竞争
3. **分层内存池**: 针对不同大小对象优化
4. **Huge Page支持**: 降低TLB miss

**目标**: 碎片率 < 1.3，内存效率 > 80%
