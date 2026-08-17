/*
 * NUMA strategy slot framework implementation
 * Provides strategy registration, management, and scheduling.
 */

#include "numa_strategy_slots.h"
#include "numa_composite_lru.h"
#include "numa_tinylfu.h"
#include "zmalloc.h"
#include "ae.h"
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdio.h>

/* Log macro definitions. */
#ifdef NUMA_STRATEGY_STANDALONE
#define STRATEGY_LOG(level, fmt, ...) printf("[%s] " fmt "\n", level, ##__VA_ARGS__)
#else
/* Forward declaration, provided by server.o at link time. */
/* Redis internally uses _serverLog as the actual function name. */
extern void _serverLog(int level, const char *fmt, ...);
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define STRATEGY_LOG(level, fmt, ...) _serverLog(level, fmt, ##__VA_ARGS__)
#endif

/* ========== Global manager ========== */

typedef struct {
    int initialized;                              /* Initialization flag. */
    numa_strategy_t *slots[NUMA_MAX_STRATEGY_SLOTS]; /* Slot array. */
    pthread_mutex_t lock;                         /* Thread-safety lock. */
    
    /* Factory registry. */
    numa_strategy_factory_t *factories[NUMA_MAX_STRATEGY_SLOTS];
    int factory_count;
    
    /* Statistics. */
    uint64_t total_runs;                          /* Total schedule runs. */
    uint64_t total_strategy_executions;           /* Total strategy executions. */

    /* AE scheduler. */
    aeEventLoop *event_loop;
} numa_strategy_manager_t;

static numa_strategy_manager_t strategy_manager = {0};

static int numa_strategy_slot_time_proc(aeEventLoop *eventLoop, long long id, void *clientData);

/* ========== Helper functions ========== */

/* Get the current time (microseconds). */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Look up a registered factory. */
static numa_strategy_factory_t* find_factory(const char *name) {
    for (int i = 0; i < strategy_manager.factory_count; i++) {
        if (strcmp(strategy_manager.factories[i]->name, name) == 0) {
            return strategy_manager.factories[i];
        }
    }
    return NULL;
}

/* ========== Slot 0 fallback strategy implementation ========== */

/* Slot 0 strategy private data. */
typedef struct {
    uint64_t execution_count;      /* Execution count. */
    uint64_t last_log_time;        /* Last log time. */
} noop_strategy_data_t;

/* Slot 0 strategy initialization. */
static int noop_strategy_init(numa_strategy_t *strategy) {
    noop_strategy_data_t *data = zmalloc(sizeof(*data));
    if (!data) return NUMA_STRATEGY_ERR;
    
    data->execution_count = 0;
    data->last_log_time = 0;
    strategy->private_data = data;
    
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy Slot 0] No-op strategy initialized");
    return NUMA_STRATEGY_OK;
}

/* Slot 0 strategy execution. */
static int noop_strategy_execute(numa_strategy_t *strategy) {
    noop_strategy_data_t *data = strategy->private_data;
    uint64_t now = get_current_time_us();
    
    data->execution_count++;
    
    /* Log at most once every 10 seconds to avoid log spam. */
    if (now - data->last_log_time > 10000000) {  /* 10 seconds. */
        STRATEGY_LOG(LL_VERBOSE, 
                  "[NUMA Strategy Slot 0] No-op strategy executed (count: %llu)",
                  (unsigned long long)data->execution_count);
        data->last_log_time = now;
    }
    
    return NUMA_STRATEGY_OK;
}

/* Slot 0 strategy cleanup. */
static void noop_strategy_cleanup(numa_strategy_t *strategy) {
    if (strategy->private_data) {
        noop_strategy_data_t *data = strategy->private_data;
        STRATEGY_LOG(LL_NOTICE, 
                  "[NUMA Strategy Slot 0] No-op strategy cleanup (total executions: %llu)",
                  (unsigned long long)data->execution_count);
        zfree(data);
        strategy->private_data = NULL;
    }
}

/* Slot 0 strategy info. */
static const char* noop_strategy_get_name(numa_strategy_t *strategy) {
    (void)strategy;
    return "noop";
}

static const char* noop_strategy_get_description(numa_strategy_t *strategy) {
    (void)strategy;
    return "Slot 0 default policy: no-op fallback policy for framework validation";
}

