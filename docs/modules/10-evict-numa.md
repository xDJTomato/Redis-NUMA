# NUMA 淘汰池联动模块 (evict_numa)

## 模块概述

淘汰池联动模块将 Redis 原生淘汰机制与 NUMA 迁移策略深度集成，实现**多层缓存降级**：在内存超限时优先将冷数据迁移到其他 NUMA 节点而非直接淘汰，提升缓存命中率。

**核心价值**：
- 热点数据保留在本地，冷数据降级到远端
- 距离优先节点选择，减少远程访问延迟
- 所有 NUMA 节点不可用时才真正淘汰

---

## 架构设计

### 分层淘汰流程

```
┌─────────────────────────────────────────────────────────────────┐
│                    performEvictions() 入口                       │
└────────────────────────────┬────────────────────────────────────┘
                             │
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│  Phase 1: NUMA 降级迁移                                          │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  1. 从淘汰池获取冷数据候选者                                │    │
│  │  2. 距离优先选择最佳目标节点                                │    │
│  │  3. 执行 numa_migrate_single_key                          │    │
│  │  4. 释放本地节点内存空间                                    │    │
│  └─────────────────────────────────────────────────────────┘    │
└────────────────────────────┬────────────────────────────────────┘
                             │ 所有节点不可用
                             ▼
┌─────────────────────────────────────────────────────────────────┐
│  Phase 2: 真正淘汰                                               │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │  1. 从淘汰池获取最佳淘汰候选者                              │    │
│  │  2. 执行 dbSyncDelete / dbAsyncDelete                     │    │
│  │  3. 更新统计与通知                                         │    │
│  └─────────────────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────────────┘
```

### 核心数据结构

```c
/* 淘汰池扩展条目 (src/evict.h) */
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time */
    char *key;                  /* Key name (sds) */
    char *cached;               /* Cached SDS object */
    int dbid;                   /* Key DB number */
    
    /* === NUMA 联动扩展字段 === */
    int current_node;           /* 当前所在 NUMA 节点 */
    size_t object_size;         /* 对象大小 */
    uint8_t numa_migrated;      /* 已迁移次数 */
};

/* NUMA 降级配置 */
typedef struct {
    int enabled;                /* 启用开关 */
    size_t min_demote_size;     /* 最小降级对象大小 */
    int max_migrate_count;      /* 最大迁移次数 */
    int pressure_threshold;     /* 节点压力阈值 (百分比) */
    int distance_weight;        /* 距离权重 (默认 70) */
    int pressure_weight;        /* 压力权重 (默认 30) */
    int prefer_closer_node;     /* 优先更近节点 */
} numa_demote_config_t;
```

---

## 距离优先节点选择算法

### NUMA 距离矩阵

NUMA 架构中，不同节点间的访问延迟不同：

```
典型 2 插槽 + CXL 扩展架构:

         Node0 (DDR)  Node1 (DDR)  Node2 (CXL)
Node0       10          20           50
Node1       20          10           50
Node2       50          50           10

距离解释:
- 10: 本地访问 (最快)
- 20: 跨插槽访问 (中等延迟)
- 50: CXL 远端访问 (较高延迟)
```

### 三因子加权评分公式

```
评分 = 距离归一化 × distance_weight + 压力归一化 × pressure_weight + 带宽归一化 × bandwidth_weight

默认权重: 距离 40%, 压力 30%, 带宽 30%
评分越低越优先选择
```

**新增带宽因子说明**:
- **bandwidth_weight**: 带宽权重 (默认 30)
- 通过 `numa_bw_monitor` 模块实时监控各 NUMA 节点带宽利用率
- 带宽利用率越低（越空闲）的节点评分越低，越优先选择
- 三因子公式更全面地平衡了延迟、容量和带宽资源

### 算法实现

