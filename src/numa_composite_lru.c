/*
 * NUMA Composite LRU Strategy Implementation (Slot 1)
 *
 * This is the default NUMA migration strategy, providing stability-oriented
 * hotness management and intelligent migration triggering based on Redis's
 * native LRU mechanism.
 */

#define _GNU_SOURCE  /* For sched_getcpu() */
#include "numa_composite_lru.h"
#include "zmalloc.h"
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <numa.h>

/* ========== Logging ========== */

#ifdef NUMA_STRATEGY_STANDALONE
#define CLRU_LOG(level, fmt, ...) printf("[%s] " fmt "\n", level, ##__VA_ARGS__)
#else
extern void _serverLog(int level, const char *fmt, ...);
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define CLRU_LOG(level, fmt, args...) _serverLog(level, fmt, ##args)
#endif

/* ========== Helper Functions ========== */

/* Get current time in microseconds */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Get LRU clock (simplified version) */
static uint16_t get_lru_clock(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* Return low 16 bits of seconds, similar to Redis LRU_CLOCK */
    return (uint16_t)(tv.tv_sec & 0xFFFF);
}

/* Calculate time delta handling wrap-around */
static uint16_t calculate_time_delta(uint16_t current, uint16_t last) {
    if (current >= last) {
        return current - last;
    } else {
        return (0xFFFF - last) + current + 1;
    }
}

/* Get current NUMA node */
static int get_current_numa_node(void) {
    if (numa_available() < 0) return 0;
    return numa_node_of_cpu(sched_getcpu());
}

/* ========== Dict Callbacks for Heat Map ========== */

/* Hash function for keys (pointer-based) */
static uint64_t heat_map_hash(const void *key) {
    return (uint64_t)(uintptr_t)key;
}

/* Key compare function */
static int heat_map_key_compare(void *privdata, const void *key1, const void *key2) {
    (void)privdata;
    return key1 == key2;
}

/* Value destructor */
static void heat_map_val_destructor(void *privdata, void *val) {
    (void)privdata;
    zfree(val);
}

/* Dict type for heat map */
static dictType heat_map_dict_type = {
    .hashFunction = heat_map_hash,
    .keyDup = NULL,
    .valDup = NULL,
    .keyCompare = heat_map_key_compare,
    .keyDestructor = NULL,
    .valDestructor = heat_map_val_destructor
};

/* ========== Pending Migration Management ========== */

/* Free pending migration entry */
static void free_pending_migration(void *ptr) {
    zfree(ptr);
}

/* ========== Resource Monitoring ========== */

/* Check resource status of target node */
static int check_resource_status(composite_lru_data_t *data, int node_id) {
    (void)data;
    (void)node_id;
    
    /* Simplified resource check - in production would query actual metrics */
    /* For now, always return available */
    return RESOURCE_AVAILABLE;
}

/* ========== Heat Management ========== */

/* Record key access */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val) {
    if (!strategy || !strategy->private_data || !key) return;
    
    composite_lru_data_t *data = strategy->private_data;
    composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, key);
    
    int current_node = get_current_numa_node();
    uint16_t current_time = get_lru_clock();
    
    if (!info) {
        /* First access: create heat record */
        info = zmalloc(sizeof(*info));
        if (!info) return;
        
        info->hotness = 1;
        info->stability_counter = 0;
        info->last_access = current_time;
        info->access_count = 1;
        info->current_node = current_node;
        info->preferred_node = -1;
        
        dictAdd(data->key_heat_map, key, info);
        data->heat_updates++;
        return;
    }
    
    /* Update access statistics */
    info->access_count++;
    info->last_access = current_time;
    data->heat_updates++;
    
    /* Node affinity analysis */
    if (info->current_node == current_node) {
        /* Local access: increase hotness with stability */
        if (info->hotness < COMPOSITE_LRU_HOTNESS_MAX) {
            info->hotness++;
        }
        info->stability_counter = 0;  /* Reset stability counter */
    } else {
        /* Remote access: may trigger migration evaluation */
        info->preferred_node = current_node;
        
        if (info->hotness >= data->migrate_hotness_threshold) {
            /* Schedule migration evaluation */
            /* For now, just record the preferred node */
            CLRU_LOG(LL_VERBOSE, 
                "[Composite LRU] Remote access detected for key, current=%d, accessed_from=%d, hotness=%d",
                info->current_node, current_node, info->hotness);
        }
    }
    
    (void)val;  /* Currently unused */
}

/* Perform heat decay */
void composite_lru_decay_heat(composite_lru_data_t *data) {
    if (!data || !data->key_heat_map) return;
    
    dictIterator *di = dictGetSafeIterator(data->key_heat_map);
    dictEntry *de;
    uint16_t current_time = get_lru_clock();
    uint16_t decay_threshold_lru = (uint16_t)(data->decay_threshold / 1000000);  /* Convert to seconds */
    
    while ((de = dictNext(di)) != NULL) {
        composite_lru_heat_info_t *info = dictGetVal(de);
        
        uint16_t time_diff = calculate_time_delta(current_time, info->last_access);
        
        /* Stability-based decay: only decrement after multiple threshold violations */
        if (time_diff > decay_threshold_lru) {
            info->stability_counter++;
            
            if (info->stability_counter > data->stability_count) {
                if (info->hotness > COMPOSITE_LRU_HOTNESS_MIN) {
                    info->hotness--;
                    data->decay_operations++;
                }
                info->stability_counter = 0;
            }
        } else {
            /* Recent access: reset stability counter */
            info->stability_counter = 0;
        }
    }
    
    dictReleaseIterator(di);
}

