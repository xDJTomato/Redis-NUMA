# 03-zmalloc-numa.md - zmalloc NUMA适配深度解析

## 模块概述

**核心文件**: `src/zmalloc.c`, `src/zmalloc.h`
**关联模块**: `src/numa_pool.h/c`（包含Pool和Slab双路径实现）
**核心功能**: 将Redis标准内存分配器改造为NUMA感知版本，在保持完全API兼容性的同时，实现节点本地化的高性能内存分配。

### NUMA适配的必要性

传统的Redis内存分配器存在以下问题：

**1. 缺乏NUMA意识**：
```c
// 传统分配器 - 无法感知NUMA拓扑
void *ptr = malloc(1024);  // 可能在任意节点分配
// 当CPU在Node 0访问Node 1的内存时，延迟增加2-3倍
```

**2. 性能损失**：
- 本地访问：~50-100ns
- 远程访问：~150-300ns
- 性能差异：2-3倍延迟

**3. 资源利用不均**：
- 热点数据集中在少数节点
- 其他节点内存闲置
- 系统整体性能受限

### NUMA适配的核心挑战

**1. 兼容性要求**：
- 不能改变现有Redis代码
- 保持所有zmalloc接口不变
- 向后兼容现有应用

**2. 性能要求**：
- NUMA感知不能带来性能损失
- 理想情况下应该提升性能
- 保持Redis原有的高并发特性

**3. 复杂性控制**：
- 不增加系统复杂度
- 错误处理机制完善
- 调试和监控友好

### 解决方案架构

```
┌─────────────────────────────────────────────────────────────┐
│                    Redis应用层                               │
│  sdsnew(), createObject(), dictAdd() 等                     │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│               zmalloc API兼容层 (零修改)                     │
│  zmalloc() / zfree() / zrealloc() - 接口完全不变            │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              NUMA感知分配决策层                              │
│  numa_alloc_with_size() - 根据CPU位置选择最优节点           │
└──────────────────────────┬──────────────────────────────────┘
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
        ▼                  ▼                  ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ Slab分配器    │  │ Pool分配器    │  │ 直接分配      │
│ (≤128B)      │  │ (>128B)      │  │ (>4KB)       │
│ O(1)性能     │  │ 批量预分配    │  │ 系统调用     │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │
       ▼                 ▼                 ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ numa_slab_   │  │ numa_pool_   │  │ numa_alloc_  │
│ alloc()      │  │ alloc()      │  │ onnode()     │
└──────────────┘  └──────────────┘  └──────────────┘
```

这种设计实现了"**透明NUMA优化**"：应用层无需任何修改，底层自动实现NUMA感知分配。

---

## 核心分配流程深度解析

### 完整调用链路

让我们从Redis最常见的内存分配场景开始，深入分析整个调用过程：

```
┌─────────────────────────────────────────────────────────────┐
│  Redis应用层示例                                             │
│  robj *obj = createStringObject("hello", 5);                │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  object.c: createStringObject()                             │
│  sds s = sdsnewlen(init, len);                              │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  sds.c: sdsnewlen()                                         │
│  sh = s_malloc(size);                                       │
│  // #define s_malloc zmalloc                                │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  zmalloc.c: zmalloc(size_t size)                            │
│  // src/zmalloc.c:159-165                                  │
│  if (size > 0) {                                           │
│      return ztrymalloc_usable(size, NULL);                 │
│  }                                                         │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  zmalloc.c: ztrymalloc_usable(size_t size, size_t *usable)  │
│  // src/zmalloc.c:139-157                                  │
│  #if defined(USE_NU                                                                                                     │
│  Ma) && defined(HAVE_NUMA_H)                               │
│      void *ptr = numa_alloc_with_size(size);               │
│  #elif defined(USE_NU                                                                                                   │
│  Ma) && defined(HAVE_MALLOC_SIZE)                          │
│      void *ptr = malloc(size);                             │
│  #else                                                     │
│      void *ptr = malloc(size);                             │
│  #endif                                                    │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  zmalloc.c: numa_alloc_with_size(size_t size)               │
│  // src/zmalloc.c:252-289                                  │
│  size_t total_size = size + PREFIX_SIZE;                   │
│  size_t alloc_size;                                        │
│  int target_node = get_current_numa_node();                │
│                                                            │
│  // P2优化：≤128B走Slab路径                                │
│  if (should_use_slab(size)) {                              │
│      raw_ptr = numa_slab_alloc(size, target_node, &alloc_size);
│  } else {                                                  │
│      raw_ptr = numa_pool_alloc(total_size, target_node, &alloc_size);
│  }                                                         │
│                                                            │
│  numa_init_prefix(raw_ptr, size, from_pool, target_node);  │
│  return numa_to_user_ptr(raw_ptr);                         │
└──────────────────────────┬──────────────────────────────────┘
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
        ▼                  ▼                  ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ Slab路径      │  │ Pool路径      │  │ 直接路径      │
│ (≤128B)      │  │ (>128B)      │  │ (>4KB)       │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │
       ▼                 ▼                 ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│numa_slab_    │  │numa_pool_    │  │numa_alloc_   │
│alloc()       │  │alloc()       │  │onnode()      │
└──────────────┘  └──────────────┘  └──────────────┘
```

