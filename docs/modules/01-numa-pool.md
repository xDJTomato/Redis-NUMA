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
| NUMA_POOL_SIZE_CLASSES | 16 | 大小级别数量 (P0优化后) |
| NUMA_POOL_MAX_ALLOC | 512B | 内存池分配的最大对象 |
| Size Classes | 16/32/48/64/96/128/192/256/384/512/768/1024/1536/2048/3072/4096 | 各级别大小 (P0优化后) |
| Chunk Size | 动态选择 16KB/64KB/256KB | 根据对象大小选择 (P0优化后) |

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

### 基准测试结果

| 指标 | 无内存池 | 有内存池 | 提升 |
|------|---------|---------|------|
| SET | 25K req/s | 166K req/s | 6.6x |
| GET | 27K req/s | 166K req/s | 6.2x |
| 延迟p50 | 1-2ms | 0.15-0.2ms | 10x |

### P0优化成果 (2026-02-14)

| 指标 | 优化前 | P0优化后 | 改善 |
|------|--------|-----------|------|
| 碎片率 | 3.61 | **2.36** | ↓ 36% |
| 内存效率 | 27% | **43%** | ↑ 59% |
| SET性能 | 169K | 476K | ↑ 181% |
| GET性能 | 188K | 793K | ↑ 321% |

**P0优化措施**:
- ✅ 扩展size_class从8级到16级
- ✅ 实现动态chunk大小策略 (16KB/64KB/256KB)

**详细分析**：见 [09-memory-fragmentation-analysis.md](./09-memory-fragmentation-analysis.md)

---

## 优化路线图

### P0优化 (已完成 ✅)

1. ✅ 扩展size_class从8级到16级
2. ✅ 实现动态chunk大小策略
3. ✅ 文档清理与整合

**效果**：碎片率3.61→2.36 (改善36%)

### P1优化 (已完成 ✅)

4. ✅ Free List管理 - 记录并重用已释放空间（pool级别free list）
5. ✅ Compact机制 - 整理低利用率chunk（利用率<30%时释放）
6. ✅ serverCron集成 - 每10秒检查一次compact

**效果**：碎片率2.36→2.00 (改善15%)

**总计改善**：碎片率3.61→2.00 (总计改善45%)

**技术亮点**：
- 保持16字节PREFIX设计不变，无额外开销
- Free list在pool级别管理，避免跨chunk查找
- Compact阈值30%，定期清理低利用率chunk

### P2优化 (已完成 ✅)

7. ✅ Slab Allocator机制 - 4KB slab针对小对象(≤512B)
8. ✅ Bitmap管理 - O(1)分配和释放复杂度
9. ✅ 与Pool共存 - 小对象走Slab，大对象走Pool

**效果**：碎片率2.00→1.02 (改善49%)

**总计改善**：碎片率3.61→1.02 (总计改善72%，内存效率提升263%)

**技术亮点**：
- 固定4KB slab大小，覆盖≤512B小对象
- 128位bitmap管理，支持128个对象
- 三级链表：partial_slabs、full_slabs、empty_slabs
- Empty slab缓存（最多2个）避免频繁分配
- 每个NUMA节点独立slab池

**详细优化方案**：见 [09-memory-fragmentation-analysis.md](./09-memory-fragmentation-analysis.md)

---

## 测试验证

每次优化后需执行：

```bash
# 快速功能测试
cd tests && ./numa_cxl_stress_test.sh quick

# 检查关键指标
cat reports/diagnosis_*/fragmentation.txt

# 完整压力测试
./numa_cxl_stress_test.sh full
```

**成功标准**:
- 碎片率 < 1.3
- SET性能 > 150K req/s
- 能够填充90%以上的目标数据量
- 无OOM或性能严重下降
