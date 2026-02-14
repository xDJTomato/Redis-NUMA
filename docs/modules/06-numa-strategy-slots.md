# 06-numa-strategy-slots.md - NUMA策略插槽模块

## 模块概述

**状态**: ✅ **已实现** - 插槽框架、0号兜底策略和1号默认策略已完成，支持NUMACONFIG动态配置策略

**文件**: [src/numa_strategy_slots.h](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_strategy_slots.h), [src/numa_strategy_slots.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_strategy_slots.c), [src/numa_composite_lru.h](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_composite_lru.h), [src/numa_composite_lru.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_composite_lru.c)

**功能**: 提供插槽化策略管理框架，支持动态加载、配置和组合多种NUMA负载策略，与NUMACONFIG命令接口深度集成。

**核心思想**:
- 策略模块化：每个策略独立实现，通过统一接口接入
- 插槽机制：策略按插槽编号组织，支持动态插拔
- 组合使用：多个策略可同时生效，按优先级执行
- 已实现：0号no-op兜底策略和1号composite-lru默认策略

---

## 插槽架构设计

### 核心概念

```
┌─────────────────────────────────────────────────────────────┐
│                    策略插槽管理器                            │
│  numa_strategy_manager                                      │
├─────────────────────────────────────────────────────────────┤
│  插槽0: [No-op兜底策略]   ← ✅ 已实现，默认启用             │
│  插槽1: [复合LRU策略]     ← ✅ 已实现，作为默认策略        │
│  插槽2: [用户策略A]       ← 可选                              │
│  插槽3: [用户策略B]       ← 可选                              │
│  插槽4: [用户策略C]       ← 可选                              │
│  ...                                                        │
│  插槽N: [自定义策略]      ← 动态加载                          │
└─────────────────────────────────────────────────────────────┘
```

**插槽编号说明**：
- **插槽0**：对外暴露的no-op兜底策略，默认启用，用于验证框架可用性
- **插槽1**：复合LRU默认策略，已实现，提供稳定性优先的热度管理
- **插槽2+**：用户可自定义策略插槽

### 策略接口定义

```c
/* 策略插槽接口 */
typedef struct numa_strategy_s numa_strategy_t;

typedef enum {
    STRATEGY_TYPE_PERIODIC = 1,    /* 定期执行策略 */
    STRATEGY_TYPE_EVENT_DRIVEN,    /* 事件驱动策略 */
    STRATEGY_TYPE_HYBRID           /* 混合策略 */
} numa_strategy_type_t;

typedef enum {
    STRATEGY_PRIORITY_LOW = 1,     /* 低优先级 */
    STRATEGY_PRIORITY_NORMAL,      /* 正常优先级 */
    STRATEGY_PRIORITY_HIGH         /* 高优先级 */
} numa_strategy_priority_t;

/* 策略接口函数表 */
typedef struct {
    /* 初始化策略 */
    int (*init)(numa_strategy_t *strategy);
    
    /* 执行策略逻辑 */
    int (*execute)(numa_strategy_t *strategy);
    
    /* 清理资源 */
    void (*cleanup)(numa_strategy_t *strategy);
    
    /* 获取策略信息 */
    const char* (*get_name)(numa_strategy_t *strategy);
    const char* (*get_description)(numa_strategy_t *strategy);
    
    /* 配置管理 */
    int (*set_config)(numa_strategy_t *strategy, const char *key, const char *value);
    int (*get_config)(numa_strategy_t *strategy, const char *key, char *buf, size_t buf_len);
} numa_strategy_vtable_t;

/* 策略基类 */
struct numa_strategy_s {
    /* 基本信息 */
    int slot_id;                           /* 插槽ID */
    const char *name;                      /* 策略名称 */
    const char *description;               /* 策略描述 */
    
    /* 执行控制 */
    numa_strategy_type_t type;             /* 策略类型 */
    numa_strategy_priority_t priority;     /* 执行优先级 */
    int enabled;                           /* 是否启用 */
    uint64_t execute_interval_us;          /* 执行间隔（微秒） */
    uint64_t last_execute_time;            /* 上次执行时间 */
    
    /* 虚函数表 */
    const numa_strategy_vtable_t *vtable;
    
    /* 私有数据 */
    void *private_data;                    /* 策略私有数据 */
    
    /* 统计信息 */
    uint64_t total_executions;             /* 总执行次数 */
    uint64_t total_failures;               /* 失败次数 */
    uint64_t total_execution_time_us;      /* 总执行时间（微秒） */
};

/* 策略管理器 */
typedef struct {
    int initialized;                                  /* 初始化标志 */
    numa_strategy_t *slots[NUMA_MAX_STRATEGY_SLOTS];  /* 策略插槽 */
    pthread_mutex_t lock;                             /* 线程安全锁 */
    
    /* 工厂注册表 */
    numa_strategy_factory_t *factories[NUMA_MAX_STRATEGY_SLOTS];
    int factory_count;                                /* 已注册工厂数 */
    
    /* 统计信息 */
    uint64_t total_runs;                              /* 总调度次数 */
    uint64_t total_strategy_executions;               /* 总策略执行次数 */
} numa_strategy_manager_t;
```