### PREFIX机制详解

NUMA版本的zmalloc引入了16字节的PREFIX来存储元数据：

```c
// src/zmalloc.h:54-61
typedef struct {
    size_t size;           /* 8字节 - 用户请求的内存大小 */
    char from_pool;        /* 1字节 - 来源标记 */
    char node_id;          /* 1字节 - NUMA节点ID */
    char padding[6];       /* 6字节 - 填充对齐到16字节 */
} numa_alloc_prefix_t;

#define PREFIX_SIZE (sizeof(numa_alloc_prefix_t))  // 16字节
```

**内存布局**：
```
用户请求100字节的内存：

实际分配 (116字节):
┌─────────────────────────────────────────────────────────────┐
│  PREFIX区域 (16字节)                                        │
├─────────────────────────────────────────────────────────────┤
│  size = 100                                                 │
│  from_pool = 1 (来自Pool) 或 2 (来自Slab)                   │
│  node_id = 0 (分配时的NUMA节点)                             │
│  padding[6] = 0                                             │
├─────────────────────────────────────────────────────────────┤
│  用户数据区域 (100字节)                                      │
│  ← zmalloc() 返回这个地址给用户                             │
└─────────────────────────────────────────────────────────────┘
↑
numa_alloc_onnode()实际返回的地址
```

**关键函数实现**：
```c
// src/zmalloc.c:210-220
static inline void numa_init_prefix(void *raw_ptr, size_t user_size, 
                                   int from_pool, int node_id) {
    numa_alloc_prefix_t *prefix = (numa_alloc_prefix_t *)raw_ptr;
    prefix->size = user_size;
    prefix->from_pool = from_pool;
    prefix->node_id = node_id;
    // padding字段自动初始化为0
}

// src/zmalloc.c:222-228
static inline void *numa_to_user_ptr(void *raw_ptr) {
    return (char *)raw_ptr + PREFIX_SIZE;
}

// src/zmalloc.c:230-236
static inline numa_alloc_prefix_t *numa_get_prefix(void *user_ptr) {
    return (numa_alloc_prefix_t *)((char *)user_ptr - PREFIX_SIZE);
}
```

### 释放流程详解

释放内存时需要正确处理PREFIX信息：

```c
// src/zmalloc.c:310-325
void zfree(void *ptr) {
    if (ptr == NULL) return;
    
#if defined(USE_NUMA) && defined(HAVE_NUMA_H)
    numa_free_with_size(ptr);
#elif defined(USE_JEMALLOC)
    free(ptr);
#else
    free(ptr);
#endif
}

// src/zmalloc.c:292-308
static void numa_free_with_size(void *user_ptr) {
    if (user_ptr == NULL) return;
    
    // 获取PREFIX信息
    numa_alloc_prefix_t *prefix = numa_get_prefix(user_ptr);
    size_t total_size = prefix->size + PREFIX_SIZE;
    int from_pool = prefix->from_pool;
    int node_id = prefix->node_id;  // P2修复：使用原始节点ID
    
    // 更新统计信息
    update_zmalloc_stat_free(total_size);
    
    // 释放内存
    void *raw_ptr = (char *)user_ptr - PREFIX_SIZE;
    if (from_pool == 0) {
        // 直接分配的内存
        numa_free(raw_ptr, total_size);
    } else if (from_pool == 1) {
        // Pool分配的内存
        numa_pool_free(raw_ptr, total_size, 1);
    } else if (from_pool == 2) {
        // Slab分配的内存
        numa_slab_free(raw_ptr, total_size, node_id);
    }
}
```

