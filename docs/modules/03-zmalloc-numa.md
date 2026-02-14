# 03-zmalloc-numa.md - zmalloc NUMA适配模块

## 模块概述

**文件**: `src/zmalloc.c`, `src/zmalloc.h`

**功能**: 改造Redis标准内存分配器，使其支持NUMA感知分配，同时保持与原有接口完全兼容。

**设计目标**:
- 向后兼容：上层代码零修改
- NUMA感知：根据当前CPU自动选择节点
- 性能优化：集成内存池减少系统调用
- 元数据管理：PREFIX机制跟踪内存信息

---

## 业务调用链

### 标准分配流程

```
┌─────────────────────────────────────────────────────────────┐
│  Redis上层代码                                               │
│  sdsnew(), createObject(), dictAdd() ...                    │
└──────────────────────────┬──────────────────────────────────┘
                           │ zmalloc(size)
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  zmalloc.c: zmalloc()                                       │
│  └──► ztrymalloc_usable(size, NULL)                         │
└──────────────────────────┬──────────────────────────────────┘
                           │
           ┌───────────────┼───────────────┐
           │               │               │
           ▼               ▼               ▼
    ┌────────────┐  ┌────────────┐  ┌────────────┐
    │ HAVE_NUMA  │  │HAVE_MALLOC_│  │ 标准libc   │
    │  numa_     │  │   SIZE     │  │ 分配器     │
    │ alloc_with_│  │  malloc()  │  │            │
    │   size()   │  │            │  │            │
    └─────┬──────┘  └─────┬──────┘  └─────┬──────┘
          │               │               │
          ▼               ▼               ▼
    ┌─────────────────────────────────────────┐
    │  numa_pool_alloc() 或 numa_alloc_onnode │
    │  malloc() / je_malloc() / tc_malloc()   │
    └─────────────────────────────────────────┘
```

### NUMA分配详细流程 (P2: Slab+Pool双路径)

```
┌─────────────────────────────────────────────────────────────┐
│  numa_alloc_with_size(size)                                 │
│  1. 计算 total_size = size + PREFIX_SIZE (16B)              │
│  2. 获取 current_node = numa_pool_get_node()                │
└──────────────────────────┬──────────────────────────────────┘
                           │
           ┌───────────────┴───────────────┐
           │                               │
           ▼                               ▼
    ┌──────────────┐              ┌──────────────┐
    │ ≤128B 小对象  │              │ >128B 大对象  │
    │ (Slab路径)    │              │ (Pool路径)    │
    └──────┬───────┘              └──────┬───────┘
           │                             │
           ▼                             ▼
    ┌──────────────┐              ┌──────────────┐
    │numa_slab_    │              │numa_pool_    │
    │alloc(size)   │              │alloc(size)   │
    │              │              │              │
    │• 16KB slab   │              │• 256KB chunk │
    │• Bitmap O(1) │              │• Bump pointer│
    │• 128 objects │              │• Free list   │
    └──────┬───────┘              └──────┬───────┘
           │                             │
           │ from_pool = 2               │ from_pool = 1
           │                             │
           └───────────┬─────────────────┘
                       │
                       ▼
┌─────────────────────────────────────────────────────────────┐
│  numa_init_prefix(raw_ptr, size, from_pool, node_id)        │
│  - 写入size                                                 │
│  - 写入from_pool (0=直接, 1=Pool, 2=Slab)                   │
│  - 写入node_id (P2修复: 记录分配节点用于正确释放)           │
│  - 返回 user_ptr = raw_ptr + PREFIX_SIZE                    │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  返回用户指针（指向实际数据区域）                             │
└─────────────────────────────────────────────────────────────┘
```

### 释放流程 (P2修复后)

```
┌─────────────────────────────────────────────────────────────┐
│  zfree(user_ptr)                                            │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  numa_get_prefix(user_ptr)                                  │
│  └──► prefix = user_ptr - PREFIX_SIZE                       │
│       - 读取 size                                           │
│       - 读取 from_pool (0=直接, 1=Pool, 2=Slab)             │
│       - 读取 node_id (P2修复: 正确的目标节点)               │
└──────────────────────────┬──────────────────────────────────┘
                           │
       ┌───────────────────┼───────────────────┐
       │                   │                   │
       ▼                   ▼                   ▼
┌──────────────┐   ┌──────────────┐   ┌──────────────┐
│ from_pool=0  │   │ from_pool=1  │   │ from_pool=2  │
│ (直接分配)    │   │ (Pool分配)   │   │ (Slab分配)   │
└──────┬───────┘   └──────┬───────┘   └──────┬───────┘
       │                  │                  │
       ▼                  │                  ▼
┌──────────────┐          │         ┌──────────────┐
│numa_free()   │          │         │numa_slab_    │
│归还系统      │          │         │free()        │
└──────────────┘          │         │              │
                          │         │• 读取header  │
                          │         │• 定位bitmap │
                          │         │• 原子清除bit│
                          │         │• O(1)复杂度 │
                          │         └──────────────┘
                          │
                          ▼
                   池内存不单独释放
                   （程序结束时统一清理）

P2关键修复:
┌─────────────────────────────────────────────────────────────┐
│  numa_free_with_size() 现在使用 prefix->node_id             │
│  而不是调用 numa_pool_get_node() 获取当前节点               │
│                                                             │
│  修复前: node = numa_pool_get_node()  ← 错误!当前CPU可能    │
│                                         不是分配时的节点    │
│  修复后: node = prefix->node_id       ← 正确!使用原始节点   │
└─────────────────────────────────────────────────────────────┘
```

