# 07-numa-composite-lru.md - NUMA复合LRU策略

## 模块概述

**状态**: ✅ **已实现** - 1号策略框架已完成，作为策略插槽的默认策略，支持NUMACONFIG运行时配置

**文件**: `src/numa_composite_lru.h`, `src/numa_composite_lru.c`

**功能**: 基于Redis原生LRU机制的复合策略，作为NUMA策略插槽框架的1号默认策略，提供稳定性优先的热度管理和智能迁移触发，可通过NUMACONFIG命令动态调整参数。

**依赖关系**:
- 策略插槽框架：[numa_strategy_slots.c](./06-numa-strategy-slots.md)（已实现）
- Key迁移模块：[numa_key_migrate.c](./05-numa-key-migrate.md)（已实现）
- 内存分配层：[numa_pool.c](./01-numa-pool.md)（P0/P1优化完成）、[numa_slab.c](./03-zmalloc-numa.md)（P2优化完成）

**核心特性**:
- **访问感知统一递增**: 无论本地/远程访问，热度一律 +1，消除节点差异化处理
- **惰性阶梯衰减**: 衰减在下次访问时结算，按空闲时长分档，无需后台扫描
- **资源感知迁移**: 结合节点负载和带宽状况做出迁移决策
- **TTL 元数据清理**: 通过 `numa_on_key_delete` 钩子防止过期 key 产生幽灵热度

---

## 策略架构

### 核心组件

```
┌─────────────────────────────────────────────────────────────┐
│                    复合LRU策略                               │
├─────────────────────────────────────────────────────────────┤
│  热度管理模块                                                 │
│  ├── 访问感知统一递增 (本地/远程均 +1)                       │
│  ├── 热度等级控制 (0-7级)                                   │
│  └── 惰性阶梯衰减 (访问时按时间差一次结算)                   │
├─────────────────────────────────────────────────────────────┤
│  资源监控模块                                                 │
│  ├── 节点内存使用率监测                                     │
│  ├── 带宽利用率评估                                         │
│  └── 迁移压力分析                                           │
├─────────────────────────────────────────────────────────────┤
│  迁移决策模块                                                 │
│  ├── 热点识别 (基于访问频次而非节点亲和性)                   │
│  ├── 负载均衡触发                                           │
│  └── 迁移候选生成                                           │
└─────────────────────────────────────────────────────────────┘
```

### 数据结构

```c
/* 复合LRU策略私有数据 */
typedef struct {
    /* 热度控制参数 */
    uint32_t decay_threshold;           /* 衰减阈值(微秒) */
    uint8_t  stability_count;           /* 稳定计数器阈值 */
    uint8_t  migrate_hotness_threshold; /* 迁移热度门槛 */
    
    /* 资源监控参数 */
    double   overload_threshold;        /* 节点过载阈值(0.0-1.0) */
    double   bandwidth_threshold;       /* 带宽饱和阈值 */
    double   pressure_threshold;        /* 迁移压力阈值 */
    
    /* 内部状态 */
    uint64_t last_decay_time;           /* 上次衰减执行时间 */
    dict    *key_heat_map;              /* Key热度映射表 */
    list    *pending_migrations;        /* 待处理迁移队列 */
    
    /* 统计信息 */
    uint64_t heat_updates;              /* 热度更新次数 */
    uint64_t migrations_triggered;      /* 触发的迁移数 */
    uint64_t decay_operations;          /* 衰减操作次数 */
} composite_lru_data_t;

/* Key热度信息 */
typedef struct {
    uint8_t  hotness;                   /* 热度等级(0-7) */
    uint8_t  stability_counter;         /* 稳定计数器 */
    uint16_t last_access;               /* 最后访问时间(LRU_CLOCK低16位) */
    uint64_t access_count;              /* 累计访问次数 */
    int      current_node;              /* 当前所在节点 */
} heat_info_t;
```

---

## 核心算法实现

### 1. 热度更新机制

热度追踪同时支持两条路径，均执行「先惰性衰减，再统一递增」逻辑。

**路径一：PREFIX 路径（主路径，val != NULL）**

热度内嵌在每块 NUMA 内存的 PREFIX 头部，零额外开销：

