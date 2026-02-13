/* numa_key_migrate.c - NUMA Key-level migration implementation
 *
 * Implements Redis Key-level migration with LRU-integrated hotness tracking.
 */

#define _GNU_SOURCE
#include "numa_key_migrate.h"
#include "numa_migrate.h"
#include "zmalloc.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "quicklist.h"
#include "intset.h"
#include "ziplist.h"
#include <string.h>
#include <stdio.h>
#include <numa.h>
#include <sys/time.h>

/* External Redis functions */
extern void _serverLog(int level, const char *fmt, ...);
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_DEBUG 0
#define KEY_MIGRATE_LOG(level, fmt, ...) _serverLog(level, fmt, ##__VA_ARGS__)

/* Global context */
static numa_key_migrate_ctx_t global_ctx = {0};

/* ========== Helper Functions ========== */

/* Get current time in microseconds */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Calculate time delta considering LRU clock wraparound */
static uint16_t calculate_time_delta(uint16_t current, uint16_t last) {
    if (current >= last) {
        return current - last;
    } else {
        /* Wraparound case */
        return (0xFFFF - last) + current + 1;
    }
}

/* Get current NUMA node of calling thread */
static int get_current_numa_node(void) {
    int cpu = sched_getcpu();
    if (cpu < 0) return 0;
    return numa_node_of_cpu(cpu);
}

/* ========== Metadata Management ========== */

/* Hash function for robj pointers */
static uint64_t key_obj_hash(const void *key) {
    return dictGenHashFunction(key, sizeof(void*));
}

/* Key comparison for robj pointers */
static int key_obj_compare(void *privdata, const void *key1, const void *key2) {
    (void)privdata;
    return key1 == key2 ? 0 : 1;
}

/* Metadata destructor */
static void metadata_destructor(void *privdata, void *val) {
    (void)privdata;
    zfree(val);
}

/* Dictionary type for key metadata */
static dictType keyMetadataDictType = {
    key_obj_hash,           /* hash function */
    NULL,                   /* key dup */
    NULL,                   /* val dup */
    key_obj_compare,        /* key compare */
    NULL,                   /* key destructor */
    metadata_destructor     /* val destructor */
};

/* Create metadata for a key */
static key_numa_metadata_t* create_key_metadata(robj *key, robj *val) {
    key_numa_metadata_t *meta = zmalloc(sizeof(*meta));
    if (!meta) return NULL;
    
    meta->current_node = 0;  /* Default node 0 */
    meta->hotness_level = HOTNESS_DEFAULT;
    meta->last_access_time = LRU_CLOCK() & 0xFFFF;
    meta->memory_footprint = 0;  /* Will be updated */
    meta->access_count = 1;
    
    return meta;
}

/* Get or create key metadata */
static key_numa_metadata_t* get_or_create_metadata(robj *key, robj *val) {
    pthread_mutex_lock(&global_ctx.mutex);
    
    dictEntry *entry = dictFind(global_ctx.key_metadata, key);
    key_numa_metadata_t *meta;
    
    if (entry) {
        meta = dictGetVal(entry);
    } else {
        meta = create_key_metadata(key, val);
        if (meta) {
            if (dictAdd(global_ctx.key_metadata, key, meta) != DICT_OK) {
                zfree(meta);
                meta = NULL;
            }
        }
    }
    
    pthread_mutex_unlock(&global_ctx.mutex);
    return meta;
}

/* ========== Module Initialization ========== */

int numa_key_migrate_init(void) {
    if (global_ctx.initialized) {
        return NUMA_KEY_MIGRATE_OK;
    }
    
    if (numa_available() == -1) {
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] NUMA not available");
        return NUMA_KEY_MIGRATE_ERR;
    }
    
    /* Initialize metadata dictionary */
    global_ctx.key_metadata = dictCreate(&keyMetadataDictType, NULL);
    if (!global_ctx.key_metadata) {
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] Failed to create metadata dict");
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* Initialize mutex */
    if (pthread_mutex_init(&global_ctx.mutex, NULL) != 0) {
        dictRelease(global_ctx.key_metadata);
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] Failed to initialize mutex");
        return NUMA_KEY_MIGRATE_ERR;
    }
    
    /* Initialize statistics */
    memset(&global_ctx.stats, 0, sizeof(global_ctx.stats));
    
    global_ctx.initialized = 1;
    KEY_MIGRATE_LOG(LL_NOTICE, "[NUMA Key Migrate] Module initialized successfully");
    
    return NUMA_KEY_MIGRATE_OK;
}

void numa_key_migrate_cleanup(void) {
    if (!global_ctx.initialized) {
        return;
    }
    
    pthread_mutex_lock(&global_ctx.mutex);
    
    if (global_ctx.key_metadata) {
        dictRelease(global_ctx.key_metadata);
        global_ctx.key_metadata = NULL;
    }
    
    pthread_mutex_unlock(&global_ctx.mutex);
    pthread_mutex_destroy(&global_ctx.mutex);
    
    global_ctx.initialized = 0;
    KEY_MIGRATE_LOG(LL_NOTICE, "[NUMA Key Migrate] Module cleanup completed");
}

