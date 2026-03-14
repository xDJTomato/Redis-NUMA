/*
 * NUMA复合LRU策略实现（插槽1默认策略）
 *
 * 这是NUMA迁移的默认策略，提供稳定性导向的热度管理
 * 和基于Redis原生LRU机制的智能迁移触发。
 */

#define _GNU_SOURCE  /* 用于sched_getcpu() */
#include "numa_composite_lru.h"
#include "zmalloc.h"
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <numa.h>

/* ========== 日志输出 ========== */

#ifdef NUMA_STRATEGY_STANDALONE
#define _serverLog(level, fmt, ...) printf("[%s] " fmt "\n", level, ##__VA_ARGS__)
#else
extern void _serverLog(int level, const char *fmt, ...);
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#endif

/* ========== 辅助函数 ========== */

/* 获取当前时间（微秒） */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* 获取LRU时钟（简化版） */
static uint16_t get_lru_clock(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    /* 返回秒数的低16位，类似Redis LRU_CLOCK */
    return (uint16_t)(tv.tv_sec & 0xFFFF);
}

/* 计算时间差，处理数字回绕 */
static uint16_t calculate_time_delta(uint16_t current, uint16_t last) {
    if (current >= last) {
        return current - last;
    } else {
        return (0xFFFF - last) + current + 1;
    }
}

/*
 * compute_lazy_decay_steps - 阶梯式惰性衰减查表
 *
 * 将空闲时间（自上次访问经过的秒数）映射为热度衰减量。
 * 在下次访问时应用，无需后台扫描。
 *
 *  elapsed < 10s   : 衰减0（短暂停顿，完全免疫）
 *  elapsed < 60s   : 衰减1
 *  elapsed < 5min  : 衰减2
 *  elapsed < 30min : 衰减3
 *  elapsed >= 30min: 全清除（衰减 = HOTNESS_MAX）
 */
static uint8_t compute_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < LAZY_DECAY_STEP1_SECS) return 0;
    if (elapsed_secs < LAZY_DECAY_STEP2_SECS) return 1;
    if (elapsed_secs < LAZY_DECAY_STEP3_SECS) return 2;
    if (elapsed_secs < LAZY_DECAY_STEP4_SECS) return 3;
    return COMPOSITE_LRU_HOTNESS_MAX; /* 长时间空闲的Key全清除 */
}

/* 获取当前NUMA节点 */
static int get_current_numa_node(void) {
    if (numa_available() < 0) return 0;
    return numa_node_of_cpu(sched_getcpu());
}

/* ========== 热度图字典回调函数 ========== */

/* 哈希函数（基于指针） */
static uint64_t heat_map_hash(const void *key) {
    return (uint64_t)(uintptr_t)key;
}

/* Key比较函数 */
static int heat_map_key_compare(void *privdata, const void *key1, const void *key2) {
    (void)privdata;
    return key1 == key2;
}

/* 値析构函数 */
static void heat_map_val_destructor(void *privdata, void *val) {
    (void)privdata;
    zfree(val);
}

/* 热度图字典类型定义 */
static dictType heat_map_dict_type = {
    .hashFunction = heat_map_hash,
    .keyDup = NULL,
    .valDup = NULL,
    .keyCompare = heat_map_key_compare,
    .keyDestructor = NULL,
    .valDestructor = heat_map_val_destructor
};

/* ========== 待迁移任务管理 ========== */

/* 释放待迁移条目 */
static void free_pending_migration(void *ptr) {
    zfree(ptr);
}

/* ========== 资源监控 ========== */

/* 检查目标节点的资源状态 */
static int check_resource_status(composite_lru_data_t *data, int node_id) {
    (void)data;
    (void)node_id;
    
    /* 简化版资源检查 - 生产环境中应查询实际指标 */
    /* 当前始终返回可用 */
    return RESOURCE_AVAILABLE;
}

/* ========== 热度管理 ========== */