/* ========== Migration Processing ========== */

/* Process pending migrations */
static void process_pending_migrations(composite_lru_data_t *data) {
    if (!data || !data->pending_migrations) return;
    
    uint64_t now = get_current_time_us();
    listNode *node, *next;
    
    node = listFirst(data->pending_migrations);
    while (node) {
        next = listNextNode(node);
        pending_migration_t *pm = listNodeValue(node);
        
        /* Check for timeout */
        if (now - pm->enqueue_time > COMPOSITE_LRU_PENDING_TIMEOUT) {
            data->pending_timeouts++;
            listDelNode(data->pending_migrations, node);
            node = next;
            continue;
        }
        
        /* Check if resource is now available */
        int status = check_resource_status(data, pm->target_node);
        if (status == RESOURCE_AVAILABLE) {
            /* Execute migration - would call numa_migrate_single_key here */
            CLRU_LOG(LL_VERBOSE, 
                "[Composite LRU] Processing pending migration to node %d",
                pm->target_node);
            
            /* Migration logic would go here */
            data->migrations_triggered++;
            listDelNode(data->pending_migrations, node);
        }
        
        node = next;
    }
}

/* Trigger load balancing if needed */
static void check_load_balancing(numa_strategy_t *strategy) {
    (void)strategy;
    
    /* In production, would analyze node loads and trigger rebalancing */
    /* For now, this is a placeholder */
}

/* ========== Strategy vtable Implementation ========== */

/* Strategy initialization */
int composite_lru_init(numa_strategy_t *strategy) {
    composite_lru_data_t *data = zmalloc(sizeof(*data));
    if (!data) return NUMA_STRATEGY_ERR;
    
    memset(data, 0, sizeof(*data));
    
    /* Initialize parameters with defaults */
    data->decay_threshold = COMPOSITE_LRU_DEFAULT_DECAY_THRESHOLD;
    data->stability_count = COMPOSITE_LRU_DEFAULT_STABILITY_COUNT;
    data->migrate_hotness_threshold = COMPOSITE_LRU_DEFAULT_MIGRATE_THRESHOLD;
    data->overload_threshold = COMPOSITE_LRU_DEFAULT_OVERLOAD_THRESHOLD;
    data->bandwidth_threshold = COMPOSITE_LRU_DEFAULT_BANDWIDTH_THRESHOLD;
    data->pressure_threshold = COMPOSITE_LRU_DEFAULT_PRESSURE_THRESHOLD;
    
    /* Create heat map */
    data->key_heat_map = dictCreate(&heat_map_dict_type, NULL);
    if (!data->key_heat_map) {
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }
    
    /* Create pending migration queue */
    data->pending_migrations = listCreate();
    if (!data->pending_migrations) {
        dictRelease(data->key_heat_map);
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }
    listSetFreeMethod(data->pending_migrations, free_pending_migration);
    
    data->last_decay_time = get_current_time_us();
    
    strategy->private_data = data;
    
    CLRU_LOG(LL_NOTICE, "[Composite LRU] Strategy initialized (slot 1 default)");
    return NUMA_STRATEGY_OK;
}

/* Strategy execution */
int composite_lru_execute(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return NUMA_STRATEGY_ERR;
    
    composite_lru_data_t *data = strategy->private_data;
    uint64_t now = get_current_time_us();
    
    /* 1. Perform periodic heat decay */
    if (now - data->last_decay_time > data->decay_threshold) {
        composite_lru_decay_heat(data);
        data->last_decay_time = now;
    }
    
    /* 2. Process pending migrations */
    process_pending_migrations(data);
    
    /* 3. Check global load balancing */
    check_load_balancing(strategy);
    
    return NUMA_STRATEGY_OK;
}

/* Strategy cleanup */
void composite_lru_cleanup(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return;
    
    composite_lru_data_t *data = strategy->private_data;
    
    CLRU_LOG(LL_NOTICE, 
        "[Composite LRU] Cleanup - heat_updates=%llu, migrations=%llu, decays=%llu",
        (unsigned long long)data->heat_updates,
        (unsigned long long)data->migrations_triggered,
        (unsigned long long)data->decay_operations);
    
    if (data->key_heat_map) {
        dictRelease(data->key_heat_map);
    }
    
    if (data->pending_migrations) {
        listRelease(data->pending_migrations);
    }
    
    zfree(data);
    strategy->private_data = NULL;
}

/* Get strategy name */
static const char* composite_lru_get_name(numa_strategy_t *strategy) {
    (void)strategy;
    return "composite-lru";
}