---

## 核心数据结构

### 1. PREFIX结构（NUMA版本）

```c
typedef struct {
    size_t size;           /* 8字节 - 用户请求的内存大小 */
    char from_pool;        /* 1字节 - 是否来自内存池 (1=是, 0=否) */
    char node_id;          /* 1字节 - NUMA节点ID (P2修复: 正确释放路由) */
    char padding[6];       /* 6字节 - 填充对齐 */
} numa_alloc_prefix_t;

#define PREFIX_SIZE (sizeof(numa_alloc_prefix_t))  /* 16字节 */
```

**内存布局**:
```
分配100字节 (P2修复后):
┌──────────────────────┬─────────────────────────────────────────┐
│  size = 100          │         100字节用户数据                  │
│  from_pool = 0/1/2   │                                         │
│  node_id = 0/1/2/... │  from_pool: 0=直接分配, 1=Pool, 2=Slab  │
│  padding[6]          │  node_id: 记录分配时的NUMA节点          │
│       (16字节)       │                                         │
├──────────────────────┴─────────────────────────────────────────┤
│  实际分配: 116字节                                              │
│  zmalloc返回: 用户数据开始地址 (raw_ptr + 16)                   │
└────────────────────────────────────────────────────────────────┘
       ▲
       └── 实际numa_alloc_onnode返回的地址
```

### 2. NUMA上下文

```c
static struct {
    int numa_available;           /* NUMA是否可用 */
    int num_nodes;                /* NUMA节点数量 */
    int current_node;             /* 当前节点 */
    int allocation_strategy;      /* 分配策略 */
    int *node_distance_order;     /* 按距离排序的节点数组 */
} numa_ctx = {0};
```

### 3. 线程本地存储

```c
static __thread int tls_current_node = -1;  /* 线程当前节点 */
```

---

## 核心算法

### 1. 初始化算法

```c
void numa_init(void)
{
    /* 1. 检查NUMA可用性 */
    if (numa_available() == -1) {
        numa_ctx.numa_available = 0;
        return;
    }
    
    /* 2. 初始化内存池模块 */
    if (numa_pool_init() != 0) {
        numa_ctx.numa_available = 0;
        return;
    }
    
    /* 3. 获取系统信息 */
    numa_ctx.num_nodes = numa_pool_num_nodes();
    numa_ctx.current_node = numa_pool_get_node();
    
    /* 4. 计算节点距离顺序 */
    for (int i = 0; i < numa_ctx.num_nodes; i++) {
        numa_ctx.node_distance_order[i] = i;
    }
    /* 按numa_distance排序... */
}
```

### 2. 分配算法

```c
static void *numa_alloc_with_size(size_t size)
{
    ASSERT_NO_SIZE_OVERFLOW(size);
    
    size_t total_size = size + PREFIX_SIZE;
    size_t alloc_size;
    
    /* 1. 尝试从内存池分配 */
    void *raw_ptr = numa_pool_alloc(total_size, numa_ctx.current_node, &alloc_size);
    if (!raw_ptr)
        return NULL;
    
    /* 2. 判断是否来自内存池 */
    int from_pool = (total_size <= NUMA_POOL_MAX_ALLOC) ? 1 : 0;
    
    /* 3. 初始化PREFIX */
    numa_init_prefix(raw_ptr, size, from_pool);
    
    /* 4. 更新统计 */
    update_zmalloc_stat_alloc(total_size);
    
    /* 5. 返回用户指针 */
    return numa_to_user_ptr(raw_ptr);
}
```

### 3. 释放算法

```c
static void numa_free_with_size(void *user_ptr)
{
    if (user_ptr == NULL)
        return;
    
    /* 1. 获取PREFIX */
    numa_alloc_prefix_t *prefix = numa_get_prefix(user_ptr);
    size_t total_size = prefix->size + PREFIX_SIZE;
    
    /* 2. 更新统计 */
    update_zmalloc_stat_free(total_size);
    
    /* 3. 释放内存 */
    void *raw_ptr = (char *)user_ptr - PREFIX_SIZE;
    numa_pool_free(raw_ptr, total_size, prefix->from_pool);
}
```