/* 记录Key访问 - 使用PREFIX热度追踪 */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val) {
    if (!strategy || !strategy->private_data || !key) return;
    
    composite_lru_data_t *data = strategy->private_data;
    int current_node = get_current_numa_node();
    uint16_t current_time = get_lru_clock();
    
    /* 如果val是有效内存指针，则使用PREFIX热度追踪 */
    if (val) {
        uint8_t current_hotness = numa_get_hotness(val);
        int mem_node = numa_get_node_id(val);

        /* 阶梯式惰性衰减：结算自上次访问以来的衰减债 */
        uint16_t last_access = numa_get_last_access(val);
        uint16_t elapsed = calculate_time_delta(current_time, last_access);
        uint8_t decay = compute_lazy_decay_steps(elapsed);
        if (decay > 0) {
            uint8_t decayed = (decay >= current_hotness) ? 0 : (current_hotness - decay);
            if (decayed != current_hotness) {
                numa_set_hotness(val, decayed);
                data->decay_operations++;
                _serverLog(LL_VERBOSE,
                    "[Composite LRU] Lazy decay (PREFIX): val=%p, elapsed=%us, decay=%d, hotness %d->%d",
                    val, (unsigned)elapsed, decay, current_hotness, decayed);
                current_hotness = decayed;
            }
        }
        
        /* 任何访问（本地或远程）都递增热度 */
        if (current_hotness < COMPOSITE_LRU_HOTNESS_MAX) {
            numa_set_hotness(val, current_hotness + 1);
        }

        /* 远程访问：预判断热度达到阈値时触发迁移 */
        if (mem_node != current_node &&
            current_hotness >= data->migrate_hotness_threshold) {
            _serverLog(LL_VERBOSE,
                "[Composite LRU] Remote access detected: val=%p, current_node=%d, mem_node=%d, hotness=%d, threshold=%d",
                val, current_node, mem_node, current_hotness, data->migrate_hotness_threshold);

            /* 加入待迁移队列 */
            pending_migration_t *pm = zmalloc(sizeof(*pm));
            if (pm) {
                pm->key = key;
                pm->target_node = current_node;
                pm->enqueue_time = get_current_time_us();
                pm->priority = current_hotness;
                listAddNodeTail(data->pending_migrations, pm);
            }
        }
        
        /* 更新PREFIX中的访问统计 */
        numa_increment_access_count(val);
        numa_set_last_access(val, current_time);
        
        data->heat_updates++;
    } else {
        /* 回退到旧式字典追踪方式 */
        composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, key);
        
        if (!info) {
            /* 首次访问：创建热度记录 */
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
        
        /* 更新访问统计 - 在覆写前保存旧的last_access */
        info->access_count++;
        uint16_t old_last_access = info->last_access;
        info->last_access = current_time;
        data->heat_updates++;

        /* 阶梯式惰性衰减：结算自上次访问以来的衰减债 */
        uint16_t elapsed = calculate_time_delta(current_time, old_last_access);
        uint8_t decay = compute_lazy_decay_steps(elapsed);
        if (decay > 0) {
            uint8_t before = info->hotness;
            info->hotness = (decay >= info->hotness) ? 0 : (info->hotness - decay);
            if (info->hotness != before) {
                data->decay_operations++;
                _serverLog(LL_VERBOSE,
                    "[Composite LRU] Lazy decay (dict): key=%p, elapsed=%us, decay=%d, hotness %d->%d",
                    key, (unsigned)elapsed, decay, before, info->hotness);
            }
        }
        
        /* 任何访问（本地或远程）都递增热度 */
        if (info->hotness < COMPOSITE_LRU_HOTNESS_MAX) {
            info->hotness++;
        }
        info->stability_counter = 0;

        /* 远程访问：热度达到阈値时记录迁移候选 */
        if (info->current_node != current_node) {
            info->preferred_node = current_node;
            if (info->hotness >= data->migrate_hotness_threshold) {
                _serverLog(LL_VERBOSE,
                    "[Composite LRU] Remote access detected (legacy): key=%p, current_node=%d, accessed_from=%d, hotness=%d, threshold=%d",
                    key, info->current_node, current_node, info->hotness, data->migrate_hotness_threshold);
            }
        }
    }
}