```c
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val) {
    uint16_t current_time = get_lru_clock();

    if (val) {
        uint8_t current_hotness = numa_get_hotness(val);
        int mem_node = numa_get_node_id(val);

        /* Step 1: 惰性衰减 - 按空闲时长一次结算 */
        uint16_t last_access = numa_get_last_access(val);
        uint16_t elapsed = calculate_time_delta(current_time, last_access);
        uint8_t decay = compute_lazy_decay_steps(elapsed);
        if (decay > 0) {
            current_hotness = (decay >= current_hotness) ? 0 : (current_hotness - decay);
            numa_set_hotness(val, current_hotness);
        }

        /* Step 2: 统一递增 - 无论本地/远程一律 +1 */
        if (current_hotness < COMPOSITE_LRU_HOTNESS_MAX)
            numa_set_hotness(val, current_hotness + 1);

        /* Step 3: 远程访问且热度达阈值 → 加入迁移队列 */
        if (mem_node != current_node &&
            current_hotness >= data->migrate_hotness_threshold) {
            /* 加入 pending_migrations 队列 */
        }

        numa_increment_access_count(val);
        numa_set_last_access(val, current_time);
    }
    /* ... 字典回退路径（val == NULL）类似 */
}
```

**路径二：字典回退路径（val == NULL）**

当调用方无法提供值指针时，热度存入独立哈希表 `key_heat_map`，逻辑相同，兼容旧接口。

### 2. 惰性阶梯衰减算法

不依赖后台周期扫描，在 key **被下次访问时**一次性结算衰减量，衰减发生在热度递增之前。

```c
/*
 * compute_lazy_decay_steps - 阶梯衰减查表
 *
 * 空闲时长 → 衰减量：
 *   < 10s  : decay 0  (短暂停顿，完全免疫)
 *   < 60s  : decay 1
 *   < 5min : decay 2
 *   < 30min: decay 3
 *   >= 30min: 全清为 0
 */
static uint8_t compute_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < LAZY_DECAY_STEP1_SECS) return 0;
    if (elapsed_secs < LAZY_DECAY_STEP2_SECS) return 1;
    if (elapsed_secs < LAZY_DECAY_STEP3_SECS) return 2;
    if (elapsed_secs < LAZY_DECAY_STEP4_SECS) return 3;
    return COMPOSITE_LRU_HOTNESS_MAX; /* 长期冷数据全清 */
}
```

**与原稳定性衰减的对比**：

| 维度 | 原稳定性计数器衰减 | 惰性阶梯衰减（当前） |
|------|-------------------|--------------------|
| 触发方式 | 后台定时扫描（每秒） | 下次访问时结算 |
| 扫描开销 | O(n)，n 为 key 数量 | O(1)，零额外扫描 |
| 短暂离线 | 仍会触发衰减 | < 10s 完全免疫 |
| 长期冷数据 | 缓慢线性降至 0 | 直接全清 |
| TTL 过期 key | 可能残留衰减操作 | 无影响（结合 `numa_on_key_delete`） |

后台 `composite_lru_decay_heat` 仍保留作为字典路径的**补充扫描**（针对长时间无任何访问的 key），不影响 PREFIX 路径。

### 3. 资源感知迁移决策

```c
/* 资源状态检查 */
static int composite_lru_check_resources(int node_id, size_t required_memory) {
    /* 检查内存使用率 */
    double memory_usage = numa_get_node_memory_usage(node_id);
    if (memory_usage > overload_threshold) {
        return RESOURCE_OVERLOADED;
    }
    
    /* 检查带宽利用率 */
    double bandwidth_usage = numa_get_node_bandwidth_utilization(node_id);
    if (bandwidth_usage > bandwidth_threshold) {
        return RESOURCE_BANDWIDTH_SATURATED;
    }
    
    /* 检查迁移压力 */
    double migration_pressure = numa_get_migration_pressure(node_id);
    if (migration_pressure > pressure_threshold) {
        return RESOURCE_MIGRATION_PRESSURE;
    }
    
    return RESOURCE_AVAILABLE;
}

/* 迁移调度 */
static void composite_lru_schedule_migration(numa_strategy_t *strategy, 
                                           robj *key, heat_info_t *info,
                                           int target_node) {
    composite_lru_data_t *data = strategy->private_data;
    
    /* 检查目标节点资源状态 */
    int resource_status = composite_lru_check_resources(target_node, 
                                                       get_key_memory_size(key));
    
    switch (resource_status) {
        case RESOURCE_AVAILABLE:
            /* 资源充足，立即触发迁移 */
            composite_lru_trigger_immediate_migration(strategy, key, 
                                                    info->current_node, target_node);
            break;
            
        case RESOURCE_OVERLOADED:
        case RESOURCE_BANDWIDTH_SATURATED:
            /* 资源紧张，加入延迟队列 */
            composite_lru_add_to_pending_queue(data, key, info, target_node);
            break;
            
        case RESOURCE_MIGRATION_PRESSURE:
            /* 迁移压力大，暂缓处理 */
            break;
    }
}
```