/* Slot 0 strategy configuration (not supported yet). */
static int noop_strategy_set_config(numa_strategy_t *strategy, 
                                   const char *key, const char *value) {
    (void)strategy; (void)key; (void)value;
    return NUMA_STRATEGY_EINVAL;
}

static int noop_strategy_get_config(numa_strategy_t *strategy, 
                                   const char *key, char *buf, size_t buf_len) {
    (void)strategy; (void)key; (void)buf; (void)buf_len;
    return NUMA_STRATEGY_EINVAL;
}

/* Slot 0 strategy vtable. */
static const numa_strategy_vtable_t noop_strategy_vtable = {
    .init = noop_strategy_init,
    .execute = noop_strategy_execute,
    .cleanup = noop_strategy_cleanup,
    .get_name = noop_strategy_get_name,
    .get_description = noop_strategy_get_description,
    .set_config = noop_strategy_set_config,
    .get_config = noop_strategy_get_config
};

/* Slot 0 strategy creation. */
static numa_strategy_t* noop_strategy_create(void) {
    numa_strategy_t *strategy = zmalloc(sizeof(*strategy));
    if (!strategy) return NULL;
    
    memset(strategy, 0, sizeof(*strategy));
    strategy->slot_id = 0;
    strategy->name = "noop";
    strategy->description = "Slot 0 no-op fallback policy";
    strategy->type = STRATEGY_TYPE_PERIODIC;
    strategy->priority = STRATEGY_PRIORITY_LOW;
    strategy->enabled = 1;  /* Enabled by default. */
    strategy->execute_interval_us = 1000000;  /* 1-second execution interval. */
    strategy->scheduler_mode = NUMA_STRATEGY_SCHED_SERVERCRON;
    strategy->ae_time_event_id = AE_DELETED_EVENT_ID;
    strategy->step_budget = 64;
    strategy->max_runtime_us_per_step = 500;
    strategy->vtable = &noop_strategy_vtable;
    
    return strategy;
}

/* Slot 0 strategy destruction. */
static void noop_strategy_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    if (strategy->vtable && strategy->vtable->cleanup) {
        strategy->vtable->cleanup(strategy);
    }
    zfree(strategy);
}

/* Slot 0 strategy factory. */
static numa_strategy_factory_t noop_strategy_factory = {
    .name = "noop",
    .description = "No-op fallback policy",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_LOW,
    .default_interval_us = 1000000,
    .create = noop_strategy_create,
    .destroy = noop_strategy_destroy
};

/* Register the slot 0 strategy. */
int numa_strategy_register_noop(void) {
    return numa_strategy_register_factory(&noop_strategy_factory);
}

/* Register the slot 1 strategy (forwards to the composite_lru module). */
int numa_strategy_register_composite_lru(void) {
    return numa_composite_lru_register();
}

/* Register the slot 2 strategy (forwards to the tinylfu module). */
int numa_strategy_register_tinylfu(void) {
    return numa_tinylfu_register();
}

/* ========== Strategy manager implementation ========== */

/* Initialize the strategy manager. */
int numa_strategy_init(void) {
    if (strategy_manager.initialized) {
        return NUMA_STRATEGY_OK;
    }
    
    memset(&strategy_manager, 0, sizeof(strategy_manager));
    pthread_mutex_init(&strategy_manager.lock, NULL);
    
    /* Register the built-in slot 0 strategy. */
    if (numa_strategy_register_noop() != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to register no-op strategy");
        return NUMA_STRATEGY_ERR;
    }
    
    /* Automatically create and insert the slot 0 strategy. */
    if (numa_strategy_slot_insert(0, "noop") != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to insert no-op strategy to slot 0");
        return NUMA_STRATEGY_ERR;
    }
    
    /* Register the built-in slot 1 strategy (Composite LRU). */
    if (numa_strategy_register_composite_lru() != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to register composite-lru strategy");
        /* A slot 1 registration failure does not abort framework init. */
    } else {
        /* Automatically create and insert the slot 1 strategy. */
        if (numa_strategy_slot_insert(1, "composite-lru") != NUMA_STRATEGY_OK) {
            STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to insert composite-lru to slot 1");
        } else {
            STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Composite LRU strategy inserted to slot 1");
        }
    }
    
    /* Register the built-in slot 2 strategy (TinyLFU). */
    if (numa_tinylfu_register() != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to register tinylfu strategy");
    } else {
        if (numa_strategy_slot_insert(2, "tinylfu") != NUMA_STRATEGY_OK) {
            STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to insert tinylfu to slot 2");
        } else {
            /* Slot 2 is disabled by default; users must enable it manually. */
            numa_strategy_slot_disable(2);
            STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] TinyLFU strategy inserted to slot 2 (disabled by default)");
        }
    }

    strategy_manager.initialized = 1;
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Strategy slot framework initialized (slots 0,1,2 ready)");

    return NUMA_STRATEGY_OK;
}