---

## 0号兜底策略：No-op策略

0号插槽是对外暴露的no-op（无操作）策略，主要用于：
1. **框架验证**：验证策略插槽框架的基本可用性
2. **降级保障**：当其他策略未就绪或禁用时提供兜底
3. **测试基线**：为性能测试提供无开销的基线对比

### 策略实现

```c
/* 0号策略私有数据 */
typedef struct {
    uint64_t execution_count;      /* 执行计数 */
    uint64_t last_log_time;        /* 上次日志时间 */
} noop_strategy_data_t;

/* 0号策略执行函数 */
static int noop_strategy_execute(numa_strategy_t *strategy) {
    noop_strategy_data_t *data = strategy->private_data;
    uint64_t now = get_current_time_us();
    
    data->execution_count++;
    
    /* 每10秒打印一次日志，避免日志过多 */
    if (now - data->last_log_time > 10000000) {
        STRATEGY_LOG(LL_VERBOSE, 
                  "[NUMA Strategy Slot 0] No-op strategy executed (count: %llu)",
                  (unsigned long long)data->execution_count);
        data->last_log_time = now;
    }
    
    return NUMA_STRATEGY_OK;
}
```

### 执行特性

- **优先级**：`STRATEGY_PRIORITY_LOW`
- **执行间隔**：1000ms（通过serverCron调度）
- **开销**：极小，仅维护计数器
- **日志输出**：每10秒输出一次执行计数

---

## 1号默认策略：复合LRU策略（已实现）

✅ **当前状态**：已实现并自动加载到插槽1。

复合LRU策略是系统内置的默认策略，详细实现请参见独立文档 [07-numa-composite-lru.md](./07-numa-composite-lru.md)。

### 策略特性

1. **基础LRU更新**：维持Redis原有LRU淘汰机制
2. **热度追踪**：基于LRU时钟进行热度等级管理
3. **迁移触发**：根据热度和访问模式触发数据迁移
4. **稳定性控制**：避免频繁的升降级操作

### 核心配置

```c
#define COMPOSITE_LRU_DEFAULT_DECAY_THRESHOLD    10000000  /* 10秒 */
#define COMPOSITE_LRU_DEFAULT_STABILITY_COUNT    3         /* 连续3次 */
#define COMPOSITE_LRU_DEFAULT_MIGRATE_THRESHOLD  5         /* 热度阈值 */
#define COMPOSITE_LRU_DEFAULT_OVERLOAD_THRESHOLD 0.8       /* 80%负载 */
```

### 启动日志示例

```
[Composite LRU] Strategy initialized (slot 1 default)
[NUMA Strategy] Inserted strategy 'composite-lru' to slot 1
[NUMA Strategy] Strategy slot framework initialized (slots 0,1 ready)
```

---

## 插槽管理接口

### 核心API