/* ========== Hotness Tracking ========== */

void numa_record_key_access(robj *key, robj *val) {
    if (!global_ctx.initialized || !key || !val) {
        return;
    }
    
    key_numa_metadata_t *meta = get_or_create_metadata(key, val);
    if (!meta) {
        return;
    }
    
    int current_cpu_node = get_current_numa_node();
    uint16_t current_timestamp = LRU_CLOCK() & 0xFFFF;
    
    pthread_mutex_lock(&global_ctx.mutex);
    
    /* Update access statistics */
    meta->access_count++;
    meta->last_access_time = current_timestamp;
    
    /* Node affinity analysis */
    if (meta->current_node == current_cpu_node) {
        /* Local access: increase hotness */
        if (meta->hotness_level < HOTNESS_MAX_LEVEL) {
            meta->hotness_level++;
        }
    } else {
        /* Remote access: potential migration candidate */
        if (meta->hotness_level >= MIGRATION_HOTNESS_THRESHOLD) {
            /* TODO: Schedule migration evaluation */
            KEY_MIGRATE_LOG(LL_DEBUG, 
                "[NUMA Key Migrate] Hot key accessed remotely (hotness: %d)",
                meta->hotness_level);
        }
    }
    
    pthread_mutex_unlock(&global_ctx.mutex);
}

void numa_perform_heat_decay(void) {
    if (!global_ctx.initialized) {
        return;
    }
    
    pthread_mutex_lock(&global_ctx.mutex);
    
    dictIterator *iter = dictGetIterator(global_ctx.key_metadata);
    dictEntry *entry;
    uint16_t current_time = LRU_CLOCK() & 0xFFFF;
    
    while ((entry = dictNext(iter)) != NULL) {
        key_numa_metadata_t *meta = dictGetVal(entry);
        uint16_t time_delta = calculate_time_delta(current_time, meta->last_access_time);
        
        /* Decay hotness if not accessed for a while */
        if (time_delta > HEAT_DECAY_THRESHOLD) {
            if (meta->hotness_level > 0) {
                meta->hotness_level--;
            }
            meta->last_access_time = current_time;
        }
    }
    
    dictReleaseIterator(iter);
    pthread_mutex_unlock(&global_ctx.mutex);
}

/* ========== Type-specific Migration Adapters ========== */

/* Migrate STRING type */
int migrate_string_type(robj *key_obj, robj *val_obj, int target_node) {
    if (val_obj->encoding != OBJ_ENCODING_RAW && 
        val_obj->encoding != OBJ_ENCODING_EMBSTR) {
        /* Integer encoding, no need to migrate */
        return NUMA_KEY_MIGRATE_OK;
    }
    
    sds old_str = val_obj->ptr;
    size_t len = sdslen(old_str);
    
    /* Allocate new sds on target node */
    sds new_str = numa_zmalloc_onnode(len + 1 + sizeof(struct sdshdr8), target_node);
    if (!new_str) {
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* Copy string data */
    memcpy(new_str, old_str, len + 1 + sizeof(struct sdshdr8));
    
    /* Update pointer */
    val_obj->ptr = new_str;
    
    /* Free old memory */
    zfree(old_str);
    
    return NUMA_KEY_MIGRATE_OK;
}

/* Migrate HASH type */
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node) {
    /* TODO: Implement hash migration based on encoding */
    KEY_MIGRATE_LOG(LL_DEBUG, "[NUMA Key Migrate] Hash migration not yet implemented");
    return NUMA_KEY_MIGRATE_ETYPE;
}

/* Migrate LIST type */
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node) {
    /* TODO: Implement list migration */
    KEY_MIGRATE_LOG(LL_DEBUG, "[NUMA Key Migrate] List migration not yet implemented");
    return NUMA_KEY_MIGRATE_ETYPE;
}

/* Migrate SET type */
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node) {
    /* TODO: Implement set migration */
    KEY_MIGRATE_LOG(LL_DEBUG, "[NUMA Key Migrate] Set migration not yet implemented");
    return NUMA_KEY_MIGRATE_ETYPE;
}

/* Migrate ZSET type */
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node) {
    /* TODO: Implement zset migration */
    KEY_MIGRATE_LOG(LL_DEBUG, "[NUMA Key Migrate] Zset migration not yet implemented");
    return NUMA_KEY_MIGRATE_ETYPE;
}

/* ========== Migration Execution ========== */

