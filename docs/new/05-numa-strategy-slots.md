# 策略插槽框架

## 模块概述

`numa_strategy_slots.c/h` 实现了一个插件化的策略管理框架，支持最多 16 个策略并行运行。通过工厂模式注册策略，通过插槽机制管理和调度策略执行。

## 设计目标

1. **可扩展性**：无需修改核心代码即可添加新策略
2. **隔离性**：每个策略独立运行，互不干扰
3. **灵活性**：支持定期执行和事件驱动两种模式
4. **可观测性**：每个策略独立统计执行信息

## 核心数据结构

### 策略虚函数表

```c
typedef struct {
    // 初始化策略
    int (*init)(numa_strategy_t *strategy);

    // 执行策略逻辑（核心函数，serverCron 调用）
    int (*execute)(numa_strategy_t *strategy);

    // 清理策略资源
    void (*cleanup)(numa_strategy_t *strategy);

    // 获取策略名称和描述
    const char* (*get_name)(numa_strategy_t *strategy);
    const char* (*get_description)(numa_strategy_t *strategy);

    // 动态配置
    int (*set_config)(numa_strategy_t *strategy, const char *key, const char *value);
    int (*get_config)(numa_strategy_t *strategy, const char *key, char *buf, size_t buf_len);
} numa_strategy_vtable_t;
```

### 策略实例

```c
struct numa_strategy {
    // 基本信息
    int slot_id;                         // 所在插槽 ID
    const char *name;                    // 策略名称
    const char *description;             // 策略描述

    // 执行控制
    numa_strategy_type_t type;           // 策略类型（定期/事件/混合）
    numa_strategy_priority_t priority;   // 优先级（低/正常/高）
    int enabled;                         // 是否启用
    uint64_t execute_interval_us;        // 执行间隔（微秒）
    uint64_t last_execute_time;          // 上次执行时间

    // 虚函数表
    const numa_strategy_vtable_t *vtable;

    // 私有数据（由各策略自行定义）
    void *private_data;

    // 统计信息
    uint64_t total_executions;           // 总执行次数
    uint64_t total_failures;             // 失败次数
    uint64_t total_execution_time_us;    // 总执行时间（微秒）
};
```

### 策略工厂

```c
typedef struct {
    const char *name;                    // 策略名称
    const char *description;             // 策略描述
    numa_strategy_type_t type;           // 策略类型
    numa_strategy_priority_t default_priority;  // 默认优先级
    uint64_t default_interval_us;        // 默认执行间隔

    // 创建和销毁函数
    numa_strategy_t* (*create)(void);
    void (*destroy)(numa_strategy_t *strategy);
} numa_strategy_factory_t;
```

### 全局管理器

```c
typedef struct {
    int initialized;
    numa_strategy_t *slots[NUMA_MAX_STRATEGY_SLOTS];  // 16 个插槽
    pthread_mutex_t lock;

    // 工厂注册表
    numa_strategy_factory_t *factories[NUMA_MAX_STRATEGY_SLOTS];
    int factory_count;

    // 全局统计
    uint64_t total_runs;
    uint64_t total_strategy_executions;
} numa_strategy_manager_t;
```

## 策略类型

```c
typedef enum {
    STRATEGY_TYPE_PERIODIC = 1,     // 定期执行（如 Composite LRU 每秒执行）
    STRATEGY_TYPE_EVENT_DRIVEN,     // 事件驱动（如访问触发）
    STRATEGY_TYPE_HYBRID            // 混合模式
} numa_strategy_type_t;
```

## 策略优先级

```c
typedef enum {
    STRATEGY_PRIORITY_LOW = 1,      // 低优先级
    STRATEGY_PRIORITY_NORMAL,       // 正常优先级
    STRATEGY_PRIORITY_HIGH          // 高优先级（优先执行）
} numa_strategy_priority_t;
```

## 核心接口

### 初始化与清理

```c
// 初始化策略管理器，注册内置策略
int numa_strategy_init(void);

// 清理所有策略，释放资源
void numa_strategy_cleanup(void);
```

### 工厂注册