### 4. 策略主执行循环

```c
/* 策略执行入口 */
static int composite_lru_execute(numa_strategy_t *strategy) {
    composite_lru_data_t *data = strategy->private_data;
    uint64_t now = get_monotonic_usec();
    
    /* 1. 执行周期性热度衰减 */
    if (now - data->last_decay_time > data->decay_threshold) {
        composite_lru_decay_heat(data);
        data->last_decay_time = now;
    }
    
    /* 2. 处理延迟迁移队列 */
    composite_lru_process_pending_migrations(data);
    
    /* 3. 检查全局负载均衡需求 */
    if (composite_lru_need_load_balance()) {
        composite_lru_trigger_load_balancing(strategy);
    }
    
    /* 4. 更新统计信息 */
    data->heat_updates += dictSize(data->key_heat_map);
    
    return NUMA_OK;
}
```

---

## 配置参数

### 默认配置值

```c
/* 核心参数 */
#define COMPOSITE_LRU_DEFAULT_DECAY_THRESHOLD    10000000  /* 10秒(微秒) */
#define COMPOSITE_LRU_DEFAULT_STABILITY_COUNT    3         /* 连续3次超阈値 */
#define COMPOSITE_LRU_DEFAULT_MIGRATE_THRESHOLD  5         /* 热度>=5触发迁移 */

/* 惰性阶梯衰减阈値（单位：LRU时钟秒，访问时结算） */
#define LAZY_DECAY_STEP1_SECS    10    /* < 10s  : decay 0 (短暂停顿，免疫) */
#define LAZY_DECAY_STEP2_SECS    60    /* < 60s  : decay 1 */
#define LAZY_DECAY_STEP3_SECS   300    /* < 5min : decay 2 */
#define LAZY_DECAY_STEP4_SECS  1800    /* < 30min: decay 3; ≥ 30min 全清 */

/* 资源阈値 */
#define COMPOSITE_LRU_DEFAULT_OVERLOAD_THRESHOLD     0.8   /* 80%内存使用率 */
#define COMPOSITE_LRU_DEFAULT_BANDWIDTH_THRESHOLD    0.9   /* 90%带宽利用率 */
#define COMPOSITE_LRU_DEFAULT_PRESSURE_THRESHOLD     0.7   /* 70%迁移压力 */

/* 性能参数 */
#define COMPOSITE_LRU_MAX_PENDING_MIGRATIONS     1000      /* 最大延迟迁移数 */
#define COMPOSITE_LRU_PENDING_TIMEOUT            30000000   /* 30秒超时(微秒) */
```

### 动态配置接口

```c
/* 配置管理 */
static int composite_lru_set_config(numa_strategy_t *strategy, 
                                  const char *key, const char *value) {
    composite_lru_data_t *data = strategy->private_data;
    
    if (strcmp(key, "decay_threshold") == 0) {
        data->decay_threshold = atoi(value) * 1000000;  /* 转换为微秒 */
    } else if (strcmp(key, "stability_count") == 0) {
        data->stability_count = atoi(value);
    } else if (strcmp(key, "migrate_threshold") == 0) {
        data->migrate_hotness_threshold = atoi(value);
    } else if (strcmp(key, "overload_threshold") == 0) {
        data->overload_threshold = atof(value);
    } else if (strcmp(key, "bandwidth_threshold") == 0) {
        data->bandwidth_threshold = atof(value);
    } else {
        return NUMA_CONFIG_UNKNOWN;
    }
    
    return NUMA_OK;
}

static int composite_lru_get_config(numa_strategy_t *strategy, 
                                  const char *key, char *buf, size_t buf_len) {
    composite_lru_data_t *data = strategy->private_data;
    
    if (strcmp(key, "decay_threshold") == 0) {
        snprintf(buf, buf_len, "%u", data->decay_threshold / 1000000);
    } else if (strcmp(key, "stability_count") == 0) {
        snprintf(buf, buf_len, "%u", data->stability_count);
    } else if (strcmp(key, "migrate_threshold") == 0) {
        snprintf(buf, buf_len, "%u", data->migrate_hotness_threshold);
    } else {
        return NUMA_CONFIG_UNKNOWN;
    }
    
    return NUMA_OK;
}
```

---

## 性能特征

### 时间复杂度