**P2关键修复说明**：
```c
// 修复前的问题：
int node = numa_pool_get_node();  // ❌ 错误！获取的是当前CPU节点

// 修复后：
int node = prefix->node_id;       // ✅ 正确！使用分配时的节点
```
这个修复解决了Slab释放时节点选择错误导致的内存泄漏问题。

## NUMA感知机制详解

### CPU到节点的映射

NUMA感知的核心是准确获取当前CPU所在的NUMA节点：

```c
// src/zmalloc.c:256-267
static int get_current_numa_node(void) {
    // 快速路径：单节点系统
    if (numa_ctx.num_nodes == 1) {
        return 0;
    }
    
    // 快速路径：线程本地缓存
    if (tls_current_node >= 0) {
        return tls_current_node;
    }
    
    // 慢速路径：系统调用
    int cpu = sched_getcpu();
    int node = numa_node_of_cpu(cpu);
    
    // 更新线程本地缓存
    tls_current_node = node;
    return node;
}
```

**优化策略**：
1. **单节点优化**：避免不必要的系统调用
2. **TLS缓存**：减少`sched_getcpu()`调用频率
3. **轮询机制**：多节点环境下使用轮询分配

### 节点选择策略

```c
// src/zmalloc.c:269-287
static int select_target_node(size_t size) {
    int current_node = get_current_numa_node();
    
    switch (numa_ctx.allocation_strategy) {
        case NUMA_STRATEGY_LOCAL_FIRST:
            // 本地优先策略
            if (has_enough_memory(current_node, size)) {
                return current_node;
            }
            // 本地不足时按距离选择最近节点
            return find_closest_node_with_memory(current_node, size);
            
        case NUMA_STRATEGY_INTERLEAVE:
            // 交错分配策略
            static __thread int round_robin_index = 0;
            int target = round_robin_index % numa_ctx.num_nodes;
            round_robin_index++;
            return target;
            
        default:
            return current_node;
    }
}
```

### 内存压力感知

智能的内存分配还需要考虑节点的内存压力：

```c
// src/zmalloc.c:289-310
static int has_enough_memory(int node, size_t size) {
    // 获取节点内存使用情况
    long free_mem = numa_node_size64(node, NULL);
    long total_mem = get_total_memory_for_node(node);
    
    // 计算使用率
    double usage_ratio = (double)(total_mem - free_mem) / total_mem;
    
    // 预留20%的安全边际
    if (usage_ratio > 0.8) {
        return 0;  // 内存压力过大
    }
    
    // 检查是否有足够连续内存
    if (free_mem < (long)size) {
        return 0;  // 内存不足
    }
    
    return 1;  // 内存充足
}
```

这种机制确保了：
- 避免在高负载节点上分配内存
- 防止内存碎片化
- 提高整体系统稳定性

### 线程本地存储优化

```c
// src/zmalloc.c:94
static __thread int tls_current_node = -1;

// 在CPU绑定发生变化时更新TLS
void numa_update_thread_node(void) {
    int new_node = get_current_numa_node();
    if (tls_current_node != new_node) {
        tls_current_node = new_node;
        serverLog(LL_DEBUG, "Thread moved from node %d to %d", 
                  tls_current_node, new_node);
    }
}
```

TLS优化带来的收益：
- 减少系统调用开销（从每次分配都调用`sched_getcpu()`优化为缓存机制）
- 提高多线程环境下的性能
- 保持NUMA局部性

---

## 核心数据结构深度解析

### 1. NUMA上下文管理

```c
// src/zmalloc.c:80-90
static struct {
    int numa_available;              /* NUMA是否可用 */
    int num_nodes;                   /* 系统NUMA节点数 */
    int current_node;                /* 当前默认节点 */
    int allocation_strategy;         /* 分配策略 */
    int *node_distance_matrix;       /* 节点距离矩阵 */
    int *node_memory_info;           /* 各节点内存信息 */
    pthread_mutex_t ctx_lock;        /* 上下文保护锁 */
} numa_ctx = {0};

// 线程本地存储
static __thread int tls_current_node = -1;  // 当前线程所在节点
static __thread uint64_t tls_alloc_count = 0;  // 分配计数（用于缓存失效）
```