```c
/* 策略注册与管理 */
int numa_strategy_register_factory(const numa_strategy_factory_t *factory);
numa_strategy_t* numa_strategy_create(const char *name);
void numa_strategy_destroy(numa_strategy_t *strategy);

/* 插槽操作 */
int numa_strategy_slot_insert(int slot_id, const char *strategy_name);
int numa_strategy_slot_remove(int slot_id);
int numa_strategy_slot_enable(int slot_id);
int numa_strategy_slot_disable(int slot_id);
int numa_strategy_slot_configure(int slot_id, const char *config_str);

/* 查询接口 */
numa_strategy_t* numa_strategy_slot_get(int slot_id);
int numa_strategy_slot_list(char *buf, size_t buf_len);
int numa_strategy_slot_status(int slot_id, char *buf, size_t buf_len);
```

### Redis命令接口

```c
/*
NUMA.SLOT.ADD <slot_id> <strategy_name> [interval_ms] [priority]
NUMA.SLOT.REMOVE <slot_id>
NUMA.SLOT.ENABLE <slot_id>
NUMA.SLOT.DISABLE <slot_id>
NUMA.SLOT.CONFIG <slot_id> <key> <value>
NUMA.SLOT.LIST
NUMA.SLOT.STATUS <slot_id>
NUMA.SLOT.PRESET <preset_name>
*/

/* 示例用法 */
// 添加水位线策略到插槽2
NUMA.SLOT.ADD 2 watermark interval=2000 priority=normal

// 配置插槽2的参数
NUMA.SLOT.CONFIG 2 high_watermark 0.8
NUMA.SLOT.CONFIG 2 low_watermark 0.3

// 禁用插槽2
NUMA.SLOT.DISABLE 2

// 查看所有插槽状态
NUMA.SLOT.LIST
```

---

## 配置文件支持

```conf
# NUMA策略插槽配置
# 插槽1：复合LRU策略（默认，不可移除）
numa-slot-1 composite-lru interval=1000 priority=high

# 插槽2：水位线策略
numa-slot-2 watermark interval=2000 priority=normal
numa-watermark-high 0.8
numa-watermark-low 0.3

# 插槽3：带宽感知策略
numa-slot-3 bandwidth interval=5000 priority=low
numa-bandwidth-threshold 0.9

# 自适应调节
numa-auto-scaling yes
numa-base-interval 1000000
```

---

## 执行调度机制

### 调度器设计

```c
/* 策略执行器线程 */
static void* numa_strategy_executor_thread(void *arg) {
    while (strategy_manager.running) {
        uint64_t now = get_monotonic_usec();
        
        /* 按优先级顺序执行各插槽 */
        for (int priority = STRATEGY_PRIORITY_HIGH; 
             priority >= STRATEGY_PRIORITY_LOW; 
             priority--) {
            
            for (int slot = 0; slot < NUMA_MAX_STRATEGY_SLOTS; slot++) {
                numa_strategy_t *strategy = strategy_manager.slots[slot];
                
                if (strategy && 
                    strategy->enabled && 
                    strategy->priority == priority &&
                    now - strategy->last_execute_time >= strategy->execute_interval_us) {
                    
                    /* 执行策略 */
                    uint64_t start_time = get_monotonic_usec();
                    int result = strategy->vtable->execute(strategy);
                    uint64_t exec_time = get_monotonic_usec() - start_time;
                    
                    /* 更新统计 */
                    strategy->last_execute_time = now;
                    strategy->execute_count++;
                    strategy->total_execution_time_us += exec_time;
                    
                    if (result == NUMA_OK) {
                        strategy->success_count++;
                    } else {
                        strategy->fail_count++;
                    }
                }
            }
        }
        
        usleep(100000);  /* 100ms轮询间隔 */
    }
    
    return NULL;
}
```

### 自适应调节

```c
/* 根据系统状态自动调节策略参数 */
static void numa_strategy_auto_tune(void) {
    if (!strategy_manager.auto_scaling_enabled) return;
    
    double system_load = get_system_load_average();
    uint64_t migration_rate = get_recent_migration_rate();
    
    /* 负载高时降低执行频率 */
    if (system_load > 0.8) {
        for (int slot = 0; slot < NUMA_MAX_STRATEGY_SLOTS; slot++) {
            numa_strategy_t *strategy = strategy_manager.slots[slot];
            if (strategy && strategy->slot_id != 1) {  /* 保留插槽1 */
                strategy->execute_interval_us *= 1.5;  /* 降低50%执行频率 */
            }
        }
    }
    /* 负载低时提高执行频率 */
    else if (system_load < 0.3 && migration_rate < 10) {
        for (int slot = 0; slot < NUMA_MAX_STRATEGY_SLOTS; slot++) {
            numa_strategy_t *strategy = strategy_manager.slots[slot];
            if (strategy && strategy->slot_id != 1) {
                strategy->execute_interval_us *= 0.8;  /* 提高25%执行频率 */
            }
        }
    }
}
```