```c
int numaFindBestDemoteNode(size_t object_size, int current_node) {
    /* 1. 收集候选节点 */
    for (int i = 0; i < num_nodes; i++) {
        if (i == current_node) continue;
        
        double pressure = numaGetNodePressure(i);
        size_t free_mem = numaGetNodeFreeMemory(i);
        
        /* 过滤：压力超限或空间不足的节点 */
        if (pressure >= threshold) continue;
        if (free_mem < object_size * 2) continue;
        
        candidates[idx].distance = numa_distance(current_node, i);
        candidates[idx].pressure = pressure;
        /* 新增：获取节点带宽利用率 */
        candidates[idx].bandwidth = numa_bw_monitor_get_node_utilization(i);
    }
    
    /* 2. 计算综合评分 (三因子) */
    for (int i = 0; i < candidate_count; i++) {
        double dist_norm = distance / max_distance;
        double pres_norm = pressure / max_pressure;
        double bw_norm = bandwidth / max_bandwidth;
        
        score = dist_norm * 0.4 + pres_norm * 0.3 + bw_norm * 0.3;
    }
    
    /* 3. 返回最低评分节点 */
    return candidates[best_idx].node_id;
}
```

### 决策示例

**场景**: Node0 内存超限，需要降级迁移 5KB 对象

| 候选节点 | 距离 | 压力 | 带宽利用率 | 归一化距离 | 归一化压力 | 归一化带宽 | 评分 (40:30:30) | 排名 |
|---------|------|------|-----------|-----------|-----------|-----------|----------------|------|
| Node1   | 20   | 0.6  | 0.4       | 0.40      | 0.67      | 0.40      | 0.48           | **1** |
| Node2   | 50   | 0.3  | 0.8       | 1.00      | 0.33      | 0.80      | 0.76           | 2    |

**结论**: 选择 Node1（综合评分最低），距离更近且带宽更空闲。

---

## 配置参数

### redis.conf

```conf
############################## NUMA DEMOTION ################################

# 启用 NUMA 降级 (在淘汰前先迁移到其他节点)
numa-demote-enabled yes

# 最小降级对象大小 (小于此值的对象直接淘汰)
numa-demote-min-size 1kb

# 最大迁移次数 (超过此次数的对象直接淘汰)
numa-demote-max-migrate 3

# 节点压力阈值 (0-100), 超过此阈值的节点不作为降级目标
numa-demote-pressure-threshold 90

# 距离权重 (0-100, 三权重之和应为 100)
numa-demote-distance-weight 40

# 压力权重 (0-100, 三权重之和应为 100)
numa-demote-pressure-weight 30

# 带宽权重 (0-100, 三权重之和应为 100)
numa-demote-bandwidth-weight 30

# 是否优先选择更近节点
numa-demote-prefer-closer yes
```

### 运行时调整

```bash
redis-cli CONFIG SET numa-demote-distance-weight 40
redis-cli CONFIG SET numa-demote-pressure-weight 30
redis-cli CONFIG SET numa-demote-bandwidth-weight 30
```

### 配置建议

| 场景 | distance_weight | pressure_weight | bandwidth_weight | 说明 |
|------|-----------------|-----------------|------------------|------|
| 延迟敏感 | 70 | 15 | 15 | 优先选择近端节点 |
| 容量优先 | 20 | 50 | 30 | 优先选择空闲节点 |
| 带宽敏感 | 20 | 20 | 60 | 优先选择低带宽节点 |
| 平衡模式 | 40 | 30 | 30 | 三因子均衡 (推荐默认) |
| **推荐默认** | **40** | **30** | **30** | 兼顾延迟、容量与带宽 |

---

## 统计与监控

### INFO 命令输出

```
# Stats
...
evicted_keys:0
numa_demote_count:156        # NUMA降级迁移次数
numa_demote_bytes:1048576    # NUMA降级迁移字节数
numa_demote_failed:3         # NUMA降级失败次数
numa_demote_near:120         # 迁移到近端节点的次数
numa_demote_far:36           # 迁移到远端节点的次数
...
```

### 统计指标含义