**上下文字段详解**：
- `numa_available`：运行时检测NUMA支持情况
- `num_nodes`：系统实际的NUMA节点数量
- `current_node`：全局默认节点（主要用于单节点场景）
- `allocation_strategy`：分配策略（LOCAL_FIRST/INTERLEAVE等）
- `node_distance_matrix`：节点间访问延迟矩阵
- `node_memory_info`：各节点内存使用情况

### 2. PREFIX结构设计

```c
// src/zmalloc.h:54-61
typedef struct {
    size_t size;           /* 8字节 - 用户请求的真实大小 */
    char from_pool;        /* 1字节 - 分配来源标记 */
    char node_id;          /* 1字节 - 分配时的NUMA节点 */
    char padding[6];       /* 6字节 - 16字节对齐填充 */
} numa_alloc_prefix_t;

#define PREFIX_SIZE (sizeof(numa_alloc_prefix_t))  // 固定16字节
```

**设计考量**：
1. **大小字段**：libNUMA不提供查询分配大小的API，必须自己记录
2. **来源标记**：区分Pool/Slab/直接分配，释放时采用不同策略
3. **节点ID**：P2关键修复，记录分配时的节点用于正确释放
4. **对齐填充**：16字节对齐优化CPU访问性能

### 3. 分配统计结构

```c
// src/zmalloc.c:92-102
static struct {
    size_t total_allocated;          /* 总分配字节数 */
    size_t total_freed;              /* 总释放字节数 */
    size_t current_used;             /* 当前使用量 */
    uint64_t alloc_count;            /* 分配次数 */
    uint64_t free_count;             /* 释放次数 */
    uint64_t pool_hits;              /* 内存池命中次数 */
    uint64_t direct_allocs;          /* 直接分配次数 */
    pthread_mutex_t stats_lock;      /* 统计保护锁 */
} zmalloc_stats = {0};
```

### 4. 策略配置结构

```c
// 可配置的NUMA策略参数
typedef struct {
    int strategy;                    /* 分配策略 */
    double memory_pressure_threshold; /* 内存压力阈值 */
    int cache_invalidation_period;   /* TLS缓存失效周期 */
    int enable_node_balancing;       /* 是否启用节点负载均衡 */
    size_t min_allocation_size;      /* 最小NUMA分配大小 */
} numa_config_t;

static numa_config_t current_config = {
    .strategy = NUMA_STRATEGY_LOCAL_FIRST,
    .memory_pressure_threshold = 0.8,
    .cache_invalidation_period = 1000,
    .enable_node_balancing = 1,
    .min_allocation_size = 64
};
```

---

## 核心算法实现

### 1. 初始化算法

```c
// src/zmalloc.c:110-150
void numa_init(void) {
    // 双重检查锁定模式
    if (__atomic_load_n(&numa_ctx.numa_available, __ATOMIC_ACQUIRE))
        return;
    
    pthread_mutex_lock(&numa_ctx.ctx_lock);
    if (numa_ctx.numa_available) {
        pthread_mutex_unlock(&numa_ctx.ctx_lock);
        return;
    }
    
    // 检查系统NUMA支持
    if (numa_available() == -1) {
        serverLog(LL_WARNING, "NUMA not available on this system");
        numa_ctx.numa_available = 0;
        pthread_mutex_unlock(&numa_ctx.ctx_lock);
        return;
    }
    
    // 初始化NUMA内存池
    if (numa_pool_init() != 0) {
        serverLog(LL_WARNING, "Failed to initialize NUMA memory pools");
        numa_ctx.numa_available = 0;
        pthread_mutex_unlock(&numa_ctx.ctx_lock);
        return;
    }
    
    // 获取系统信息
    numa_ctx.num_nodes = numa_num_configured_nodes();
    numa_ctx.current_node = numa_preferred();
    
    // 构建节点距离矩阵
    build_node_distance_matrix();
    
    // 初始化统计信息
    memset(&zmalloc_stats, 0, sizeof(zmalloc_stats));
    pthread_mutex_init(&zmalloc_stats.stats_lock, NULL);
    
    __atomic_store_n(&numa_ctx.numa_available, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&numa_ctx.ctx_lock);
    
    serverLog(LL_NOTICE, "NUMA allocator initialized with %d nodes", 
              numa_ctx.num_nodes);
}

static void build_node_distance_matrix(void) {
    int max_node = numa_max_node();
    numa_ctx.node_distance_matrix = malloc((max_node + 1) * (max_node + 1) * sizeof(int));
    
    for (int i = 0; i <= max_node; i++) {
        for (int j = 0; j <= max_node; j++) {
            int distance = numa_distance(i, j);
            numa_ctx.node_distance_matrix[i * (max_node + 1) + j] = distance;
        }
    }
}
```