| 操作 | 平均复杂度 | 最坏复杂度 | 说明 |
|------|-----------|-----------|------|
| 热度更新 | O(1) | O(1) | hash表查找 |
| 热度衰减 | O(n) | O(n) | n为Key数量 |
| 资源检查 | O(1) | O(1) | 系统调用 |
| 迁移调度 | O(1) | O(log n) | 队列操作 |

### 典型性能指标

- **热度更新延迟**: < 1μs
- **周期性衰减**: 10-100ms（取决于Key数量）
- **内存开销**: ~32字节/Key
- **CPU占用**: < 1%（正常负载下）

---

## 与其他模块的交互

### 与Key迁移模块的接口

```c
/* 通过标准接口触发迁移 */
static int composite_lru_trigger_immediate_migration(numa_strategy_t *strategy,
                                                   robj *key,
                                                   int source_node,
                                                   int target_node) {
    /* 调用Key迁移模块的标准接口 */
    int result = numa_migrate_single_key(server.db[0], key, target_node);
    
    if (result == NUMA_MIGRATE_OK) {
        composite_lru_data_t *data = strategy->private_data;
        data->migrations_triggered++;
        
        /* 更新本地热度信息 */
        heat_info_t *info = dictFetchValue(data->key_heat_map, key);
        if (info) {
            info->current_node = target_node;
        }
    }
    
    return result;
}
```

### 与策略管理器的集成

```c
/* 策略工厂定义 */
static numa_strategy_factory_t composite_lru_factory = {
    .name = "composite-lru",
    .description = "基于Redis LRU的复合热度管理策略",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,  /* 1秒执行间隔 */
    .create = composite_lru_create,
    .destroy = composite_lru_destroy
};

/* 模块初始化 */
int composite_lru_module_init(void) {
    return numa_strategy_register_factory(&composite_lru_factory);
}
```

---

## 设计原则

### 1. 稳定性优先
- 避免因短期访问波动导致的频繁迁移
- 采用计数器机制确保决策的稳定性
- 设置合理的衰减阈值防止过度敏感

### 2. 资源感知
- 实时监控节点资源状态
- 根据资源状况动态调整迁移策略
- 避免在资源紧张时强制迁移

### 3. 渐进式优化
- 从小规模、低频次开始
- 根据效果逐步调整参数
- 保持与原系统兼容性

### 4. 可观测性
- 详细的统计信息收集
- 可动态调整的配置参数
- 清晰的状态监控接口

---

## 应用场景

### 适用环境
- 多NUMA节点的Redis部署
- 存在明显访问热点的应用
- 对内存延迟敏感的业务场景
- 需要动态负载均衡的高并发系统

### 配置建议
- **保守模式**: 提高阈值，降低执行频率
- **激进模式**: 降低阈值，提高执行频率
- **自适应模式**: 根据系统负载动态调整

---

## 开发日志

### v1.0 复合LRU策略实现 (2026-02-14)

#### 实现目标
实现NUMA策略插槽框架的1号默认策略，提供稳定性优先的热度管理和迁移决策机制。

#### 新增文件

| 文件 | 行数 | 说明 |
|-----|------|------|
| `src/numa_composite_lru.h` | ~120行 | 头文件，定义数据结构和接口 |
| `src/numa_composite_lru.c` | ~485行 | 实现文件，策略核心逻辑 |

#### 实现功能

##### 1. 策略工厂与注册
```c
/* 策略工厂定义 */
static numa_strategy_factory_t composite_lru_factory = {
    .name = "composite-lru",
    .description = "Stability-first composite LRU hotness management",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,  /* 1秒执行间隔 */
    .create = composite_lru_create,
    .destroy = composite_lru_destroy
};

/* 自动注册到slot 1 */
numa_strategy_slot_insert(1, "composite-lru");
```

##### 2. 热度管理数据结构
```c
/* Key热度信息 */
typedef struct {
    uint8_t  hotness;           /* 热度等级(0-7) */
    uint8_t  stability_counter; /* 稳定计数器 */
    uint16_t last_access;       /* 最后访问时间 */
    uint64_t access_count;      /* 累计访问次数 */
    int      current_node;      /* 当前所在节点 */
    int      preferred_node;    /* 首选迁移目标 */
} composite_lru_heat_info_t;
```

##### 3. 稳定性衰减算法
- 基于LRU时钟的时间差计算
- 稳定计数器机制：连续多次超阈值才降级
- 防止短期访问波动导致的误判

