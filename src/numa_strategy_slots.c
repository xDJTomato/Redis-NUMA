/*
 * NUMA策略插槽框架实现
 * 提供策略的注册、管理和调度功能
 */

#include "numa_strategy_slots.h"
#include "numa_composite_lru.h"
#include "zmalloc.h"
#include <string.h>
#include <sys/time.h>
#include <pthread.h>
#include <stdio.h>

/* 日志宏定义 */
#ifdef NUMA_STRATEGY_STANDALONE
#define STRATEGY_LOG(level, fmt, ...) printf("[%s] " fmt "\n", level, ##__VA_ARGS__)
#else
/* 前向声明，链接时由server.o提供 */
/* Redis 内部使用 _serverLog 作为实际函数名 */
extern void _serverLog(int level, const char *fmt, ...);
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define STRATEGY_LOG(level, fmt, ...) _serverLog(level, fmt, ##__VA_ARGS__)
#endif

/* ========== 全局管理器 ========== */

typedef struct {
    int initialized;                              /* 初始化标志 */
    numa_strategy_t *slots[NUMA_MAX_STRATEGY_SLOTS]; /* 插槽数组 */
    pthread_mutex_t lock;                         /* 线程安全锁 */
    
    /* 工厂注册表 */
    numa_strategy_factory_t *factories[NUMA_MAX_STRATEGY_SLOTS];
    int factory_count;
    
    /* 统计信息 */
    uint64_t total_runs;                          /* 总调度次数 */
    uint64_t total_strategy_executions;           /* 总策略执行次数 */
} numa_strategy_manager_t;

static numa_strategy_manager_t strategy_manager = {0};

/* ========== 辅助函数 ========== */

/* 获取当前时间(微秒) */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* 查找已注册的工厂 */
static numa_strategy_factory_t* find_factory(const char *name) {
    for (int i = 0; i < strategy_manager.factory_count; i++) {
        if (strcmp(strategy_manager.factories[i]->name, name) == 0) {
            return strategy_manager.factories[i];
        }
    }
    return NULL;
}

/* ========== 0号兜底策略实现 ========== */

/* 0号策略私有数据 */
typedef struct {
    uint64_t execution_count;      /* 执行计数 */
    uint64_t last_log_time;        /* 上次日志时间 */
} noop_strategy_data_t;

/* 0号策略初始化 */
static int noop_strategy_init(numa_strategy_t *strategy) {
    noop_strategy_data_t *data = zmalloc(sizeof(*data));
    if (!data) return NUMA_STRATEGY_ERR;
    
    data->execution_count = 0;
    data->last_log_time = 0;
    strategy->private_data = data;
    
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy Slot 0] No-op strategy initialized");
    return NUMA_STRATEGY_OK;
}

/* 0号策略执行 */
static int noop_strategy_execute(numa_strategy_t *strategy) {
    noop_strategy_data_t *data = strategy->private_data;
    uint64_t now = get_current_time_us();
    
    data->execution_count++;
    
    /* 每10秒打印一次日志，避免日志过多 */
    if (now - data->last_log_time > 10000000) {  /* 10秒 */
        STRATEGY_LOG(LL_VERBOSE, 
                  "[NUMA Strategy Slot 0] No-op strategy executed (count: %llu)",
                  (unsigned long long)data->execution_count);
        data->last_log_time = now;
    }
    
    return NUMA_STRATEGY_OK;
}

/* 0号策略清理 */
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

/* 0号策略信息获取 */
static const char* noop_strategy_get_name(numa_strategy_t *strategy) {
    (void)strategy;
    return "noop";
}

static const char* noop_strategy_get_description(numa_strategy_t *strategy) {
    (void)strategy;
    return "Slot 0 no-operation fallback strategy for framework verification";
}

/* 0号策略配置（暂不支持） */
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

/* 0号策略虚函数表 */
static const numa_strategy_vtable_t noop_strategy_vtable = {
    .init = noop_strategy_init,
    .execute = noop_strategy_execute,
    .cleanup = noop_strategy_cleanup,
    .get_name = noop_strategy_get_name,
    .get_description = noop_strategy_get_description,
    .set_config = noop_strategy_set_config,
    .get_config = noop_strategy_get_config
};