---

## 策略工厂通俗解释

### 什么是策略工厂？

**简单理解**：策略工厂就像一个"策略制造机"，告诉框架"如何生产"一个策略。框架通过这个工厂来创建、销毁策略实例。

**类比**：就像汽车工厂，不是直接把汽车给用户，而是告诉用户"来我这里可以买到汽车"。框架也是通过工厂来"订购"策略。

### 工厂的核心组成

```c
/* 策略工厂 = 策略的"生产说明书" */
typedef struct {
    const char *name;                    /* 策略名字（如"composite-lru"） */
    const char *description;             /* 策略简介 */
    numa_strategy_type_t type;           /* 策略类型（定期执行/事件驱动） */
    numa_strategy_priority_t default_priority;  /* 优先级（高/中/低） */
    uint64_t default_interval_us;        /* 默认执行间隔（微秒） */
    
    /* 两个关键函数：创建和销毁 */
    numa_strategy_t* (*create)(void);    /* 制造策略 */
    void (*destroy)(numa_strategy_t *strategy);  /* 销毁策略 */
} numa_strategy_factory_t;
```

### 为什么要用工厂模式？

**问题**：如果直接创建策略，框架需要知道每个策略的具体细节（需要多少内存、如何初始化等）。

**解决**：工厂模式让框架只关心"接口"，不关心"实现"。就像你去餐厅点菜，只需要说"我要一份炒饭"，不需要知道厨师怎么做。

```
┌─────────────────────────────────────────┐
│           策略插槽框架                   │
│  "我要一个composite-lru策略"            │
└──────────────┬──────────────────────────┘
               │ 调用 create()
               ▼
┌─────────────────────────────────────────┐
│      composite-lru 策略工厂             │
│  1. 分配内存                            │
│  2. 设置参数（10秒衰减、热度阈值5等）    │
│  3. 创建热度表                          │
│  4. 返回策略实例                        │
└─────────────────────────────────────────┘
```

### 完整实现示例

以1号策略（复合LRU）为例，看看工厂如何工作：

**第一步：定义策略的"身体"（数据结构）**

```c
/* 策略实例 = 策略的"身体" */
struct numa_strategy_s {
    int slot_id;                    /* 住在哪个插槽（如1号） */
    const char *name;               /* 名字 */
    const char *description;        /* 简介 */
    
    numa_strategy_type_t type;      /* 类型：定期执行 */
    numa_strategy_priority_t priority;  /* 优先级：高 */
    int enabled;                    /* 是否启用 */
    uint64_t execute_interval_us;   /* 多久执行一次（1秒） */
    
    const numa_strategy_vtable_t *vtable;  /* 策略的"技能表" */
    void *private_data;             /* 策略的"私有物品" */
};

/* 1号策略的私有数据 */
typedef struct {
    uint32_t decay_threshold;       /* 热度衰减时间（10秒） */
    uint8_t  stability_count;       /* 稳定计数器（3次） */
    dict    *key_heat_map;          /* 每个key的热度表 */
    list    *pending_migrations;    /* 待迁移队列 */
    uint64_t heat_updates;          /* 统计：热度更新次数 */
} composite_lru_data_t;
```

**第二步：定义策略的"技能"（虚函数表）**

```c
/* 虚函数表 = 策略的"技能清单" */
static const numa_strategy_vtable_t composite_lru_vtable = {
    .init = composite_lru_init,           /* 初始化技能 */
    .execute = composite_lru_execute,     /* 执行技能 */
    .cleanup = composite_lru_cleanup,     /* 清理技能 */
    .get_name = composite_lru_get_name,   /* 报名字 */
    .get_description = composite_lru_get_description,  /* 报简介 */
    .set_config = composite_lru_set_config,  /* 改配置 */
    .get_config = composite_lru_get_config   /* 查配置 */
};
```

