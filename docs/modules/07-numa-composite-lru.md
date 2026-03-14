# 07-numa-composite-lru - NUMA复合LRU策略

**文件**: `src/numa_composite_lru.h`, `src/numa_composite_lru.c`  
**版本**: v2.7  
**状态**: 已实现 — 策略插槽框架1号默认策略，支持NUMACONFIG运行时配置

基于Redis原生LRU机制的NUMA迁移策略，通过访问感知统一递增和惰性阶梯衰减精确追踪热度，结合节点资源状况触发智能数据迁移。

---

## 架构

```
复合LRU策略（插槽1）
├── 热度管理
│   ├── 访问感知统一递增: 本地/远程访问均 +1
│   ├── 热度等级: 0-7
│   └── 惰性阶梯衰减: 下次访问时按空闲时长一次性结算
├── 资源监控
│   ├── 节点内存使用率
│   ├── 带宽利用率
│   └── 迁移压力评估
└── 迁移决策
    ├── 热点识别: hotness ≥ migrate_hotness_threshold
    ├── 资源可用 → 立即迁移
    └── 资源紧张 → 加入延迟队列

热度更新两条路径（均执行"先衰减后递增"）:
  PREFIX路径 (val != NULL): 热度内嵌于NUMA内存PREFIX头，零额外开销
  字典回退路径 (val == NULL): 热度存入 key_heat_map，兼容旧接口
```

---

## 核心数据结构

```c
/* 策略私有数据 */
typedef struct {
    uint32_t decay_threshold;
    uint8_t  stability_count;
    uint8_t  migrate_hotness_threshold;
    double   overload_threshold;
    double   bandwidth_threshold;
    double   pressure_threshold;
    uint64_t last_decay_time;
    dict    *key_heat_map;           /* 字典回退路径的热度映射 */
    list    *pending_migrations;     /* 待处理迁移队列 */
    uint64_t heat_updates;
    uint64_t migrations_triggered;
} composite_lru_data_t;

/* Key热度信息 */
typedef struct {
    uint8_t  hotness;                /* 0-7 */
    uint8_t  stability_counter;
    uint16_t last_access;            /* LRU_CLOCK低16位 */
    uint64_t access_count;
    int      current_node;
} heat_info_t;
```

**惰性衰减阶梯**（`compute_lazy_decay_steps`）:

| 空闲时长 | 衰减量 |
|---------|--------|
| < 10s   | 0（免疫）|
| < 60s   | 1 |
| < 5min  | 2 |
| < 30min | 3 |
| ≥ 30min | 全清为0 |

---

## 接口

```c
/* 策略生命周期（通过 vtable 调用）*/
int  composite_lru_init(numa_strategy_t *strategy);
int  composite_lru_execute(numa_strategy_t *strategy);
void composite_lru_cleanup(numa_strategy_t *strategy);

/* 热度更新 Hook（在 lookupKey 等访问点插入）*/
void composite_lru_record_access(numa_strategy_t *strategy,
                                 void *key, void *val);

/* NUMACONFIG 动态配置（通过 set_config vtable）*/
// decay_threshold     → 衰减间隔（秒）
// migrate_threshold   → 迁移热度门槛
// overload_threshold  → 节点过载阈值(0.0-1.0)
```

---

## 配置参数

```c
#define COMPOSITE_LRU_DEFAULT_DECAY_THRESHOLD    10000000  /* 10s (微秒) */
#define COMPOSITE_LRU_DEFAULT_STABILITY_COUNT    3
#define COMPOSITE_LRU_DEFAULT_MIGRATE_THRESHOLD  5

#define LAZY_DECAY_STEP1_SECS    10
#define LAZY_DECAY_STEP2_SECS    60
#define LAZY_DECAY_STEP3_SECS   300
#define LAZY_DECAY_STEP4_SECS  1800

#define COMPOSITE_LRU_DEFAULT_OVERLOAD_THRESHOLD    0.8
#define COMPOSITE_LRU_DEFAULT_BANDWIDTH_THRESHOLD   0.9
#define COMPOSITE_LRU_DEFAULT_PRESSURE_THRESHOLD    0.7
#define COMPOSITE_LRU_MAX_PENDING_MIGRATIONS        1000
```