/* Clean up the strategy manager. */
void numa_strategy_cleanup(void) {
    if (!strategy_manager.initialized) return;
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    /* Clean up all slots. */
    for (int i = 0; i < NUMA_MAX_STRATEGY_SLOTS; i++) {
        if (strategy_manager.slots[i]) {
            numa_strategy_destroy(strategy_manager.slots[i]);
            strategy_manager.slots[i] = NULL;
        }
    }
    
    strategy_manager.initialized = 0;
    pthread_mutex_unlock(&strategy_manager.lock);
    pthread_mutex_destroy(&strategy_manager.lock);
    
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Strategy slot framework cleaned up");
}

/* Register a strategy factory. */
int numa_strategy_register_factory(const numa_strategy_factory_t *factory) {
    if (!factory || !factory->name || !factory->create || !factory->destroy) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    /* Check whether it already exists. */
    if (find_factory(factory->name) != NULL) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_EEXIST;
    }
    
    /* Check capacity. */
    if (strategy_manager.factory_count >= NUMA_MAX_STRATEGY_SLOTS) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ERR;
    }
    
    /* Register the factory. */
    strategy_manager.factories[strategy_manager.factory_count++] = 
        (numa_strategy_factory_t*)factory;
    
    pthread_mutex_unlock(&strategy_manager.lock);
    
    STRATEGY_LOG(LL_VERBOSE, "[NUMA Strategy] Registered strategy factory: %s", factory->name);
    return NUMA_STRATEGY_OK;
}

/* Create a strategy instance. */
numa_strategy_t* numa_strategy_create(const char *name) {
    if (!name) return NULL;
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_factory_t *factory = find_factory(name);
    if (!factory) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NULL;
    }
    
    numa_strategy_t *strategy = factory->create();
    pthread_mutex_unlock(&strategy_manager.lock);
    
    if (strategy && strategy->vtable && strategy->vtable->init) {
        if (strategy->vtable->init(strategy) != NUMA_STRATEGY_OK) {
            numa_strategy_destroy(strategy);
            return NULL;
        }
    }
    
    return strategy;
}

/* Destroy a strategy instance. */
void numa_strategy_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_factory_t *factory = find_factory(strategy->name);
    if (factory && factory->destroy) {
        factory->destroy(strategy);
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
}

/* Insert a strategy into a slot. */
int numa_strategy_slot_insert(int slot_id, const char *strategy_name) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS || !strategy_name) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    /* Check whether the slot is already occupied. */
    if (strategy_manager.slots[slot_id] != NULL) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_EEXIST;
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
    
    /* Create the strategy instance. */
    numa_strategy_t *strategy = numa_strategy_create(strategy_name);
    if (!strategy) {
        return NUMA_STRATEGY_ENOENT;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    strategy->slot_id = slot_id;
    strategy_manager.slots[slot_id] = strategy;
    if (strategy->ae_time_event_id == 0)
        strategy->ae_time_event_id = AE_DELETED_EVENT_ID;
    if (strategy->step_budget == 0)
        strategy->step_budget = 64;
    if (strategy->max_runtime_us_per_step == 0)
        strategy->max_runtime_us_per_step = 500;
    pthread_mutex_unlock(&strategy_manager.lock);
    
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Inserted strategy '%s' to slot %d", 
              strategy_name, slot_id);
    
    return NUMA_STRATEGY_OK;
}

