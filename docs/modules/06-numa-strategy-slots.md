# 06-numa-strategy-slots.md - NUMA策略插槽模块

## 模块概述

**状态**: ✅ **已实现** - 插槽框架和0号兜底策略已完成

**文件**: [src/numa_strategy_slots.h](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_strategy_slots.h), [src/numa_strategy_slots.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_strategy_slots.c)

**功能**: 提供插槽化策略管理框架，支持动态加载、配置和组合多种NUMA负载策略。

**核心思想**:
- 策略模块化：每个策略独立实现，通过统一接口接入
- 插槽机制：策略按插槽编号组织，支持动态插拔
- 组合使用：多个策略可同时生效，按优先级执行
- 已实现：0号no-op兜底策略，用于验证框架可用性

---

## 插槽架构设计

### 核心概念

```
┌─────────────────────────────────────────────────────────────┐
│                    策略插槽管理器                            │
│  numa_strategy_manager                                      │
├─────────────────────────────────────────────────────────────┤
│  插槽0: [No-op兜底策略]   ← ✅ 已实现，默认启用             │
│  插槽1: [复合LRU策略]     ← ⚠️ 规划中，将来作为默认策略  │
│  插槽2: [用户策略A]       ← 可选                              │
│  插槽3: [用户策略B]       ← 可选                              │
│  插槽4: [用户策略C]       ← 可选                              │
│  ...                                                        │
│  插槽N: [自定义策略]      ← 动态加载                          │
└─────────────────────────────────────────────────────────────┘
```

**插槽编号说明**：
- **插槽0**：对外暴露的no-op兜底策略，默认启用，用于验证框架可用性
- **插槽1**：保留给复合LRU默认策略（尚未实现）
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

## 1号默认策略：复合LRU策略（规划中）

⚠️ **当前状态**：未实现，仅为规划文档。

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

## 策略开发规范

### 策略工厂模板

```c
/* 策略工厂定义模板 */
typedef struct {
    const char *name;
    const char *description;
    numa_strategy_type_t type;
    numa_strategy_priority_t default_priority;
    uint64_t default_interval_us;
    
    numa_strategy_t* (*create)(void);
    void (*destroy)(numa_strategy_t *strategy);
} numa_strategy_factory_t;

/* 策略实现模板 */
static int my_strategy_execute(numa_strategy_t *strategy) {
    my_strategy_data_t *data = strategy->private_data;
    
    /* 实现策略逻辑 */
    // ...
    
    return NUMA_OK;
}

static numa_strategy_t* my_strategy_create(void) {
    numa_strategy_t *strategy = zcalloc(sizeof(numa_strategy_t));
    my_strategy_data_t *data = zcalloc(sizeof(my_strategy_data_t));
    
    /* 初始化数据 */
    // ...
    
    strategy->private_data = data;
    strategy->vtable = &my_strategy_vtable;
    strategy->type = STRATEGY_TYPE_PERIODIC;
    strategy->priority = STRATEGY_PRIORITY_NORMAL;
    strategy->execute_interval_us = 5000000;  /* 5秒 */
    
    return strategy;
}

/* 注册策略 */
static numa_strategy_factory_t my_strategy_factory = {
    .name = "my_strategy",
    .description = "自定义策略描述",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_NORMAL,
    .default_interval_us = 5000000,
    .create = my_strategy_create,
    .destroy = numa_strategy_destroy
};

/* 在模块初始化时注册 */
int my_strategy_module_init(void) {
    return numa_strategy_register_factory(&my_strategy_factory);
}
```

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