| 指标 | 含义 |
|------|------|
| `numa_demote_count` | 成功降级迁移的总次数 |
| `numa_demote_bytes` | 降级迁移的总字节数 |
| `numa_demote_failed` | 迁移失败的次数 |
| `numa_demote_near` | 迁移到近距离节点 (distance ≤ 20) 的次数 |
| `numa_demote_far` | 迁移到远距离节点 (distance > 20) 的次数 |

---

## 源码结构

```
src/
├── evict.h              # 淘汰池扩展结构体与接口定义
├── evict_numa.c         # NUMA降级核心实现
│   ├── numaGetNodePressure()      # 节点压力查询
│   ├── numaGetNodeFreeMemory()    # 节点空闲内存查询
│   ├── numaFindBestDemoteNode()   # 距离优先节点选择
│   └── evictionTryNumaDemote()    # 降级执行入口
├── evict.c              # 淘汰主流程 (集成降级路径)
├── server.h             # 配置与统计字段定义
└── config.c             # 配置项解析
```

---

## 调用链分析

```
performEvictions()
    │
    ├── evictionPoolPopulate()        # 填充淘汰池
    │
    ├── 遍历淘汰池候选者
    │   │
    │   ├── evictionTryNumaDemote()   # 尝试NUMA降级
    │   │   │
    │   │   ├── objectComputeSize()   # 计算对象大小
    │   │   ├── numa_get_node_id()    # 获取当前节点
    │   │   │
    │   │   └── numaFindBestDemoteNode()  # 选择目标节点
    │   │       │
    │   │       ├── numaGetNodePressure()   # 查询压力
    │   │       ├── numaGetNodeFreeMemory() # 查询空闲内存
    │   │       ├── numa_distance()         # 查询距离
    │   │       └── 加权评分选择
    │   │
    │   ├── numa_migrate_single_key() # 执行迁移
    │   │
    │   └── 更新统计
    │
    └── [降级失败] → dbSyncDelete()   # 真正淘汰
```

---

## 性能考量

### 内存开销

- 淘汰池扩展字段：每个条目增加约 24 字节
- 节点压力缓存：16 节点 × 8 字节 = 128 字节
- 总体开销可忽略不计

### CPU 开销

- 节点压力查询：缓存 1 秒，避免频繁 sysfs 读取
- 距离查询：numa_distance() 是轻量级系统调用
- 评分计算：纯算术运算，纳秒级

### 迁移开销

- 迁移由 `numa_migrate_single_key()` 执行
- 异步化可能，但当前为同步执行
- 建议监控 `numa_demote_far` 比例，过高时调整权重

---

## 与其他模块的关系

```
┌─────────────────────────────────────────────────────────────┐
│                    evict_numa (本模块)                       │
│  淘汰池联动 + 距离优先选择                                    │
└──────────┬──────────────────────────────────┬──────────────┘
           │                                  │
           ▼                                  ▼
┌──────────────────────┐          ┌──────────────────────┐
│  numa_key_migrate    │          │  numa_composite_lru  │
│  (Key级迁移实现)      │          │  (热度追踪策略)       │
└──────────────────────┘          └──────────────────────┘
           │                                  │
           └─────────────┬────────────────────┘
                         ▼
              ┌──────────────────────┐
              │     numa_pool        │
              │   (内存池分配器)      │
              └──────────────────────┘
```

---

## 版本历史

| 版本 | 日期 | 改进内容 |
|------|------|----------|
| v3.4 | 2026-04-16 | 初始实现：淘汰池联动 + 距离优先选择 |
| v3.5 | 2026-04-16 | 升级三因子评分公式：新增带宽监控集成 (numa_bw_monitor)，权重调整为 40/30/30 |

---

## 参考资料

- [evict.c](../../src/evict.c) - Redis 原生淘汰机制
- [numa_key_migrate.md](./05-numa-key-migrate.md) - Key 迁移模块
- [numa_composite_lru.md](./07-numa-composite-lru.md) - 热度追踪策略