---

## 接口函数

### 标准分配接口（保持兼容）

```c
void *zmalloc(size_t size);           /* 分配内存 */
void *zcalloc(size_t size);           /* 分配并清零 */
void *zrealloc(void *ptr, size_t size); /* 重新分配 */
void zfree(void *ptr);                /* 释放内存 */
size_t zmalloc_size(void *ptr);       /* 获取内存大小 */
```

### NUMA专用接口

```c
/* 初始化与配置 */
void numa_init(void);
void numa_cleanup(void);
int numa_set_strategy(int strategy);
int numa_get_strategy(void);

/* NUMA感知分配 */
void *numa_zmalloc(size_t size);
void *numa_zcalloc(size_t size);
void *numa_zrealloc(void *ptr, size_t size);
void numa_zfree(void *ptr);

/* 指定节点分配 */
void *numa_zmalloc_onnode(size_t size, int node);
void *numa_zcalloc_onnode(size_t size, int node);

/* 节点管理 */
void numa_set_current_node(int node);
int numa_get_current_node(void);
```

---

## 策略配置

### 分配策略

```c
#define NUMA_STRATEGY_LOCAL_FIRST 0  /* 优先本地节点 */
#define NUMA_STRATEGY_INTERLEAVE  1  /* 跨节点交错分配 */
```

**默认策略**: LOCAL_FIRST
- 在本地NUMA节点分配内存
- 本地节点不足时按距离顺序选择其他节点

---

## 性能数据

### P2修复前 (存在性能bug)

| 操作 | 标准Redis (jemalloc) | NUMA版本 (有bug) |
|------|---------------------|-----------------|
| SET | ~169K req/s | **14-19K req/s** ⚠️ 严重下降 |
| 延迟p50 | ~0.15ms | **10-50ms** ⚠️ 严重延迟 |

**问题**: Slab释放时NUMA节点选择错误，导致内存泄漏和性能骤降

### P2修复后 (v3.2-P2, 2026-02-14)

| 操作 | 标准Redis (jemalloc) | NUMA版本 (P2修复) | 达成率 |
|------|---------------------|-------------------|--------|
| SET | ~169K req/s | **154-166K req/s** | **93-98%** |
| GET | ~169K req/s | **154-166K req/s** | **93-98%** |
| 延迟p50 | ~0.15ms | **0.15-0.2ms** | 相当 |
| 碎片率 | ~1.0 | **1.02-1.03** | 优秀 |

**8G内存压力测试结果**:
| 指标 | 数值 |
|------|------|
| 测试数据量 | 10 million keys |
| RSS内存占用 | 7.86 GB |
| 内存碎片率 | **1.03** |
| 内存效率 | **97%** |

**结论**: P2修复后性能达到标准Redis的93-98%，碎片率控制在1.03

---

## 设计决策

### 为什么需要16字节PREFIX？

| 字段 | 大小 | 用途 |
|------|------|------|
| size | 8字节 | 记录用户请求大小（libNUMA无法查询） |
| from_pool | 1字节 | 区分池分配/直接分配，释放策略不同 |
| padding | 7字节 | 对齐到16字节，优化CPU访问 |

**对比原有PREFIX**:
- 标准libc模式: 8字节（仅size）
- NUMA模式: 16字节（size + from_pool + padding）

### 为什么保持zmalloc接口不变？

- **兼容性**: 所有现有Redis代码无需修改
- **透明性**: 上层模块不感知NUMA存在
- **可回退**: NUMA不可用时自动使用标准分配器

### 为什么使用线程本地存储？

```c
static __thread int tls_current_node = -1;
```

- **性能**: 避免每次分配都调用sched_getcpu()
- **一致性**: 线程内保持节点偏好稳定
- **灵活性**: 支持按线程设置节点偏好

---

## 代码示例

### 基本使用

```c
/* 无需修改现有代码，自动使用NUMA分配 */
sds mystring = sdsnew("Hello, NUMA!");
robj *obj = createStringObject("key", 3);

/* NUMA专用：指定节点分配 */
void *node_local = numa_zmalloc_onnode(1024, 0);
void *node_remote = numa_zmalloc_onnode(1024, 1);
```

### 检查内存位置

```c
/* 获取内存大小（通过PREFIX） */
size_t size = zmalloc_size(ptr);

/* 获取当前节点 */
int node = numa_get_current_node();
```

---

## 限制与注意事项

1. **PREFIX开销**: 每个分配额外16字节元数据
2. **内存碎片**: 小对象按大小级别对齐，有一定内存浪费
3. **线程安全**: zmalloc本身是线程安全的，但节点设置需要同步
4. **NUMA依赖**: 需要libnuma库和NUMA硬件支持