/* Remove the strategy from a slot. */
int numa_strategy_slot_remove(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ENOENT;
    }

    long long ae_id = strategy->ae_time_event_id;
    strategy->ae_time_event_id = AE_DELETED_EVENT_ID;
    strategy_manager.slots[slot_id] = NULL;
    pthread_mutex_unlock(&strategy_manager.lock);

    if (ae_id != AE_DELETED_EVENT_ID && strategy_manager.event_loop)
        aeDeleteTimeEvent(strategy_manager.event_loop, ae_id);

    numa_strategy_destroy(strategy);
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Removed strategy from slot %d", slot_id);
    
    return NUMA_STRATEGY_OK;
}

/* Enable a slot. */
int numa_strategy_slot_enable(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ENOENT;
    }
    
    strategy->enabled = 1;
    int schedule_ae = (strategy->scheduler_mode == NUMA_STRATEGY_SCHED_AE &&
                       strategy->ae_time_event_id == AE_DELETED_EVENT_ID &&
                       strategy_manager.event_loop != NULL);
    pthread_mutex_unlock(&strategy_manager.lock);

    if (schedule_ae)
        numa_strategy_slot_schedule_ae(slot_id);

    STRATEGY_LOG(LL_VERBOSE, "[NUMA Strategy] Enabled slot %d", slot_id);
    return NUMA_STRATEGY_OK;
}

/* Disable a slot. */
int numa_strategy_slot_disable(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ENOENT;
    }
    
    strategy->enabled = 0;
    long long ae_id = strategy->ae_time_event_id;
    strategy->ae_time_event_id = AE_DELETED_EVENT_ID;
    pthread_mutex_unlock(&strategy_manager.lock);

    if (ae_id != AE_DELETED_EVENT_ID && strategy_manager.event_loop)
        aeDeleteTimeEvent(strategy_manager.event_loop, ae_id);

    STRATEGY_LOG(LL_VERBOSE, "[NUMA Strategy] Disabled slot %d", slot_id);
    return NUMA_STRATEGY_OK;
}

/* Configure a slot. */
int numa_strategy_slot_configure(int slot_id, const char *key, const char *value) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS || !key || !value) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ENOENT;
    }
    
    int result = NUMA_STRATEGY_EINVAL;
    if (strategy->vtable && strategy->vtable->set_config) {
        result = strategy->vtable->set_config(strategy, key, value);
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
    return result;
}

/* Get the strategy of a slot. */
numa_strategy_t* numa_strategy_slot_get(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NULL;
    }
    /* Hot path: this function is called on every lookupKey hit. Redis runs a
     * single-threaded event loop, and the slot array is only modified on the
     * main thread (NUMA STRATEGY command / serverCron / AE time events), so a
     * plain read is safe and avoids locking the global mutex on Redis's hottest path. */
    return strategy_manager.slots[slot_id];
}

/* List the state of all slots. */
int numa_strategy_slot_list(char *buf, size_t buf_len) {
    if (!buf || buf_len == 0) return NUMA_STRATEGY_EINVAL;
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    int offset = 0;
    for (int i = 0; i < NUMA_MAX_STRATEGY_SLOTS; i++) {
        numa_strategy_t *strategy = strategy_manager.slots[i];
        if (strategy) {
            offset += snprintf(buf + offset, buf_len - offset,
                             "Slot %d: %s (%s) %s\n",
                             i, strategy->name, 
                             strategy->enabled ? "enabled" : "disabled",
                             strategy->description);
            if (offset >= (int)buf_len) break;
        }
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
    return NUMA_STRATEGY_OK;
}

/* Get the state of a slot. */
int numa_strategy_slot_status(int slot_id, char *buf, size_t buf_len) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS || !buf || buf_len == 0) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy) {
        pthread_mutex_unlock(&strategy_manager.lock);
        snprintf(buf, buf_len, "Slot %d: empty\n", slot_id);
        return NUMA_STRATEGY_ENOENT;
    }
    
    snprintf(buf, buf_len,
             "Slot %d: %s\n"
             "  Description: %s\n"
             "  Status: %s\n"
             "  Scheduler: %s\n"
             "  AE time event: %lld\n"
             "  Step budget: %u\n"
             "  Max runtime per step: %u us\n"
             "  Executions: %llu\n"
             "  Failures: %llu\n"
             "  Total time: %llu us\n"
             "  Max time: %llu us\n"
             "  Timeouts: %llu\n",
             slot_id, strategy->name,
             strategy->description,
             strategy->enabled ? "enabled" : "disabled",
             strategy->scheduler_mode == NUMA_STRATEGY_SCHED_AE ? "ae" : "servercron",
             strategy->ae_time_event_id,
             strategy->step_budget,
             strategy->max_runtime_us_per_step,
             (unsigned long long)strategy->total_executions,
             (unsigned long long)strategy->total_failures,
             (unsigned long long)strategy->total_execution_time_us,
             (unsigned long long)strategy->max_execution_time_us,
             (unsigned long long)strategy->timeout_count);
    
    pthread_mutex_unlock(&strategy_manager.lock);
    return NUMA_STRATEGY_OK;
}