/* 0号策略创建 */
static numa_strategy_t* noop_strategy_create(void) {
    numa_strategy_t *strategy = zmalloc(sizeof(*strategy));
    if (!strategy) return NULL;
    
    memset(strategy, 0, sizeof(*strategy));
    strategy->slot_id = 0;
    strategy->name = "noop";
    strategy->description = "Slot 0 no-operation fallback strategy";
    strategy->type = STRATEGY_TYPE_PERIODIC;
    strategy->priority = STRATEGY_PRIORITY_LOW;
    strategy->enabled = 1;  /* 默认启用 */
    strategy->execute_interval_us = 1000000;  /* 1秒执行间隔 */
    strategy->vtable = &noop_strategy_vtable;
    
    return strategy;
}

/* 0号策略销毁 */
static void noop_strategy_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    if (strategy->vtable && strategy->vtable->cleanup) {
        strategy->vtable->cleanup(strategy);
    }
    zfree(strategy);
}

/* 0号策略工厂 */
static numa_strategy_factory_t noop_strategy_factory = {
    .name = "noop",
    .description = "No-operation fallback strategy",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_LOW,
    .default_interval_us = 1000000,
    .create = noop_strategy_create,
    .destroy = noop_strategy_destroy
};

/* 注册0号策略 */
int numa_strategy_register_noop(void) {
    return numa_strategy_register_factory(&noop_strategy_factory);
}

/* 注册1号策略（转发到composite_lru模块） */
int numa_strategy_register_composite_lru(void) {
    return numa_composite_lru_register();
}

/* ========== 策略管理器实现 ========== */

/* 初始化策略管理器 */
int numa_strategy_init(void) {
    if (strategy_manager.initialized) {
        return NUMA_STRATEGY_OK;
    }
    
    memset(&strategy_manager, 0, sizeof(strategy_manager));
    pthread_mutex_init(&strategy_manager.lock, NULL);
    
    /* 注册内置的0号策略 */
    if (numa_strategy_register_noop() != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to register no-op strategy");
        return NUMA_STRATEGY_ERR;
    }
    
    /* 自动创建并插入0号策略到slot 0 */
    if (numa_strategy_slot_insert(0, "noop") != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to insert no-op strategy to slot 0");
        return NUMA_STRATEGY_ERR;
    }
    
    /* 注册内置的1号策略（Composite LRU） */
    if (numa_strategy_register_composite_lru() != NUMA_STRATEGY_OK) {
        STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to register composite-lru strategy");
        /* 1号策略注册失败不影响框架初始化 */
    } else {
        /* 自动创建并插入1号策略到slot 1 */
        if (numa_strategy_slot_insert(1, "composite-lru") != NUMA_STRATEGY_OK) {
            STRATEGY_LOG(LL_WARNING, "[NUMA Strategy] Failed to insert composite-lru to slot 1");
        } else {
            STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Composite LRU strategy inserted to slot 1");
        }
    }
    
    strategy_manager.initialized = 1;
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Strategy slot framework initialized (slots 0,1 ready)");
    
    return NUMA_STRATEGY_OK;
}

/* 清理策略管理器 */
void numa_strategy_cleanup(void) {
    if (!strategy_manager.initialized) return;
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    /* 清理所有插槽 */
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

/* 注册策略工厂 */
int numa_strategy_register_factory(const numa_strategy_factory_t *factory) {
    if (!factory || !factory->name || !factory->create || !factory->destroy) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    /* 检查是否已存在 */
    if (find_factory(factory->name) != NULL) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_EEXIST;
    }
    
    /* 检查容量 */
    if (strategy_manager.factory_count >= NUMA_MAX_STRATEGY_SLOTS) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_ERR;
    }
    
    /* 注册工厂 */
    strategy_manager.factories[strategy_manager.factory_count++] = 
        (numa_strategy_factory_t*)factory;
    
    pthread_mutex_unlock(&strategy_manager.lock);
    
    STRATEGY_LOG(LL_VERBOSE, "[NUMA Strategy] Registered strategy factory: %s", factory->name);
    return NUMA_STRATEGY_OK;
}

/* 创建策略实例 */
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

/* 销毁策略实例 */
void numa_strategy_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    numa_strategy_factory_t *factory = find_factory(strategy->name);
    if (factory && factory->destroy) {
        factory->destroy(strategy);
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
}

