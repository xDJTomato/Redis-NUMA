# 01-numa-pool.md - NUMA内存池模块

## 模块概述

**文件**: `src/numa_pool.h`, `src/numa_pool.c`

**功能**: 为每个NUMA节点提供独立的内存池，减少`numa_alloc_onnode`系统调用次数，提升内存分配性能。

**设计目标**:
- 批量分配64KB内存块，按需切分小对象
- 8级大小分类覆盖Redis大部分小对象
- O(1)时间复杂度的分配算法
- 线程安全（per-pool互斥锁）

---

## 业务调用链

```
┌─────────────────────────────────────────────────────────────┐
│  调用入口                                                    │
│  zmalloc.c: numa_alloc_with_size()                          │
└──────────────────────────┬──────────────────────────────────┘
                           │ numa_pool_alloc(size, node, &alloc_size)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  numa_pool.c: numa_pool_alloc()                             │
│  1. 检查size <= NUMA_POOL_MAX_ALLOC (512B)                  │
│  2. 查找合适的大小级别 (16/32/64/128/256/512/1024/2048)     │
│  3. 获取对应节点的pool                                      │
│  4. 加锁 (pthread_mutex_lock)                               │
│  5. 尝试从现有chunk分配 (bump pointer)                      │
│  6. 如无空间，分配新64KB chunk                              │
│  7. 解锁                                                    │
│  8. 返回内存地址                                            │
└──────────────────────────┬──────────────────────────────────┘
                           │ 回退：numa_alloc_onnode()
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  libnuma: numa_alloc_onnode()                               │
└─────────────────────────────────────────────────────────────┘
```

---

## 核心数据结构

### 1. 内存块 (numa_pool_chunk_t)

```c
typedef struct numa_pool_chunk {
    void *memory;                  /* NUMA-allocated memory */
    size_t size;                   /* Chunk size (64KB) */
    size_t offset;                 /* Current allocation offset */
    struct numa_pool_chunk *next;  /* Next chunk in list */
} numa_pool_chunk_t;
```

**内存布局**:
```
Chunk (64KB):
├─ offset: 0      ├─ offset: 256   ├─ offset: 512   ├─ ...
│  [alloc 1]      │  [alloc 2]     │  [alloc 3]     │
│  256 bytes      │  256 bytes     │  256 bytes     │
└─────────────────┴────────────────┴────────────────┘
```

### 2. 大小级别池 (numa_size_class_pool_t)

```c
typedef struct {
    size_t obj_size;               /* Object size for this class */
    numa_pool_chunk_t *chunks;     /* Chunk list */
    pthread_mutex_t lock;          /* Thread safety */
    size_t chunks_count;           /* Statistics */
} numa_size_class_pool_t;
```

### 3. 节点池 (numa_node_pool_t)

```c
typedef struct numa_node_pool {
    int node_id;
    numa_size_class_pool_t pools[NUMA_POOL_SIZE_CLASSES];  /* 8 pools */
    numa_pool_stats_t stats;
} numa_node_pool_t;
```

### 4. 全局上下文

```c
static struct {
    int initialized;
    int numa_available;
    int num_nodes;
    int current_node;
    numa_node_pool_t *node_pools;  /* Array of node pools */
    pthread_mutex_t init_lock;
} pool_ctx;
```

---

## 核心算法

### 1. Bump Pointer分配算法

```c
/* 伪代码 */
void *bump_pointer_alloc(pool, size) {
    aligned_size = (size + 15) & ~15;  /* 16-byte align */
    
    for each chunk in pool.chunks {
        if (chunk.offset + aligned_size <= chunk.size) {
            ptr = chunk.memory + chunk.offset;
            chunk.offset += aligned_size;
            return ptr;
        }
    }
    
    /* No space, allocate new chunk */
    new_chunk = alloc_new_chunk(pool);
    add to pool.chunks;
    return bump_pointer_alloc(pool, size);  /* Retry */
}
```

**时间复杂度**: O(1) - 通常第一个chunk就有空间

### 2. 大小级别选择

```c
static const size_t numa_pool_size_classes[8] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

/* 选择逻辑 */
for (int i = 0; i < NUMA_POOL_SIZE_CLASSES; i++) {
    if (alloc_size <= numa_pool_size_classes[i]) {
        return &node_pool->pools[i];
    }
}
```

---

## 配置参数

| 参数 | 值 | 说明 |
|------|-----|------|
| NUMA_POOL_SIZE_CLASSES | 8 | 大小级别数量 |
| NUMA_POOL_CHUNK_SIZE | 64KB | 每个chunk的大小 |
| NUMA_POOL_MAX_ALLOC | 512B | 内存池分配的最大对象 |
| Size Classes | 16/32/64/128/256/512/1024/2048 | 各级别大小 |

---

## 接口函数

### 初始化与清理

```c
int numa_pool_init(void);           /* 初始化所有节点的内存池 */
void numa_pool_cleanup(void);       /* 清理所有内存池 */
```

### 内存分配与释放

```c
void *numa_pool_alloc(size_t size, int node, size_t *total_size);
void numa_pool_free(void *ptr, size_t total_size, int from_pool);
```

### 节点管理

```c
void numa_pool_set_node(int node);
int numa_pool_get_node(void);
int numa_pool_num_nodes(void);
int numa_pool_available(void);
```

### 统计信息

```c
void numa_pool_get_stats(int node, numa_pool_stats_t *stats);
void numa_pool_reset_stats(void);
```

---

## 性能数据

| 指标 | 无内存池 | 有内存池 | 提升 |
|------|---------|---------|------|
| SET | 25K req/s | 166K req/s | 6.6x |
| GET | 27K req/s | 166K req/s | 6.2x |
| 延迟p50 | 1-2ms | 0.15-0.2ms | 10x |

---

## 设计决策

### 为什么使用64KB chunk？

- 覆盖Redis大部分小对象（sds、robj等）
- 64KB = 128个512B对象，命中率合理
- 与Linux页大小对齐，减少内存碎片

### 为什么池内存不单独释放？

- Redis是长期运行的内存数据库
- 小对象频繁分配/释放，池内存会被快速重用
- 简化实现，避免复杂的内存回收机制
- 程序结束时统一清理所有chunks

### 为什么使用per-pool锁而非全局锁？

- 不同大小级别的分配互不干扰
- 减少锁竞争，提高并发性能
- 每个pool独立管理自己的chunks
