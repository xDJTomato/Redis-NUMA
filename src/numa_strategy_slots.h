#ifndef NUMA_STRATEGY_SLOTS_H
#define NUMA_STRATEGY_SLOTS_H

#include <stdint.h>
#include <stddef.h>

typedef struct aeEventLoop aeEventLoop;

/* Strategy slot configuration. */
#define NUMA_MAX_STRATEGY_SLOTS 16       /* Maximum number of slots. */
#define NUMA_SLOT_DEFAULT_ID    1        /* Default strategy slot ID. */

/* Return value definitions. */
#define NUMA_STRATEGY_OK       0
#define NUMA_STRATEGY_ERR      -1
#define NUMA_STRATEGY_ENOENT   -2        /* Strategy does not exist. */
#define NUMA_STRATEGY_EINVAL   -3        /* Invalid argument. */
#define NUMA_STRATEGY_EEXIST   -4        /* Slot already occupied. */

/* Strategy step execution return values. */
#define NUMA_STRATEGY_STEP_IDLE       0
#define NUMA_STRATEGY_STEP_PROGRESS   1
#define NUMA_STRATEGY_STEP_DONE       2
#define NUMA_STRATEGY_STEP_AGAIN      3
#define NUMA_STRATEGY_STEP_ERROR     -1
#define NUMA_STRATEGY_STEP_TIMEOUT   -2

/* Strategy scheduling modes. */
#define NUMA_STRATEGY_SCHED_SERVERCRON 0
#define NUMA_STRATEGY_SCHED_AE         1

/* Strategy types. */
typedef enum {
    STRATEGY_TYPE_PERIODIC = 1,          /* Periodically executed strategy. */
    STRATEGY_TYPE_EVENT_DRIVEN,          /* Event-driven strategy. */
    STRATEGY_TYPE_HYBRID                 /* Hybrid strategy. */
} numa_strategy_type_t;

/* Strategy priorities. */
typedef enum {
    STRATEGY_PRIORITY_LOW = 1,           /* Low priority. */
    STRATEGY_PRIORITY_NORMAL,            /* Normal priority. */
    STRATEGY_PRIORITY_HIGH               /* High priority. */
} numa_strategy_priority_t;

/* Forward declarations. */
typedef struct numa_strategy numa_strategy_t;

/* Strategy vtable. */
typedef struct {
    /* Initialize the strategy. */
    int (*init)(numa_strategy_t *strategy);
    
    /* Execute the strategy logic. */
    int (*execute)(numa_strategy_t *strategy);
    int (*execute_step)(numa_strategy_t *strategy, uint64_t deadline_us, uint32_t budget);

    /* Clean up strategy resources. */
    void (*cleanup)(numa_strategy_t *strategy);
    
    /* Get strategy info. */
    const char* (*get_name)(numa_strategy_t *strategy);
    const char* (*get_description)(numa_strategy_t *strategy);
    
    /* Configuration management. */
    int (*set_config)(numa_strategy_t *strategy, const char *key, const char *value);
    int (*get_config)(numa_strategy_t *strategy, const char *key, char *buf, size_t buf_len);
} numa_strategy_vtable_t;

/* Strategy instance structure. */
struct numa_strategy {
    /* Basic info. */
    int slot_id;                         /* Slot ID. */
    const char *name;                    /* Strategy name. */
    const char *description;             /* Strategy description. */
    
    /* Execution control. */
    numa_strategy_type_t type;           /* Strategy type. */
    numa_strategy_priority_t priority;   /* Priority. */
    int enabled;                         /* Whether enabled. */
    uint64_t execute_interval_us;        /* Execution interval (microseconds). */
    uint64_t last_execute_time;          /* Last execution time. */
    
    /* Vtable. */
    const numa_strategy_vtable_t *vtable;
    
    /* Private data. */
    void *private_data;
    
    /* Statistics. */
    uint64_t total_executions;           /* Total executions. */
    uint64_t total_failures;             /* Failure count. */
    uint64_t total_execution_time_us;    /* Total execution time (microseconds). */

    /* AE scheduling stats. */
    int scheduler_mode;                  /* Scheduler mode. */
    long long ae_time_event_id;          /* AE time event ID */
    uint32_t step_budget;                /* Per-step budget. */
    uint32_t max_runtime_us_per_step;    /* Max runtime per step. */
    uint64_t max_execution_time_us;      /* Max single execution time. */
    uint64_t timeout_count;              /* Timeout count. */
    uint64_t last_ae_run_us;             /* Last AE execution time. */
};

/* Strategy factory structure. */
typedef struct {
    const char *name;                    /* Strategy name. */
    const char *description;             /* Strategy description. */
    numa_strategy_type_t type;           /* Strategy type. */
    numa_strategy_priority_t default_priority;
    uint64_t default_interval_us;
    
    /* Create and destroy functions. */
    numa_strategy_t* (*create)(void);
    void (*destroy)(numa_strategy_t *strategy);
} numa_strategy_factory_t;

/* ========== Core interface ========== */

/* Initialization and cleanup. */
int numa_strategy_init(void);
void numa_strategy_cleanup(void);

/* Strategy factory registration. */
int numa_strategy_register_factory(const numa_strategy_factory_t *factory);

/* Strategy creation and destruction. */
numa_strategy_t* numa_strategy_create(const char *name);
void numa_strategy_destroy(numa_strategy_t *strategy);

/* Slot operations. */
int numa_strategy_slot_insert(int slot_id, const char *strategy_name);
int numa_strategy_slot_remove(int slot_id);
int numa_strategy_slot_enable(int slot_id);
int numa_strategy_slot_disable(int slot_id);
int numa_strategy_slot_configure(int slot_id, const char *key, const char *value);

/* Query interface. */
numa_strategy_t* numa_strategy_slot_get(int slot_id);
int numa_strategy_slot_list(char *buf, size_t buf_len);
int numa_strategy_slot_status(int slot_id, char *buf, size_t buf_len);

/* Execution scheduling. */
void numa_strategy_run_all(void);                    /* Execute all enabled strategies. */
int numa_strategy_run_slot(int slot_id);            /* Execute the strategy of a given slot. */
int numa_strategy_scheduler_init(aeEventLoop *el);  /* Initialize the AE scheduler. */
int numa_strategy_slot_schedule_ae(int slot_id);    /* Register a slot AE time event. */
int numa_strategy_slot_unschedule_ae(int slot_id);  /* Unregister a slot AE time event. */
void numa_strategy_scheduler_cron(void);            /* AE scheduler health check. */

/* Built-in strategy registration functions. */
int numa_strategy_register_noop(void);               /* Register the slot 0 fallback strategy. */
int numa_strategy_register_composite_lru(void);      /* Register the slot 1 default strategy. */
int numa_strategy_register_tinylfu(void);             /* Register the slot 2 TinyLFU strategy. */

#endif /* NUMA_STRATEGY_SLOTS_H */