int numa_migrate_single_key(redisDb *db, robj *key, int target_node) {
    if (!global_ctx.initialized || !db || !key) {
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    if (target_node < 0 || target_node > numa_max_node()) {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Invalid target node %d", target_node);
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    /* Lookup key in database */
    dictEntry *de = dictFind(db->dict, key->ptr);
    if (!de) {
        return NUMA_KEY_MIGRATE_ENOENT;
    }
    
    robj *val = dictGetVal(de);
    if (!val) {
        return NUMA_KEY_MIGRATE_ENOENT;
    }
    
    uint64_t start_time = get_current_time_us();
    int result = NUMA_KEY_MIGRATE_OK;
    
    /* Type-specific migration */
    switch (val->type) {
        case OBJ_STRING:
            result = migrate_string_type(key, val, target_node);
            break;
        case OBJ_HASH:
            result = migrate_hash_type(key, val, target_node);
            break;
        case OBJ_LIST:
            result = migrate_list_type(key, val, target_node);
            break;
        case OBJ_SET:
            result = migrate_set_type(key, val, target_node);
            break;
        case OBJ_ZSET:
            result = migrate_zset_type(key, val, target_node);
            break;
        default:
            KEY_MIGRATE_LOG(LL_WARNING, 
                "[NUMA Key Migrate] Unsupported type %d", val->type);
            result = NUMA_KEY_MIGRATE_ETYPE;
    }
    
    /* Update statistics */
    pthread_mutex_lock(&global_ctx.mutex);
    
    global_ctx.stats.total_migrations++;
    if (result == NUMA_KEY_MIGRATE_OK) {
        global_ctx.stats.successful_migrations++;
        
        /* Update key metadata */
        key_numa_metadata_t *meta = numa_get_key_metadata(key);
        if (meta) {
            meta->current_node = target_node;
        }
    } else {
        global_ctx.stats.failed_migrations++;
    }
    
    global_ctx.stats.total_migration_time_us += (get_current_time_us() - start_time);
    
    pthread_mutex_unlock(&global_ctx.mutex);
    
    return result;
}

int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node) {
    if (!global_ctx.initialized || !db || !key_list) {
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    listIter *iter = listGetIterator(key_list, AL_START_HEAD);
    listNode *node;
    int success_count = 0;
    int fail_count = 0;
    
    while ((node = listNext(iter)) != NULL) {
        robj *key = listNodeValue(node);
        int result = numa_migrate_single_key(db, key, target_node);
        
        if (result == NUMA_KEY_MIGRATE_OK) {
            success_count++;
        } else {
            fail_count++;
        }
    }
    
    listReleaseIterator(iter);
    
    KEY_MIGRATE_LOG(LL_VERBOSE, 
        "[NUMA Key Migrate] Batch migration: %d succeeded, %d failed",
        success_count, fail_count);
    
    return success_count > 0 ? NUMA_KEY_MIGRATE_OK : NUMA_KEY_MIGRATE_ERR;
}

int numa_migrate_keys_by_pattern(redisDb *db, const char *pattern, int target_node) {
    /* TODO: Implement pattern-based migration */
    KEY_MIGRATE_LOG(LL_DEBUG, 
        "[NUMA Key Migrate] Pattern-based migration not yet implemented");
    return NUMA_KEY_MIGRATE_ETYPE;
}

int numa_migrate_entire_database(redisDb *db, int target_node) {
    if (!global_ctx.initialized || !db) {
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    dictIterator *iter = dictGetIterator(db->dict);
    dictEntry *entry;
    int success_count = 0;
    int fail_count = 0;
    
    while ((entry = dictNext(iter)) != NULL) {
        robj *key = dictGetKey(entry);
        int result = numa_migrate_single_key(db, key, target_node);
        
        if (result == NUMA_KEY_MIGRATE_OK) {
            success_count++;
        } else {
            fail_count++;
        }
    }
    
    dictReleaseIterator(iter);
    
    KEY_MIGRATE_LOG(LL_NOTICE, 
        "[NUMA Key Migrate] Database migration: %d succeeded, %d failed",
        success_count, fail_count);
    
    return success_count > 0 ? NUMA_KEY_MIGRATE_OK : NUMA_KEY_MIGRATE_ERR;
}

/* ========== Query Interfaces ========== */

key_numa_metadata_t* numa_get_key_metadata(robj *key) {
    if (!global_ctx.initialized || !key) {
        return NULL;
    }
    
    pthread_mutex_lock(&global_ctx.mutex);
    dictEntry *entry = dictFind(global_ctx.key_metadata, key);
    key_numa_metadata_t *meta = entry ? dictGetVal(entry) : NULL;
    pthread_mutex_unlock(&global_ctx.mutex);
    
    return meta;
}

int numa_get_key_current_node(robj *key) {
    key_numa_metadata_t *meta = numa_get_key_metadata(key);
    return meta ? meta->current_node : -1;
}

void numa_get_migration_statistics(numa_key_migrate_stats_t *stats) {
    if (!stats || !global_ctx.initialized) {
        return;
    }
    
    pthread_mutex_lock(&global_ctx.mutex);
    *stats = global_ctx.stats;
    pthread_mutex_unlock(&global_ctx.mutex);
}

void numa_reset_migration_statistics(void) {
    if (!global_ctx.initialized) {
        return;
    }
    
    pthread_mutex_lock(&global_ctx.mutex);
    memset(&global_ctx.stats, 0, sizeof(global_ctx.stats));
    pthread_mutex_unlock(&global_ctx.mutex);
    
    KEY_MIGRATE_LOG(LL_VERBOSE, "[NUMA Key Migrate] Statistics reset");
}