/* Execute the strategy of a given slot. */
int numa_strategy_run_slot(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy || !strategy->enabled) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ENOENT;
    }
    
    uint64_t now = get_current_time_us();
    
    /* Check the execution interval. */
    if (now - strategy->last_execute_time < strategy->execute_interval_us) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_OK;
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
    
    /* Execute the strategy. */
    uint64_t start_time = get_current_time_us();
    int result = NUMA_STRATEGY_OK;
    
    if (strategy->vtable && strategy->vtable->execute) {
        result = strategy->vtable->execute(strategy);
    }
    
    uint64_t elapsed = get_current_time_us() - start_time;
    
    /* Update the statistics. */
    pthread_mutex_lock(&strategy_manager.lock);
    strategy->last_execute_time = now;
    strategy->total_executions++;
    strategy->total_execution_time_us += elapsed;
    if (elapsed > strategy->max_execution_time_us)
        strategy->max_execution_time_us = elapsed;
    if (strategy->max_runtime_us_per_step && elapsed >= strategy->max_runtime_us_per_step)
        strategy->timeout_count++;
    if (result < 0) {
        strategy->total_failures++;
    }
    pthread_mutex_unlock(&strategy_manager.lock);
    
    return result;
}

/* AE scheduler. */
int numa_strategy_scheduler_init(aeEventLoop *el) {
    if (!el) return NUMA_STRATEGY_EINVAL;

    pthread_mutex_lock(&strategy_manager.lock);
    strategy_manager.event_loop = el;
    pthread_mutex_unlock(&strategy_manager.lock);

    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] AE scheduler initialized");
    return NUMA_STRATEGY_OK;
}

int numa_strategy_slot_schedule_ae(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NUMA_STRATEGY_EINVAL;
    }

    pthread_mutex_lock(&strategy_manager.lock);

    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy || !strategy->enabled) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ENOENT;
    }
    if (!strategy_manager.event_loop) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ERR;
    }
    if (strategy->ae_time_event_id != AE_DELETED_EVENT_ID) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_OK;
    }

    long long delay_ms = strategy->execute_interval_us / 1000;
    if (delay_ms <= 0) delay_ms = 1;
    aeEventLoop *el = strategy_manager.event_loop;
    pthread_mutex_unlock(&strategy_manager.lock);

    long long id = aeCreateTimeEvent(el, delay_ms, numa_strategy_slot_time_proc,
                                     (void*)(intptr_t)slot_id, NULL);
    if (id == AE_ERR) return NUMA_STRATEGY_ERR;

    pthread_mutex_lock(&strategy_manager.lock);
    strategy = strategy_manager.slots[slot_id];
    if (strategy && strategy->enabled) {
        strategy->scheduler_mode = NUMA_STRATEGY_SCHED_AE;
        strategy->ae_time_event_id = id;
    }
    pthread_mutex_unlock(&strategy_manager.lock);

    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Scheduled slot %d as AE time event %lld", slot_id, id);
    return NUMA_STRATEGY_OK;
}

int numa_strategy_slot_unschedule_ae(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NUMA_STRATEGY_EINVAL;
    }

    pthread_mutex_lock(&strategy_manager.lock);

    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ENOENT;
    }

    long long id = strategy->ae_time_event_id;
    strategy->ae_time_event_id = AE_DELETED_EVENT_ID;
    strategy->scheduler_mode = NUMA_STRATEGY_SCHED_SERVERCRON;
    aeEventLoop *el = strategy_manager.event_loop;
    pthread_mutex_unlock(&strategy_manager.lock);

    if (id != AE_DELETED_EVENT_ID && el)
        aeDeleteTimeEvent(el, id);

    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Unscheduled slot %d from AE", slot_id);
    return NUMA_STRATEGY_OK;
}