**第三步：实现"制造"函数（create）**

```c
/* 制造一个1号策略实例 */
static numa_strategy_t* composite_lru_create(void) {
    /* 1. 分配策略主体内存 */
    numa_strategy_t *strategy = zmalloc(sizeof(*strategy));
    if (!strategy) return NULL;
    
    memset(strategy, 0, sizeof(*strategy));
    
    /* 2. 填写基本信息 */
    strategy->slot_id = 1;  /* 默认住1号插槽 */
    strategy->name = "composite-lru";
    strategy->description = "Stability-first composite LRU strategy";
    strategy->type = STRATEGY_TYPE_PERIODIC;  /* 定期执行 */
    strategy->priority = STRATEGY_PRIORITY_HIGH;  /* 高优先级 */
    strategy->enabled = 1;  /* 默认启用 */
    strategy->execute_interval_us = 1000000;  /* 1秒执行一次 */
    strategy->vtable = &composite_lru_vtable;  /* 绑定技能表 */
    
    /* 3. 创建私有数据（策略的"个人物品"） */
    composite_lru_data_t *data = zmalloc(sizeof(*data));
    if (!data) {
        zfree(strategy);
        return NULL;
    }
    
    /* 4. 初始化私有数据 */
    data->decay_threshold = 10000000;  /* 10秒 */
    data->stability_count = 3;         /* 3次 */
    data->migrate_hotness_threshold = 5;  /* 热度>=5触发迁移 */
    data->key_heat_map = dictCreate(&heat_map_dict_type, NULL);
    data->pending_migrations = listCreate();
    
    strategy->private_data = data;
    
    return strategy;
}
```

**第四步：实现"销毁"函数（destroy）**

```c
/* 销毁一个1号策略实例 */
static void composite_lru_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    
    /* 1. 调用清理函数（如果有） */
    if (strategy->vtable && strategy->vtable->cleanup) {
        strategy->vtable->cleanup(strategy);
    }
    
    /* 2. 释放私有数据 */
    if (strategy->private_data) {
        composite_lru_data_t *data = strategy->private_data;
        
        /* 释放热度表 */
        if (data->key_heat_map) {
            dictRelease(data->key_heat_map);
        }
        
        /* 释放待迁移队列 */
        if (data->pending_migrations) {
            listRelease(data->pending_migrations);
        }
        
        zfree(data);
    }
    
    /* 3. 释放策略主体 */
    zfree(strategy);
}
```

**第五步：组装工厂并注册**

```c
/* 定义工厂 */
static numa_strategy_factory_t composite_lru_factory = {
    .name = "composite-lru",
    .description = "Stability-first composite LRU strategy",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,  /* 1秒 */
    .create = composite_lru_create,
    .destroy = composite_lru_destroy
};

/* 注册工厂到框架 */
int numa_composite_lru_register(void) {
    return numa_strategy_register_factory(&composite_lru_factory);
}
```

### 框架如何使用工厂？

```
┌─────────────────────────────────────────────────────┐
│  框架初始化流程                                       │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│  1. 注册工厂                                         │
│     numa_strategy_register_factory(&factory)        │
│                                                     │
│     框架记住："composite-lru" → create/destroy     │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│  2. 创建策略实例                                     │
│     numa_strategy_slot_insert(1, "composite-lru")  │
│                                                     │
│     框架调用：factory->create()                     │
│     得到一个完整的策略实例                          │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│  3. 执行策略                                         │
│     numa_strategy_run_slot(1)                       │
│                                                     │
│     框架调用：strategy->vtable->execute(strategy)   │
│     策略执行自己的逻辑                              │
└─────────────────────────────────────────────────────┘
                         │
                         ▼
┌─────────────────────────────────────────────────────┐
│  4. 销毁策略                                         │
│     numa_strategy_slot_remove(1)                    │
│                                                     │
│     框架调用：factory->destroy(strategy)            │
│     清理所有资源                                    │
└─────────────────────────────────────────────────────┘
```

### 关键要点总结