```c
// 注册策略工厂（通常在模块初始化时调用）
int numa_strategy_register_factory(const numa_strategy_factory_t *factory);
```

### 策略创建与销毁

```c
// 通过工厂创建策略实例
numa_strategy_t* numa_strategy_create(const char *name);

// 销毁策略并调用 cleanup
void numa_strategy_destroy(numa_strategy_t *strategy);
```

### 插槽操作

```c
// 将策略插入指定插槽
int numa_strategy_slot_insert(int slot_id, const char *strategy_name);

// 从插槽移除策略
int numa_strategy_slot_remove(int slot_id);

// 启用/禁用插槽
int numa_strategy_slot_enable(int slot_id);
int numa_strategy_slot_disable(int slot_id);

// 配置插槽参数
int numa_strategy_slot_configure(int slot_id, const char *key, const char *value);
```

### 查询接口

```c
// 获取指定插槽的策略
numa_strategy_t* numa_strategy_slot_get(int slot_id);

// 列出所有插槽状态
int numa_strategy_slot_list(char *buf, size_t buf_len);

// 获取插槽详细状态
int numa_strategy_slot_status(int slot_id, char *buf, size_t buf_len);
```

### 执行调度

```c
// 执行所有启用的策略（serverCron 调用）
void numa_strategy_run_all(void);

// 执行指定插槽的策略
int numa_strategy_run_slot(int slot_id);
```

## 执行流程

### numa_strategy_run_all()

```c
void numa_strategy_run_all(void) {
    pthread_mutex_lock(&manager.lock);

    for (int i = 0; i < NUMA_MAX_STRATEGY_SLOTS; i++) {
        numa_strategy_t *strategy = manager.slots[i];

        // 跳过空插槽和未启用的策略
        if (!strategy || !strategy->enabled) continue;

        // 检查执行间隔
        uint64_t now = get_time_us();
        if (now - strategy->last_execute_time < strategy->execute_interval_us) {
            continue;
        }

        // 执行策略
        uint64_t start = get_time_us();
        int ret = strategy->vtable->execute(strategy);
        uint64_t elapsed = get_time_us() - start;

        // 更新统计
        strategy->total_executions++;
        strategy->last_execute_time = now;
        strategy->total_execution_time_us += elapsed;
        if (ret != NUMA_STRATEGY_OK) {
            strategy->total_failures++;
        }

        manager.total_strategy_executions++;
    }

    manager.total_runs++;
    pthread_mutex_unlock(&manager.lock);
}
```

## 内置策略

### Slot 0: No-op 兜底策略

```c
// 什么都不做的策略，用于占位和测试
int numa_strategy_register_noop(void) {
    numa_strategy_factory_t factory = {
        .name = "noop",
        .description = "No-operation strategy (does nothing)",
        .type = STRATEGY_TYPE_PERIODIC,
        .default_priority = STRATEGY_PRIORITY_LOW,
        .default_interval_us = 1000000,  // 1 秒
        .create = noop_create,
        .destroy = noop_destroy
    };
    return numa_strategy_register_factory(&factory);
}
```

### Slot 1: Composite LRU 策略

```c
// 默认的 NUMA 迁移策略
int numa_strategy_register_composite_lru(void) {
    numa_strategy_factory_t factory = {
        .name = "composite-lru",
        .description = "Composite LRU migration strategy with dual-channel",
        .type = STRATEGY_TYPE_PERIODIC,
        .default_priority = STRATEGY_PRIORITY_HIGH,
        .default_interval_us = 1000000,  // 1 秒
        .create = composite_lru_create,
        .destroy = composite_lru_destroy
    };
    return numa_strategy_register_factory(&factory);
}
```

## 自定义策略开发指南

### 步骤 1：定义私有数据

```c
typedef struct {
    // 你的策略需要的数据
    int some_config;
    dict *tracking_map;
    uint64_t custom_stats;
} my_strategy_data_t;
```

### 步骤 2：实现虚函数