void numa_strategy_scheduler_cron(void) {
    if (!strategy_manager.initialized || !strategy_manager.event_loop) return;

    for (int slot_id = 0; slot_id < NUMA_MAX_STRATEGY_SLOTS; slot_id++) {
        pthread_mutex_lock(&strategy_manager.lock);
        numa_strategy_t *strategy = strategy_manager.slots[slot_id];
        int needs_schedule = strategy && strategy->enabled &&
                             strategy->scheduler_mode == NUMA_STRATEGY_SCHED_AE &&
                             strategy->ae_time_event_id == AE_DELETED_EVENT_ID;
        pthread_mutex_unlock(&strategy_manager.lock);

        if (needs_schedule)
            numa_strategy_slot_schedule_ae(slot_id);
    }
}

static int numa_strategy_slot_time_proc(aeEventLoop *eventLoop, long long id, void *clientData) {
    (void)eventLoop;
    int slot_id = (int)(intptr_t)clientData;

    pthread_mutex_lock(&strategy_manager.lock);
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    if (!strategy || !strategy->enabled || strategy->ae_time_event_id != id) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return AE_NOMORE;
    }

    uint32_t budget = strategy->step_budget;
    uint32_t max_runtime_us = strategy->max_runtime_us_per_step;
    uint64_t interval_us = strategy->execute_interval_us;
    pthread_mutex_unlock(&strategy_manager.lock);

    uint64_t start_time = get_current_time_us();
    uint64_t deadline_us = start_time + max_runtime_us;
    int result = NUMA_STRATEGY_STEP_IDLE;

    if (strategy->vtable && strategy->vtable->execute_step) {
        result = strategy->vtable->execute_step(strategy, deadline_us, budget);
    } else if (strategy->vtable && strategy->vtable->execute) {
        result = strategy->vtable->execute(strategy);
    }

    uint64_t elapsed = get_current_time_us() - start_time;

    pthread_mutex_lock(&strategy_manager.lock);
    strategy = strategy_manager.slots[slot_id];
    if (!strategy || !strategy->enabled || strategy->ae_time_event_id != id) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return AE_NOMORE;
    }

    strategy->last_ae_run_us = start_time;
    strategy->last_execute_time = start_time;
    strategy->total_executions++;
    strategy->total_execution_time_us += elapsed;
    if (elapsed > strategy->max_execution_time_us)
        strategy->max_execution_time_us = elapsed;
    if (max_runtime_us && elapsed >= max_runtime_us)
        strategy->timeout_count++;
    if (result < 0)
        strategy->total_failures++;

    int next_ms;
    if (result == NUMA_STRATEGY_STEP_AGAIN || result == NUMA_STRATEGY_STEP_TIMEOUT) {
        next_ms = 1;
    } else {
        next_ms = interval_us / 1000;
        if (next_ms <= 0) next_ms = 1;
    }

    pthread_mutex_unlock(&strategy_manager.lock);
    return next_ms;
}

/* Execute all enabled strategies. */
void numa_strategy_run_all(void) {
    if (!strategy_manager.initialized) return;

    strategy_manager.total_runs++;
    
    /* Execute by priority: HIGH -> NORMAL -> LOW. */
    for (int priority = (int)STRATEGY_PRIORITY_HIGH; 
         priority >= (int)STRATEGY_PRIORITY_LOW; 
         priority--) {
        
        for (int slot_id = 0; slot_id < NUMA_MAX_STRATEGY_SLOTS; slot_id++) {
            pthread_mutex_lock(&strategy_manager.lock);
            numa_strategy_t *strategy = strategy_manager.slots[slot_id];
            
            if (strategy && strategy->enabled && strategy->priority == priority &&
                strategy->scheduler_mode == NUMA_STRATEGY_SCHED_SERVERCRON) {
                pthread_mutex_unlock(&strategy_manager.lock);
                numa_strategy_run_slot(slot_id);
                strategy_manager.total_strategy_executions++;
            } else {
                pthread_mutex_unlock(&strategy_manager.lock);
            }
        }
    }
}