/* 执行热度衰减 */
void composite_lru_decay_heat(composite_lru_data_t *data) {
    if (!data || !data->key_heat_map) return;
    
    dictIterator *di = dictGetSafeIterator(data->key_heat_map);
    dictEntry *de;
    uint16_t current_time = get_lru_clock();
    uint16_t decay_threshold_lru = (uint16_t)(data->decay_threshold / 1000000);  /* 转换为秒 */
    
    while ((de = dictNext(di)) != NULL) {
        composite_lru_heat_info_t *info = dictGetVal(de);
        
        uint16_t time_diff = calculate_time_delta(current_time, info->last_access);
        
        /* 稳定性衰减：需多次超阈値后才递减 */
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
            /* 最近有访问：重置稳定性计数器 */
            info->stability_counter = 0;
        }
    }
    
    dictReleaseIterator(di);
}

/* ========== 迁移处理 ========== */

/* 处理待迁移任务队列 */
static void process_pending_migrations(composite_lru_data_t *data) {
    if (!data || !data->pending_migrations) return;
    
    uint64_t now = get_current_time_us();
    listNode *node, *next;
    
    node = listFirst(data->pending_migrations);
    while (node) {
        next = listNextNode(node);
        pending_migration_t *pm = listNodeValue(node);
        
        /* 检查超时 */
        if (now - pm->enqueue_time > COMPOSITE_LRU_PENDING_TIMEOUT) {
            data->pending_timeouts++;
            listDelNode(data->pending_migrations, node);
            node = next;
            continue;
        }
        
        /* 检查资源是否就绪 */
        int status = check_resource_status(data, pm->target_node);
        if (status == RESOURCE_AVAILABLE) {
            /* 执行迁移 - 实际中将调用 numa_migrate_single_key */
            _serverLog(LL_VERBOSE, 
                "[Composite LRU] *** MIGRATION TRIGGERED *** key=%p, target_node=%d, priority=%d, pending_time=%lluus",
                pm->key, pm->target_node, pm->priority, (unsigned long long)(now - pm->enqueue_time));
            
            /* 迁移逻辑实现处 */
            data->migrations_triggered++;
            listDelNode(data->pending_migrations, node);
        }
        
        node = next;
    }
}

/* 检查负载均衡，如有必要则触发 */
static void check_load_balancing(numa_strategy_t *strategy) {
    (void)strategy;
    
    /* 生产环境中，应分析节点负载并触发再均衡 */
    /* 当前为占位符实现 */
}

/* ========== 策略虚函数表实现 ========== */

/* 策略初始化 */
int composite_lru_init(numa_strategy_t *strategy) {
    composite_lru_data_t *data = zmalloc(sizeof(*data));
    if (!data) return NUMA_STRATEGY_ERR;
    
    memset(data, 0, sizeof(*data));
    
    /* 使用默认参数进行初始化 */
    data->decay_threshold = COMPOSITE_LRU_DEFAULT_DECAY_THRESHOLD;
    data->stability_count = COMPOSITE_LRU_DEFAULT_STABILITY_COUNT;
    data->migrate_hotness_threshold = COMPOSITE_LRU_DEFAULT_MIGRATE_THRESHOLD;
    data->overload_threshold = COMPOSITE_LRU_DEFAULT_OVERLOAD_THRESHOLD;
    data->bandwidth_threshold = COMPOSITE_LRU_DEFAULT_BANDWIDTH_THRESHOLD;
    data->pressure_threshold = COMPOSITE_LRU_DEFAULT_PRESSURE_THRESHOLD;
    
    /* 创建热度图 */
    data->key_heat_map = dictCreate(&heat_map_dict_type, NULL);
    if (!data->key_heat_map) {
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }
    
    /* 创建待迁移队列 */
    data->pending_migrations = listCreate();
    if (!data->pending_migrations) {
        dictRelease(data->key_heat_map);
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }
    listSetFreeMethod(data->pending_migrations, free_pending_migration);
    
    data->last_decay_time = get_current_time_us();
    
    strategy->private_data = data;
    
    _serverLog(LL_NOTICE, "[Composite LRU] Strategy initialized: migrate_threshold=%d, decay_threshold=%lluus, stability_count=%d",
        data->migrate_hotness_threshold, data->decay_threshold, data->stability_count);
    return NUMA_STRATEGY_OK;
}