```c
static int my_strategy_init(numa_strategy_t *strategy) {
    my_strategy_data_t *data = zmalloc(sizeof(my_strategy_data_t));
    // 初始化...
    strategy->private_data = data;
    return NUMA_STRATEGY_OK;
}

static int my_strategy_execute(numa_strategy_t *strategy) {
    my_strategy_data_t *data = strategy->private_data;
    // 执行迁移逻辑...
    return NUMA_STRATEGY_OK;
}

static void my_strategy_cleanup(numa_strategy_t *strategy) {
    my_strategy_data_t *data = strategy->private_data;
    // 释放资源...
    zfree(data);
}

static const char* my_strategy_get_name(numa_strategy_t *strategy) {
    return "my-strategy";
}
```

### 步骤 3：创建虚函数表

```c
static numa_strategy_vtable_t my_vtable = {
    .init = my_strategy_init,
    .execute = my_strategy_execute,
    .cleanup = my_strategy_cleanup,
    .get_name = my_strategy_get_name,
    .get_description = my_strategy_get_description,
    .set_config = my_strategy_set_config,
    .get_config = my_strategy_get_config
};
```

### 步骤 4：注册工厂

```c
int numa_strategy_register_my_strategy(void) {
    numa_strategy_factory_t factory = {
        .name = "my-strategy",
        .description = "My custom NUMA migration strategy",
        .type = STRATEGY_TYPE_PERIODIC,
        .default_priority = STRATEGY_PRIORITY_NORMAL,
        .default_interval_us = 1000000,
        .create = my_strategy_create,
        .destroy = my_strategy_destroy
    };
    return numa_strategy_register_factory(&factory);
}
```

### 步骤 5：在 numa_strategy_init() 中注册

```c
int numa_strategy_init(void) {
    // ... 现有代码 ...

    // 注册你的策略
    numa_strategy_register_my_strategy();

    // 插入到空闲插槽
    numa_strategy_slot_insert(2, "my-strategy");

    return NUMA_STRATEGY_OK;
}
```

## 插槽状态

```
插槽布局：
┌───────────────────────────────────────────┐
│ Slot 0:  Noop（兜底策略，默认启用）        │
│ Slot 1:  Composite LRU（默认策略，启用）   │
│ Slot 2:  空闲（可插入自定义策略）          │
│ Slot 3:  空闲                             │
│ ...                                       │
│ Slot 15: 空闲                             │
└───────────────────────────────────────────┘
```

## 错误码

```c
#define NUMA_STRATEGY_OK       0    // 成功
#define NUMA_STRATEGY_ERR     -1    // 一般错误
#define NUMA_STRATEGY_ENOENT  -2    // 策略不存在
#define NUMA_STRATEGY_EINVAL  -3    // 参数无效
#define NUMA_STRATEGY_EEXIST  -4    // 插槽已被占用
```

## 与其他模块的交互

### 被 serverCron 调用

```c
// server.c 中
run_with_period(1000) {  // 每秒
#ifdef HAVE_NUMA
    numa_strategy_run_all();
#endif
}
```

### 与 Composite LRU 的关系

Composite LRU 是插槽 1 的具体实现：
- 通过 `composite_lru_create()` 创建实例
- 通过 `composite_lru_execute()` 执行迁移决策
- 通过 `composite_lru_destroy()` 释放资源

### 与统一命令接口的关系

`numa_command.c` 通过插槽框架管理策略：

```c
// NUMA STRATEGY LIST
numa_strategy_slot_list(buf, sizeof(buf));

// NUMA STRATEGY SLOT <id> <name>
numa_strategy_slot_insert(slot_id, strategy_name);
```

## 线程安全

所有插槽操作通过 `pthread_mutex_t lock` 保护：
- 插槽插入/移除/启用/禁用：加锁
- 策略执行：加锁（防止并发执行同一策略）
- 统计更新：加锁

## 配置参数

通过虚函数表的 `set_config`/`get_config` 接口，每个策略可以定义自己的配置项。Composite LRU 的配置项包括：

- `migrate_hotness_threshold`: 热度阈值
- `hot_candidates_size`: 候选池大小
- `scan_batch_size`: 扫描批次大小
- `auto_migrate_enabled`: 自动迁移开关