/* Get strategy description */
static const char* composite_lru_get_description(numa_strategy_t *strategy) {
    (void)strategy;
    return "Slot 1 default: Stability-first composite LRU hotness management";
}

/* Set configuration */
static int composite_lru_set_config(numa_strategy_t *strategy, 
                                   const char *key, const char *value) {
    if (!strategy || !strategy->private_data || !key || !value) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    composite_lru_data_t *data = strategy->private_data;
    
    if (strcmp(key, "decay_threshold") == 0) {
        data->decay_threshold = (uint32_t)atoi(value) * 1000000;  /* seconds to us */
    } else if (strcmp(key, "stability_count") == 0) {
        data->stability_count = (uint8_t)atoi(value);
    } else if (strcmp(key, "migrate_threshold") == 0) {
        data->migrate_hotness_threshold = (uint8_t)atoi(value);
    } else if (strcmp(key, "overload_threshold") == 0) {
        data->overload_threshold = atof(value);
    } else if (strcmp(key, "bandwidth_threshold") == 0) {
        data->bandwidth_threshold = atof(value);
    } else if (strcmp(key, "pressure_threshold") == 0) {
        data->pressure_threshold = atof(value);
    } else {
        return NUMA_STRATEGY_EINVAL;
    }
    
    CLRU_LOG(LL_VERBOSE, "[Composite LRU] Config set: %s = %s", key, value);
    return NUMA_STRATEGY_OK;
}

/* Get configuration */
static int composite_lru_get_config(numa_strategy_t *strategy, 
                                   const char *key, char *buf, size_t buf_len) {
    if (!strategy || !strategy->private_data || !key || !buf || buf_len == 0) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    composite_lru_data_t *data = strategy->private_data;
    
    if (strcmp(key, "decay_threshold") == 0) {
        snprintf(buf, buf_len, "%u", data->decay_threshold / 1000000);
    } else if (strcmp(key, "stability_count") == 0) {
        snprintf(buf, buf_len, "%u", data->stability_count);
    } else if (strcmp(key, "migrate_threshold") == 0) {
        snprintf(buf, buf_len, "%u", data->migrate_hotness_threshold);
    } else if (strcmp(key, "overload_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->overload_threshold);
    } else if (strcmp(key, "bandwidth_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->bandwidth_threshold);
    } else if (strcmp(key, "pressure_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->pressure_threshold);
    } else if (strcmp(key, "heat_updates") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->heat_updates);
    } else if (strcmp(key, "migrations_triggered") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->migrations_triggered);
    } else if (strcmp(key, "decay_operations") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->decay_operations);
    } else {
        return NUMA_STRATEGY_EINVAL;
    }
    
    return NUMA_STRATEGY_OK;
}

/* Strategy vtable */
static const numa_strategy_vtable_t composite_lru_vtable = {
    .init = composite_lru_init,
    .execute = composite_lru_execute,
    .cleanup = composite_lru_cleanup,
    .get_name = composite_lru_get_name,
    .get_description = composite_lru_get_description,
    .set_config = composite_lru_set_config,
    .get_config = composite_lru_get_config
};

/* ========== Factory Functions ========== */

/* Create strategy instance */
numa_strategy_t* composite_lru_create(void) {
    numa_strategy_t *strategy = zmalloc(sizeof(*strategy));
    if (!strategy) return NULL;
    
    memset(strategy, 0, sizeof(*strategy));
    strategy->slot_id = 1;  /* Default to slot 1 */
    strategy->name = "composite-lru";
    strategy->description = "Stability-first composite LRU strategy (slot 1 default)";
    strategy->type = STRATEGY_TYPE_PERIODIC;
    strategy->priority = STRATEGY_PRIORITY_HIGH;
    strategy->enabled = 1;
    strategy->execute_interval_us = 1000000;  /* 1 second */
    strategy->vtable = &composite_lru_vtable;
    
    return strategy;
}

/* Destroy strategy instance */
void composite_lru_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    
    if (strategy->vtable && strategy->vtable->cleanup) {
        strategy->vtable->cleanup(strategy);
    }
    
    zfree(strategy);
}

/* Strategy factory */
static numa_strategy_factory_t composite_lru_factory = {
    .name = "composite-lru",
    .description = "Stability-first composite LRU hotness management (slot 1 default)",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,  /* 1 second */
    .create = composite_lru_create,
    .destroy = composite_lru_destroy
};

/* ========== Public Registration ========== */

/* Register strategy factory */
int numa_composite_lru_register(void) {
    return numa_strategy_register_factory(&composite_lru_factory);
}

/* ========== Statistics Query ========== */

void composite_lru_get_stats(numa_strategy_t *strategy, 
                             uint64_t *heat_updates,
                             uint64_t *migrations_triggered,
                             uint64_t *decay_operations) {
    if (!strategy || !strategy->private_data) return;
    
    composite_lru_data_t *data = strategy->private_data;
    
    if (heat_updates) *heat_updates = data->heat_updates;
    if (migrations_triggered) *migrations_triggered = data->migrations_triggered;
    if (decay_operations) *decay_operations = data->decay_operations;
}