/* 插入策略到插槽 */
int numa_strategy_slot_insert(int slot_id, const char *strategy_name) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS || !strategy_name) {
        return NUMA_STRATEGY_EINVAL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    
    /* 检查插槽是否已被占用 */
    if (strategy_manager.slots[slot_id] != NULL) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_EEXIST;
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
    
    /* 创建策略实例 */
    numa_strategy_t *strategy = numa_strategy_create(strategy_name);
    if (!strategy) {
        return NUMA_STRATEGY_ENOENT;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    strategy->slot_id = slot_id;
    strategy_manager.slots[slot_id] = strategy;
    pthread_mutex_unlock(&strategy_manager.lock);
    
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Inserted strategy '%s' to slot %d", 
              strategy_name, slot_id);
    
    return NUMA_STRATEGY_OK;
}

/* 移除插槽中的策略 */
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
    
    strategy_manager.slots[slot_id] = NULL;
    pthread_mutex_unlock(&strategy_manager.lock);
    
    numa_strategy_destroy(strategy);
    STRATEGY_LOG(LL_NOTICE, "[NUMA Strategy] Removed strategy from slot %d", slot_id);
    
    return NUMA_STRATEGY_OK;
}

/* 启用插槽 */
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
    pthread_mutex_unlock(&strategy_manager.lock);
    
    STRATEGY_LOG(LL_VERBOSE, "[NUMA Strategy] Enabled slot %d", slot_id);
    return NUMA_STRATEGY_OK;
}

/* 禁用插槽 */
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
    pthread_mutex_unlock(&strategy_manager.lock);
    
    STRATEGY_LOG(LL_VERBOSE, "[NUMA Strategy] Disabled slot %d", slot_id);
    return NUMA_STRATEGY_OK;
}

/* 配置插槽 */
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

/* 获取插槽策略 */
numa_strategy_t* numa_strategy_slot_get(int slot_id) {
    if (slot_id < 0 || slot_id >= NUMA_MAX_STRATEGY_SLOTS) {
        return NULL;
    }
    
    pthread_mutex_lock(&strategy_manager.lock);
    numa_strategy_t *strategy = strategy_manager.slots[slot_id];
    pthread_mutex_unlock(&strategy_manager.lock);
    
    return strategy;
}

/* 列出所有插槽状态 */
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

/* 获取插槽状态 */
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
             "  Executions: %llu\n"
             "  Failures: %llu\n"
             "  Total time: %llu us\n",
             slot_id, strategy->name,
             strategy->description,
             strategy->enabled ? "enabled" : "disabled",
             (unsigned long long)strategy->total_executions,
             (unsigned long long)strategy->total_failures,
             (unsigned long long)strategy->total_execution_time_us);
    
    pthread_mutex_unlock(&strategy_manager.lock);
    return NUMA_STRATEGY_OK;
}

/* 执行指定插槽策略 */
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
    
    /* 检查执行间隔 */
    if (now - strategy->last_execute_time < strategy->execute_interval_us) {
        pthread_mutex_unlock(&strategy_manager.lock);
        return NUMA_STRATEGY_OK;
    }
    
    pthread_mutex_unlock(&strategy_manager.lock);
    
    /* 执行策略 */
    uint64_t start_time = get_current_time_us();
    int result = NUMA_STRATEGY_OK;
    
    if (strategy->vtable && strategy->vtable->execute) {
        result = strategy->vtable->execute(strategy);
    }
    
    uint64_t elapsed = get_current_time_us() - start_time;
    
    /* 更新统计 */
    pthread_mutex_lock(&strategy_manager.lock);
    strategy->last_execute_time = now;
    strategy->total_executions++;
    strategy->total_execution_time_us += elapsed;
    if (result != NUMA_STRATEGY_OK) {
        strategy->total_failures++;
    }
    pthread_mutex_unlock(&strategy_manager.lock);
    
    return result;
}

/* 执行所有启用的策略 */
void numa_strategy_run_all(void) {
    if (!strategy_manager.initialized) return;
    
    strategy_manager.total_runs++;
    
    /* 按优先级执行：HIGH -> NORMAL -> LOW */
    for (int priority = (int)STRATEGY_PRIORITY_HIGH; 
         priority >= (int)STRATEGY_PRIORITY_LOW; 
         priority--) {
        
        for (int slot_id = 0; slot_id < NUMA_MAX_STRATEGY_SLOTS; slot_id++) {
            pthread_mutex_lock(&strategy_manager.lock);
            numa_strategy_t *strategy = strategy_manager.slots[slot_id];
            
            if (strategy && strategy->enabled && strategy->priority == priority) {
                pthread_mutex_unlock(&strategy_manager.lock);
                numa_strategy_run_slot(slot_id);
                strategy_manager.total_strategy_executions++;
            } else {
                pthread_mutex_unlock(&strategy_manager.lock);
            }
        }
    }
}