| 概念 | 通俗理解 | 代码对应 |
|-----|---------|---------|
| 工厂 | 策略的"生产说明书" | `numa_strategy_factory_t` |
| create | "制造"策略的函数 | `composite_lru_create()` |
| destroy | "销毁"策略的函数 | `composite_lru_destroy()` |
| 策略实例 | 策略的"身体" | `numa_strategy_t` |
| 私有数据 | 策略的"个人物品" | `private_data` |
| 虚函数表 | 策略的"技能清单" | `vtable` |
| 注册 | 把"说明书"交给框架 | `numa_strategy_register_factory()` |

---

## 性能与监控

### 关键指标

```c
typedef struct {
    /* 执行统计 */
    uint64_t total_executions;
    uint64_t successful_executions;
    uint64_t failed_executions;
    double   avg_execution_time_us;
    
    /* 策略效果 */
    uint64_t migrations_triggered;
    double   load_balance_improvement;
    double   hit_ratio_improvement;
    
    /* 资源消耗 */
    size_t   memory_overhead_bytes;
    double   cpu_utilization;
} numa_strategy_metrics_t;
```

### 监控命令

```c
/*
NUMA.METRICS                    # 查看整体指标
NUMA.METRICS.SLOT <slot_id>     # 查看特定插槽指标
NUMA.METRICS.RESET              # 重置统计数据
*/
```

---

## 设计优势

1. **模块化**：策略独立实现，易于维护和扩展
2. **灵活性**：支持动态插拔和组合使用
3. **兼容性**：保留原有LRU机制，渐进式改进
4. **可配置**：丰富的运行时配置选项
5. **可观测**：完善的监控和统计机制
6. **高性能**：优先级调度，避免策略冲突

---

## 开发日志

### v1.1 1号策略集成 (2026-02-14)

#### 实现目标
将1号策略（复合LRU）集成到策略插槽框架中，实现自动注册和加载。

#### 修改文件

| 文件 | 变更 |
|-----|------|
| `src/numa_strategy_slots.h` | +1行：添加`numa_strategy_register_composite_lru()`声明 |
| `src/numa_strategy_slots.c` | +20行：包含头文件、注册工厂、插入slot 1 |
| `src/Makefile` | +1处：添加`numa_composite_lru.o` |

#### 集成流程

```c
/* 在 numa_strategy_init() 中添加 */
/* 注册内置的1号策略（Composite LRU） */
if (numa_strategy_register_composite_lru() != NUMA_STRATEGY_OK) {
    STRATEGY_LOG(LL_WARNING, "Failed to register composite-lru");
} else {
    /* 自动创建并插入1号策略到slot 1 */
    if (numa_strategy_slot_insert(1, "composite-lru") != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "Failed to insert composite-lru to slot 1");
    }
}
```

#### 启动验证

```
[Composite LRU] Strategy initialized (slot 1 default)
[NUMA Strategy] Inserted strategy 'composite-lru' to slot 1
[NUMA Strategy] Composite LRU strategy inserted to slot 1
[NUMA Strategy] Strategy slot framework initialized (slots 0,1 ready)
```

---

### v1.0 策略插槽框架实现 (2026-02-XX)

#### 实现目标
实现NUMA策略插槽框架，提供策略的注册、管理和调度功能。

#### 核心功能
1. **策略工厂机制**：支持动态注册策略类型
2. **插槽管理**：16个插槽，支持动态插拔
3. **执行调度**：按优先级执行，支持间隔控制
4. **0号兜底策略**：no-op策略验证框架可用性

#### 文件结构
- `src/numa_strategy_slots.h` - 头文件，定义接口和数据结构
- `src/numa_strategy_slots.c` - 实现文件，框架核心逻辑

#### 关键接口
```c
/* 工厂注册 */
int numa_strategy_register_factory(const numa_strategy_factory_t *factory);

/* 插槽操作 */
int numa_strategy_slot_insert(int slot_id, const char *strategy_name);
int numa_strategy_slot_remove(int slot_id);
int numa_strategy_slot_enable(int slot_id);
int numa_strategy_slot_disable(int slot_id);

/* 执行调度 */
void numa_strategy_run_all(void);
int numa_strategy_run_slot(int slot_id);
```

---