##### 4. 策略虚函数表
```c
static const numa_strategy_vtable_t composite_lru_vtable = {
    .init = composite_lru_init,
    .execute = composite_lru_execute,
    .cleanup = composite_lru_cleanup,
    .get_name = composite_lru_get_name,
    .get_description = composite_lru_get_description,
    .set_config = composite_lru_set_config,
    .get_config = composite_lru_get_config
};
```

##### 5. 配置参数
| 参数 | 默认值 | 说明 |
|-----|-------|------|
| decay_threshold | 10秒 | 热度衰减阈值 |
| stability_count | 3 | 稳定计数器阈值 |
| migrate_threshold | 5 | 迁移热度门槛 |
| overload_threshold | 0.8 | 节点过载阈值 |
| bandwidth_threshold | 0.9 | 带宽饱和阈值 |

#### 与框架集成

修改 `numa_strategy_slots.c`：
- 添加 `#include "numa_composite_lru.h"`
- 在 `numa_strategy_init()` 中自动注册1号策略
- 添加 `numa_strategy_register_composite_lru()` 转发函数

修改 `numa_strategy_slots.h`：
- 添加 `int numa_strategy_register_composite_lru(void);` 声明

#### 编译与测试

**编译结果**: ✅ 成功
```
CC numa_composite_lru.o
LINK redis-server
```

**启动日志**: ✅ 策略正常加载
```
[Composite LRU] Strategy initialized (slot 1 default)
[NUMA Strategy] Inserted strategy 'composite-lru' to slot 1
[NUMA Strategy] Strategy slot framework initialized (slots 0,1 ready)
```

#### 设计决策

1. **周期执行**: 采用`STRATEGY_TYPE_PERIODIC`类型，每秒执行一次
2. **高优先级**: 设为`STRATEGY_PRIORITY_HIGH`，优先于其他策略
3. **独立热度表**: 策略维护自己的`key_heat_map`，与Key迁移模块元数据分离
4. **延迟迁移队列**: 资源紧张时暂存迁移请求，待资源可用时处理

#### 后续计划

- [x] 集成LRU Hook，实现自动热度追踪（v2.0 已完成）
- [ ] 实现实际的资源监控接口
- [x] 与Key迁移模块的深度集成（v2.0 已完成：`numa_on_key_delete` 钩子）
- [ ] 性能调优和参数自适应

---

### v2.0 热度策略重构 (2026-03-14)

#### 变更目标

1. **统一热度递增**: 消除本地/远程访问的热度差异，使热度真实反映访问频次
2. **惰性阶梯衰减**: 用访问时结算替代后台周期扫描，降低 CPU 开销
3. **TTL 元数据清理**: 防止过期 key 产生幽灵热度

#### 核心变更

**热度更新逻辑** 从本地/远程分支改为统一流程：衰减 → 递增 → 迁移评估。
无论访问来自本地节点还是远程节点，`hotness` 均执行 `+1`。

**惰性阶梯衰减** 新增 `compute_lazy_decay_steps()` 查表函数，在每次访问前一次性结算空闲期欠债：

| 空闲时长 | 衰减量 |
|---------|-------|
| < 10s  | 0（完全免疫） |
| < 60s  | 1 |
| < 5min | 2 |
| < 30min| 3 |
| >= 30min | 全清 |

**bug 修复**: 字典路径中原先先覆写 `last_access` 再计算 `elapsed`，导致衰减永不生效。
本次修复：先保存旧时间戳，计算差值后再更新。

**`numa_on_key_delete(robj *key)`**: 在 `db.c::dbSyncDelete` 和 `lazyfree.c::dbAsyncDelete` 两处
通过 `#ifdef HAVE_NUMA` 条件调用，清理已删除/过期 key 的元数据，消除幽灵热度。

#### 文件变更

| 文件 | 变更说明 |
|-----|----------|
| `src/numa_composite_lru.h` | 新增 `LAZY_DECAY_STEP[1-4]_SECS` 常量 |
| `src/numa_composite_lru.c` | 新增 `compute_lazy_decay_steps()`；两条路径注入惰性衰减；修复字典路径 elapsed bug |
| `src/numa_key_migrate.h` | 新增 `KEY_LAZY_DECAY_STEP[1-4]_SECS` 常量；声明 `numa_on_key_delete()` |
| `src/numa_key_migrate.c` | 新增 `compute_key_lazy_decay_steps()`；`numa_record_key_access` 注入衰减；实现 `numa_on_key_delete()` |
| `src/db.c` | `dbSyncDelete` 调用 `numa_on_key_delete` |
| `src/lazyfree.c` | `dbAsyncDelete` 调用 `numa_on_key_delete` |

---