/* 策略执行 */
int composite_lru_execute(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return NUMA_STRATEGY_ERR;
    
    composite_lru_data_t *data = strategy->private_data;
    uint64_t now = get_current_time_us();
    
    /* 1. 执行周期性热度衰减 */
    if (now - data->last_decay_time > data->decay_threshold) {
        _serverLog(LL_VERBOSE, "[Composite LRU] Executing heat decay cycle");
        composite_lru_decay_heat(data);
        data->last_decay_time = now;
    }
    
    /* 2. 处理待迁移任务 */
    if (listLength(data->pending_migrations) > 0) {
        _serverLog(LL_VERBOSE, "[Composite LRU] Processing %lu pending migrations",
            (unsigned long)listLength(data->pending_migrations));
        process_pending_migrations(data);
    }
    
    /* 3. 检查全局负载均衡 */
    check_load_balancing(strategy);
    
    return NUMA_STRATEGY_OK;
}

/* 策略清理 */
void composite_lru_cleanup(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return;
    
    composite_lru_data_t *data = strategy->private_data;
    
    _serverLog(LL_NOTICE, 
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

/* 获取策略名称 */
static const char* composite_lru_get_name(numa_strategy_t *strategy) {
    (void)strategy;
    return "composite-lru";
}

/* 获取策略描述 */
static const char* composite_lru_get_description(numa_strategy_t *strategy) {
    (void)strategy;
    return "插槽1默认策略：稳定性优先的复合LRU热度管理";
}

/* 设置配置参数 */
static int composite_lru_set_config(numa_strategy_t *strategy, 
                                   const char *key, const char *value) {
    if (!strategy || !strategy->private_data || !key || !value) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    composite_lru_data_t *data = strategy->private_data;
    
    if (strcmp(key, "decay_threshold") == 0) {
        data->decay_threshold = (uint32_t)atoi(value) * 1000000;  /* 秒转微秒 */
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
    
    _serverLog(LL_VERBOSE, "[Composite LRU] Config set: %s = %s", key, value);
    return NUMA_STRATEGY_OK;
}

/* 获取配置参数 */
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

/* 策略虚函数表 */
static const numa_strategy_vtable_t composite_lru_vtable = {
    .init = composite_lru_init,
    .execute = composite_lru_execute,
    .cleanup = composite_lru_cleanup,
    .get_name = composite_lru_get_name,
    .get_description = composite_lru_get_description,
    .set_config = composite_lru_set_config,
    .get_config = composite_lru_get_config
};

/* ========== 工厂函数 ========== */

/* 创建策略实例 */
numa_strategy_t* composite_lru_create(void) {
    numa_strategy_t *strategy = zmalloc(sizeof(*strategy));
    if (!strategy) return NULL;
    
    memset(strategy, 0, sizeof(*strategy));
    strategy->slot_id = 1;  /* 默认使用插槽1 */
    strategy->name = "composite-lru";
    strategy->description = "Stability-first composite LRU strategy (slot 1 default)";
    strategy->type = STRATEGY_TYPE_PERIODIC;
    strategy->priority = STRATEGY_PRIORITY_HIGH;
    strategy->enabled = 1;
    strategy->execute_interval_us = 1000000;  /* 1秒 */
    strategy->vtable = &composite_lru_vtable;
    
    return strategy;
}

/* 销毁策略实例 */
void composite_lru_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    
    if (strategy->vtable && strategy->vtable->cleanup) {
        strategy->vtable->cleanup(strategy);
    }
    
    zfree(strategy);
}

/* 策略工厂 */
static numa_strategy_factory_t composite_lru_factory = {
    .name = "composite-lru",
    .description = "Stability-first composite LRU hotness management (slot 1 default)",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,  /* 1秒 */
    .create = composite_lru_create,
    .destroy = composite_lru_destroy
};

/* ========== 公共注册接口 ========== */

/* 注册策略工厂 */
int numa_composite_lru_register(void) {
    return numa_strategy_register_factory(&composite_lru_factory);
}

/* ========== 统计信息查询 ========== */

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