### 2. 智能分配算法

```c
// src/zmalloc.c:252-289
static void *numa_alloc_with_size(size_t size) {
    ASSERT_NO_SIZE_OVERFLOW(size);
    
    // 计算包含PREFIX的总大小
    size_t total_size = size + PREFIX_SIZE;
    size_t alloc_size;
    
    // 选择目标节点
    int target_node = select_optimal_node(size);
    
    void *raw_ptr = NULL;
    int from_pool = 0;
    
    // P2优化：小对象优先走Slab路径
    if (should_use_slab(size)) {
        raw_ptr = numa_slab_alloc(size, target_node, &alloc_size);
        if (raw_ptr) {
            from_pool = 2;  // Slab分配
        }
    }
    
    // 回退到Pool路径
    if (!raw_ptr) {
        raw_ptr = numa_pool_alloc(total_size, target_node, &alloc_size);
        if (raw_ptr) {
            from_pool = 1;  // Pool分配
        }
    }
    
    // 最后回退到直接分配
    if (!raw_ptr) {
        raw_ptr = numa_alloc_onnode(total_size, target_node);
        if (raw_ptr) {
            alloc_size = total_size;
            from_pool = 0;  // 直接分配
        }
    }
    
    if (!raw_ptr) {
        return NULL;  // 分配失败
    }
    
    // 初始化PREFIX
    numa_init_prefix(raw_ptr, size, from_pool, target_node);
    
    // 更新统计信息
    update_allocation_stats(alloc_size, from_pool);
    
    // 返回用户指针（跳过PREFIX）
    return numa_to_user_ptr(raw_ptr);
}

static int select_optimal_node(size_t size) {
    int current_cpu_node = get_current_numa_node();
    
    // 检查当前节点是否有足够内存
    if (has_sufficient_memory(current_cpu_node, size)) {
        return current_cpu_node;
    }
    
    // 寻找最佳备选节点
    return find_best_alternative_node(current_cpu_node, size);
}
```

### 3. 内存释放算法

```c
// src/zmalloc.c:292-325
static void numa_free_with_size(void *user_ptr) {
    if (user_ptr == NULL) return;
    
    // 获取PREFIX信息
    numa_alloc_prefix_t *prefix = numa_get_prefix(user_ptr);
    size_t user_size = prefix->size;
    size_t total_size = user_size + PREFIX_SIZE;
    int from_pool = prefix->from_pool;
    int original_node = prefix->node_id;  // P2修复：使用原始节点
    
    // 更新统计信息
    update_free_stats(total_size);
    
    // 计算原始分配地址
    void *raw_ptr = (char *)user_ptr - PREFIX_SIZE;
    
    // 根据来源采用不同释放策略
    switch (from_pool) {
        case 0:  // 直接分配
            numa_free(raw_ptr, total_size);
            break;
            
        case 1:  // Pool分配
            numa_pool_free(raw_ptr, total_size, 1);
            break;
            
        case 2:  // Slab分配
            numa_slab_free(raw_ptr, total_size, original_node);
            break;
            
        default:
            serverLog(LL_WARNING, "Unknown allocation source: %d", from_pool);
            // 安全回退
            numa_free(raw_ptr, total_size);
    }
}
```

### 4. 统计更新算法

```c
// src/zmalloc.c:327-350
static void update_allocation_stats(size_t size, int from_pool) {
    pthread_mutex_lock(&zmalloc_stats.stats_lock);
    
    zmalloc_stats.total_allocated += size;
    zmalloc_stats.current_used += size;
    zmalloc_stats.alloc_count++;
    
    if (from_pool == 0) {
        zmalloc_stats.direct_allocs++;
    } else {
        zmalloc_stats.pool_hits++;
    }
    
    pthread_mutex_unlock(&zmalloc_stats.stats_lock);
}

static void update_free_stats(size_t size) {
    pthread_mutex_lock(&zmalloc_stats.stats_lock);
    
    zmalloc_stats.total_freed += size;
    zmalloc_stats.current_used -= size;
    zmalloc_stats.free_count++;
    
    pthread_mutex_unlock(&zmalloc_stats.stats_lock);
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
