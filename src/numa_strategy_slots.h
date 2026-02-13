#ifndef NUMA_STRATEGY_SLOTS_H
#define NUMA_STRATEGY_SLOTS_H

#include <stdint.h>
#include <stddef.h>

/* 策略插槽配置 */
#define NUMA_MAX_STRATEGY_SLOTS 16       /* 最大插槽数 */
#define NUMA_SLOT_DEFAULT_ID    1        /* 默认策略插槽ID */

/* 返回值定义 */
#define NUMA_STRATEGY_OK       0
#define NUMA_STRATEGY_ERR      -1
#define NUMA_STRATEGY_ENOENT   -2        /* 策略不存在 */
#define NUMA_STRATEGY_EINVAL   -3        /* 参数无效 */
#define NUMA_STRATEGY_EEXIST   -4        /* 插槽已被占用 */

/* 策略类型 */
typedef enum {
    STRATEGY_TYPE_PERIODIC = 1,          /* 定期执行策略 */
    STRATEGY_TYPE_EVENT_DRIVEN,          /* 事件驱动策略 */
    STRATEGY_TYPE_HYBRID                 /* 混合策略 */
} numa_strategy_type_t;

/* 策略优先级 */
typedef enum {
    STRATEGY_PRIORITY_LOW = 1,           /* 低优先级 */
    STRATEGY_PRIORITY_NORMAL,            /* 正常优先级 */
    STRATEGY_PRIORITY_HIGH               /* 高优先级 */
} numa_strategy_priority_t;

/* 前向声明 */
typedef struct numa_strategy numa_strategy_t;

/* 策略虚函数表 */
typedef struct {
    /* 初始化策略 */
    int (*init)(numa_strategy_t *strategy);
    
    /* 执行策略逻辑 */
    int (*execute)(numa_strategy_t *strategy);
    
    /* 清理策略资源 */
    void (*cleanup)(numa_strategy_t *strategy);
    
    /* 获取策略信息 */
    const char* (*get_name)(numa_strategy_t *strategy);
    const char* (*get_description)(numa_strategy_t *strategy);
    
    /* 配置管理 */
    int (*set_config)(numa_strategy_t *strategy, const char *key, const char *value);
    int (*get_config)(numa_strategy_t *strategy, const char *key, char *buf, size_t buf_len);
} numa_strategy_vtable_t;

/* 策略实例结构 */
struct numa_strategy {
    /* 基本信息 */
    int slot_id;                         /* 插槽ID */
    const char *name;                    /* 策略名称 */
    const char *description;             /* 策略描述 */
    
    /* 执行控制 */
    numa_strategy_type_t type;           /* 策略类型 */
    numa_strategy_priority_t priority;   /* 优先级 */
    int enabled;                         /* 是否启用 */
    uint64_t execute_interval_us;        /* 执行间隔(微秒) */
    uint64_t last_execute_time;          /* 上次执行时间 */
    
    /* 虚函数表 */
    const numa_strategy_vtable_t *vtable;
    
    /* 私有数据 */
    void *private_data;
    
    /* 统计信息 */
    uint64_t total_executions;           /* 总执行次数 */
    uint64_t total_failures;             /* 失败次数 */
    uint64_t total_execution_time_us;    /* 总执行时间(微秒) */
};

/* 策略工厂结构 */
typedef struct {
    const char *name;                    /* 策略名称 */
    const char *description;             /* 策略描述 */
    numa_strategy_type_t type;           /* 策略类型 */
    numa_strategy_priority_t default_priority;
    uint64_t default_interval_us;
    
    /* 创建和销毁函数 */
    numa_strategy_t* (*create)(void);
    void (*destroy)(numa_strategy_t *strategy);
} numa_strategy_factory_t;

/* ========== 核心接口 ========== */

/* 初始化与清理 */
int numa_strategy_init(void);
void numa_strategy_cleanup(void);

/* 策略工厂注册 */
int numa_strategy_register_factory(const numa_strategy_factory_t *factory);

/* 策略创建与销毁 */
numa_strategy_t* numa_strategy_create(const char *name);
void numa_strategy_destroy(numa_strategy_t *strategy);

/* 插槽操作 */
int numa_strategy_slot_insert(int slot_id, const char *strategy_name);
int numa_strategy_slot_remove(int slot_id);
int numa_strategy_slot_enable(int slot_id);
int numa_strategy_slot_disable(int slot_id);
int numa_strategy_slot_configure(int slot_id, const char *key, const char *value);

/* 查询接口 */
numa_strategy_t* numa_strategy_slot_get(int slot_id);
int numa_strategy_slot_list(char *buf, size_t buf_len);
int numa_strategy_slot_status(int slot_id, char *buf, size_t buf_len);

/* 执行调度 */
void numa_strategy_run_all(void);                    /* 执行所有启用的策略 */
int numa_strategy_run_slot(int slot_id);            /* 执行指定插槽策略 */

/* 内置策略注册函数 */
int numa_strategy_register_noop(void);               /* 注册0号兜底策略 */

#endif /* NUMA_STRATEGY_SLOTS_H */
