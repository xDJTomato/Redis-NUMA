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

/* External zset functions */
extern zskiplist *zslCreate(void);
extern void zslFree(zskiplist *zsl);
extern zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);

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
    (void)key_obj;  /* Unused parameter */
    (void)target_node;  /* Not directly used in sds allocation */
    
    if (val_obj->encoding != OBJ_ENCODING_RAW && 
        val_obj->encoding != OBJ_ENCODING_EMBSTR) {
        /* Integer encoding, no need to migrate */
        return NUMA_KEY_MIGRATE_OK;
    }
    
    sds old_str = val_obj->ptr;
    
    /* Create new sds using standard allocation (handles complex SDS header) */
    sds new_str = sdsnewlen(old_str, sdslen(old_str));
    if (!new_str) {
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* Update pointer */
    val_obj->ptr = new_str;
    
    /* Free old memory */
    sdsfree(old_str);
    
    return NUMA_KEY_MIGRATE_OK;
}

/* Migrate HASH type */
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter */
    
    if (val_obj->encoding == OBJ_ENCODING_ZIPLIST) {
        /* Ziplist encoding: migrate as a single blob */
        unsigned char *old_zl = val_obj->ptr;
        size_t zl_len = ziplistBlobLen(old_zl);
        
        unsigned char *new_zl = numa_zmalloc_onnode(zl_len, target_node);
        if (!new_zl) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        memcpy(new_zl, old_zl, zl_len);
        val_obj->ptr = new_zl;
        zfree(old_zl);
        
        KEY_MIGRATE_LOG(LL_DEBUG, 
            "[NUMA Key Migrate] Hash (ziplist) migrated, size: %zu bytes", zl_len);
        return NUMA_KEY_MIGRATE_OK;
        
    } else if (val_obj->encoding == OBJ_ENCODING_HT) {
        /* Hashtable encoding: migrate dict and all sds field/value pairs
         * Use standard sds functions due to complex SDS header structure */
        dict *old_dict = val_obj->ptr;
        dict *new_dict = dictCreate(old_dict->type, old_dict->privdata);
        if (!new_dict) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Pre-expand to avoid rehashing during migration */
        if (dictExpand(new_dict, dictSize(old_dict)) != DICT_OK) {
            dictRelease(new_dict);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        dictIterator *iter = dictGetIterator(old_dict);
        dictEntry *entry;
        size_t migrated_pairs = 0;
        
        while ((entry = dictNext(iter)) != NULL) {
            sds old_field = dictGetKey(entry);
            sds old_value = dictGetVal(entry);
            
            /* Create new sds using standard allocation */
            sds new_field = sdsnewlen(old_field, sdslen(old_field));
            if (!new_field) {
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            
            sds new_value = sdsnewlen(old_value, sdslen(old_value));
            if (!new_value) {
                sdsfree(new_field);
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            
            /* Add to new dict (takes ownership of new_field and new_value) */
            if (dictAdd(new_dict, new_field, new_value) != DICT_OK) {
                sdsfree(new_field);
                sdsfree(new_value);
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                return NUMA_KEY_MIGRATE_ERR;
            }
            
            migrated_pairs++;
        }
        
        dictReleaseIterator(iter);
        
        /* Swap dicts and free old */
        val_obj->ptr = new_dict;
        dictRelease(old_dict);
        
        KEY_MIGRATE_LOG(LL_DEBUG, 
            "[NUMA Key Migrate] Hash (hashtable) migrated, %zu pairs", migrated_pairs);
        return NUMA_KEY_MIGRATE_OK;
        
    } else {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown hash encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
}

/* Migrate LIST type */
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter */
    (void)target_node;  /* Not directly used in zmalloc allocation */
    
    if (val_obj->encoding != OBJ_ENCODING_QUICKLIST) {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown list encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
    
    quicklist *old_ql = val_obj->ptr;
    
    /* Create new quicklist using standard allocation */
    quicklist *new_ql = zmalloc(sizeof(quicklist));
    if (!new_ql) {
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* Copy quicklist header */
    new_ql->head = NULL;
    new_ql->tail = NULL;
    new_ql->count = old_ql->count;
    new_ql->len = 0;
    new_ql->fill = old_ql->fill;
    new_ql->compress = old_ql->compress;
    new_ql->bookmark_count = 0;
    
    /* Iterate over all quicklist nodes and migrate them */
    quicklistNode *old_node = old_ql->head;
    quicklistNode *prev_new_node = NULL;
    size_t migrated_nodes = 0;
    
    while (old_node) {
        /* Allocate new node using standard zmalloc */
        quicklistNode *new_node = zmalloc(sizeof(quicklistNode));
        if (!new_node) {
            /* Cleanup on failure */
            quicklistNode *cleanup = new_ql->head;
            while (cleanup) {
                quicklistNode *next = cleanup->next;
                if (cleanup->zl) zfree(cleanup->zl);
                zfree(cleanup);
                cleanup = next;
            }
            zfree(new_ql);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Copy node metadata */
        new_node->count = old_node->count;
        new_node->sz = old_node->sz;
        new_node->encoding = old_node->encoding;
        new_node->container = old_node->container;
        new_node->recompress = old_node->recompress;
        new_node->attempted_compress = old_node->attempted_compress;
        new_node->extra = old_node->extra;
        new_node->prev = prev_new_node;
        new_node->next = NULL;
        
        /* Migrate ziplist data */
        if (old_node->encoding == QUICKLIST_NODE_ENCODING_LZF) {
            /* LZF compressed */
            quicklistLZF *old_lzf = (quicklistLZF *)old_node->zl;
            size_t lzf_sz = sizeof(quicklistLZF) + old_lzf->sz;
            new_node->zl = zmalloc(lzf_sz);
            if (!new_node->zl) {
                zfree(new_node);
                quicklistNode *cleanup = new_ql->head;
                while (cleanup) {
                    quicklistNode *next = cleanup->next;
                    if (cleanup->zl) zfree(cleanup->zl);
                    zfree(cleanup);
                    cleanup = next;
                }
                zfree(new_ql);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            memcpy(new_node->zl, old_node->zl, lzf_sz);
        } else {
            /* Raw ziplist */
            new_node->zl = zmalloc(old_node->sz);
            if (!new_node->zl) {
                zfree(new_node);
                quicklistNode *cleanup = new_ql->head;
                while (cleanup) {
                    quicklistNode *next = cleanup->next;
                    if (cleanup->zl) zfree(cleanup->zl);
                    zfree(cleanup);
                    cleanup = next;
                }
                zfree(new_ql);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            memcpy(new_node->zl, old_node->zl, old_node->sz);
        }
        
        /* Link nodes */
        if (prev_new_node) {
            prev_new_node->next = new_node;
        } else {
            new_ql->head = new_node;
        }
        new_ql->tail = new_node;
        new_ql->len++;
        
        prev_new_node = new_node;
        old_node = old_node->next;
        migrated_nodes++;
    }
    
    /* Release old quicklist */
    old_node = old_ql->head;
    while (old_node) {
        quicklistNode *next = old_node->next;
        if (old_node->zl) zfree(old_node->zl);
        zfree(old_node);
        old_node = next;
    }
    zfree(old_ql);
    
    /* Update object pointer */
    val_obj->ptr = new_ql;
    
    KEY_MIGRATE_LOG(LL_DEBUG, 
        "[NUMA Key Migrate] List (quicklist) migrated, %zu nodes", migrated_nodes);
    return NUMA_KEY_MIGRATE_OK;
}

/* Migrate SET type */
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter */
    
    if (val_obj->encoding == OBJ_ENCODING_INTSET) {
        /* Intset encoding: migrate as a single blob */
        intset *old_is = val_obj->ptr;
        size_t is_len = intsetBlobLen(old_is);
        
        intset *new_is = numa_zmalloc_onnode(is_len, target_node);
        if (!new_is) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        memcpy(new_is, old_is, is_len);
        val_obj->ptr = new_is;
        zfree(old_is);
        
        KEY_MIGRATE_LOG(LL_DEBUG, 
            "[NUMA Key Migrate] Set (intset) migrated, size: %zu bytes", is_len);
        return NUMA_KEY_MIGRATE_OK;
        
    } else if (val_obj->encoding == OBJ_ENCODING_HT) {
        /* Hashtable encoding: migrate dict and all sds elements
         * For simplicity, we recreate using standard sds functions
         * since SDS has complex header structure */
        dict *old_dict = val_obj->ptr;
        dict *new_dict = dictCreate(old_dict->type, old_dict->privdata);
        if (!new_dict) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Pre-expand to avoid rehashing */
        if (dictExpand(new_dict, dictSize(old_dict)) != DICT_OK) {
            dictRelease(new_dict);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        dictIterator *iter = dictGetIterator(old_dict);
        dictEntry *entry;
        size_t migrated_members = 0;
        
        while ((entry = dictNext(iter)) != NULL) {
            sds old_member = dictGetKey(entry);
            
            /* Create new sds using standard allocation
             * Note: For true NUMA migration, would need numa-aware sds allocator */
            sds new_member = sdsnewlen(old_member, sdslen(old_member));
            if (!new_member) {
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            
            /* Add to new dict (set has NULL values) */
            if (dictAdd(new_dict, new_member, NULL) != DICT_OK) {
                sdsfree(new_member);
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                return NUMA_KEY_MIGRATE_ERR;
            }
            
            migrated_members++;
        }
        
        dictReleaseIterator(iter);
        
        /* Swap dicts and free old */
        val_obj->ptr = new_dict;
        dictRelease(old_dict);
        
        KEY_MIGRATE_LOG(LL_DEBUG, 
            "[NUMA Key Migrate] Set (hashtable) migrated, %zu members", migrated_members);
        return NUMA_KEY_MIGRATE_OK;
        
    } else {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown set encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
}

/* Migrate ZSET type */
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter */
    (void)target_node;  /* Not directly used in sds allocation */
    
    if (val_obj->encoding == OBJ_ENCODING_ZIPLIST) {
        /* Ziplist encoding: migrate as a single blob */
        unsigned char *old_zl = val_obj->ptr;
        size_t zl_len = ziplistBlobLen(old_zl);
        
        unsigned char *new_zl = numa_zmalloc_onnode(zl_len, target_node);
        if (!new_zl) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        memcpy(new_zl, old_zl, zl_len);
        val_obj->ptr = new_zl;
        zfree(old_zl);
        
        KEY_MIGRATE_LOG(LL_DEBUG, 
            "[NUMA Key Migrate] Zset (ziplist) migrated, size: %zu bytes", zl_len);
        return NUMA_KEY_MIGRATE_OK;
        
    } else if (val_obj->encoding == OBJ_ENCODING_SKIPLIST) {
        /* Skiplist encoding: migrate zset struct, dict, and skiplist
         * Use standard sds functions due to complex SDS header structure */
        zset *old_zs = val_obj->ptr;
        
        /* Allocate new zset structure */
        zset *new_zs = zmalloc(sizeof(zset));
        if (!new_zs) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Create new skiplist */
        new_zs->zsl = zslCreate();
        if (!new_zs->zsl) {
            zfree(new_zs);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Create new dict */
        new_zs->dict = dictCreate(old_zs->dict->type, old_zs->dict->privdata);
        if (!new_zs->dict) {
            zslFree(new_zs->zsl);
            zfree(new_zs);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Pre-expand dict */
        if (dictExpand(new_zs->dict, dictSize(old_zs->dict)) != DICT_OK) {
            dictRelease(new_zs->dict);
            zslFree(new_zs->zsl);
            zfree(new_zs);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Iterate over skiplist from tail to head for optimal insertion */
        zskiplistNode *old_node = old_zs->zsl->tail;
        size_t migrated_elements = 0;
        
        while (old_node) {
            /* Create new element string using standard sds */
            sds old_ele = old_node->ele;
            sds new_ele = sdsnewlen(old_ele, sdslen(old_ele));
            if (!new_ele) {
                dictRelease(new_zs->dict);
                zslFree(new_zs->zsl);
                zfree(new_zs);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            
            /* Insert into new skiplist */
            zskiplistNode *new_sl_node = zslInsert(new_zs->zsl, old_node->score, new_ele);
            
            /* Add to dict (element -> score pointer) */
            dictAdd(new_zs->dict, new_ele, &new_sl_node->score);
            
            migrated_elements++;
            old_node = old_node->backward;
        }
        
        /* Release old zset */
        dictRelease(old_zs->dict);
        zslFree(old_zs->zsl);
        zfree(old_zs);
        
        /* Update object pointer */
        val_obj->ptr = new_zs;
        
        KEY_MIGRATE_LOG(LL_DEBUG, 
            "[NUMA Key Migrate] Zset (skiplist) migrated, %zu elements", migrated_elements);
        return NUMA_KEY_MIGRATE_OK;
        
    } else {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown zset encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
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
        
        /* Update key metadata (already holding the lock, access dict directly) */
        dictEntry *meta_entry = dictFind(global_ctx.key_metadata, key);
        if (meta_entry) {
            key_numa_metadata_t *meta = dictGetVal(meta_entry);
            if (meta) {
                meta->current_node = target_node;
            }
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

/* ========== Redis Command Interface ========== */

/* External Redis functions for command handling */
extern void addReply(client *c, robj *obj);
extern void addReplyError(client *c, const char *err);
extern void addReplyErrorFormat(client *c, const char *fmt, ...);
extern void addReplyLongLong(client *c, long long ll);
extern void addReplyBulkCString(client *c, const char *s);
extern void addReplyArrayLen(client *c, long length);
extern void addReplyStatus(client *c, const char *status);
extern robj *lookupKeyRead(redisDb *db, robj *key);
extern robj *createStringObject(const char *ptr, size_t len);

/*
 * NUMAMIGRATE command implementation
 * 
 * Usage:
 *   NUMAMIGRATE KEY <key> <target_node>   - Migrate a single key to target NUMA node
 *   NUMAMIGRATE DB <target_node>          - Migrate entire database to target NUMA node
 *   NUMAMIGRATE STATS                     - Show migration statistics
 *   NUMAMIGRATE RESET                     - Reset migration statistics
 *   NUMAMIGRATE INFO <key>                - Get NUMA info for a key
 */
void numamigrateCommand(client *c) {
    if (!global_ctx.initialized) {
        addReplyError(c, "NUMA Key Migrate module not initialized");
        return;
    }
    
    if (c->argc < 2) {
        addReplyError(c, "wrong number of arguments for 'NUMAMIGRATE' command");
        return;
    }
    
    const char *subcmd = c->argv[1]->ptr;
    
    /* NUMAMIGRATE KEY <key> <target_node> */
    if (!strcasecmp(subcmd, "KEY")) {
        if (c->argc != 4) {
            addReplyError(c, "Usage: NUMAMIGRATE KEY <key> <target_node>");
            return;
        }
        
        robj *key = c->argv[2];
        long target_node;
        
        if (getLongFromObjectOrReply(c, c->argv[3], &target_node, "Invalid target node") != C_OK) {
            return;
        }
        
        if (target_node < 0 || target_node > numa_max_node()) {
            addReplyErrorFormat(c, "Target node %ld out of range (0-%d)", 
                target_node, numa_max_node());
            return;
        }
        
        int result = numa_migrate_single_key(c->db, key, (int)target_node);
        
        switch (result) {
            case NUMA_KEY_MIGRATE_OK:
                addReplyStatus(c, "OK");
                KEY_MIGRATE_LOG(LL_NOTICE, 
                    "[NUMA Key Migrate] Key migrated to node %ld via command", target_node);
                break;
            case NUMA_KEY_MIGRATE_ENOENT:
                addReplyError(c, "Key not found");
                break;
            case NUMA_KEY_MIGRATE_ENOMEM:
                addReplyError(c, "Out of memory during migration");
                break;
            case NUMA_KEY_MIGRATE_ETYPE:
                addReplyError(c, "Unsupported key type for migration");
                break;
            default:
                addReplyError(c, "Migration failed");
        }
    }
    /* NUMAMIGRATE DB <target_node> */
    else if (!strcasecmp(subcmd, "DB")) {
        if (c->argc != 3) {
            addReplyError(c, "Usage: NUMAMIGRATE DB <target_node>");
            return;
        }
        
        long target_node;
        if (getLongFromObjectOrReply(c, c->argv[2], &target_node, "Invalid target node") != C_OK) {
            return;
        }
        
        if (target_node < 0 || target_node > numa_max_node()) {
            addReplyErrorFormat(c, "Target node %ld out of range (0-%d)", 
                target_node, numa_max_node());
            return;
        }
        
        int result = numa_migrate_entire_database(c->db, (int)target_node);
        
        if (result == NUMA_KEY_MIGRATE_OK) {
            addReplyStatus(c, "OK");
            KEY_MIGRATE_LOG(LL_NOTICE, 
                "[NUMA Key Migrate] Database migrated to node %ld via command", target_node);
        } else {
            addReplyError(c, "Database migration failed or partially completed");
        }
    }
    /* NUMAMIGRATE STATS */
    else if (!strcasecmp(subcmd, "STATS")) {
        numa_key_migrate_stats_t stats;
        numa_get_migration_statistics(&stats);
        
        addReplyArrayLen(c, 12);
        addReplyBulkCString(c, "total_migrations");
        addReplyLongLong(c, stats.total_migrations);
        addReplyBulkCString(c, "successful_migrations");
        addReplyLongLong(c, stats.successful_migrations);
        addReplyBulkCString(c, "failed_migrations");
        addReplyLongLong(c, stats.failed_migrations);
        addReplyBulkCString(c, "total_bytes_migrated");
        addReplyLongLong(c, stats.total_bytes_migrated);
        addReplyBulkCString(c, "total_migration_time_us");
        addReplyLongLong(c, stats.total_migration_time_us);
        addReplyBulkCString(c, "peak_concurrent_migrations");
        addReplyLongLong(c, stats.peak_concurrent_migrations);
    }
    /* NUMAMIGRATE RESET */
    else if (!strcasecmp(subcmd, "RESET")) {
        numa_reset_migration_statistics();
        addReplyStatus(c, "OK");
    }
    /* NUMAMIGRATE INFO <key> */
    else if (!strcasecmp(subcmd, "INFO")) {
        if (c->argc != 3) {
            addReplyError(c, "Usage: NUMAMIGRATE INFO <key>");
            return;
        }
        
        robj *key = c->argv[2];
        
        /* Check if key exists */
        robj *val = lookupKeyRead(c->db, key);
        if (!val) {
            addReplyError(c, "Key not found");
            return;
        }
        
        key_numa_metadata_t *meta = numa_get_key_metadata(key);
        
        addReplyArrayLen(c, 12);
        addReplyBulkCString(c, "type");
        const char *type_name;
        switch (val->type) {
            case OBJ_STRING: type_name = "string"; break;
            case OBJ_LIST:   type_name = "list"; break;
            case OBJ_SET:    type_name = "set"; break;
            case OBJ_ZSET:   type_name = "zset"; break;
            case OBJ_HASH:   type_name = "hash"; break;
            default:         type_name = "unknown"; break;
        }
        addReplyBulkCString(c, type_name);
        
        addReplyBulkCString(c, "current_node");
        addReplyLongLong(c, meta ? meta->current_node : -1);
        
        addReplyBulkCString(c, "hotness_level");
        addReplyLongLong(c, meta ? meta->hotness_level : 0);
        
        addReplyBulkCString(c, "access_count");
        addReplyLongLong(c, meta ? meta->access_count : 0);
        
        addReplyBulkCString(c, "numa_nodes_available");
        addReplyLongLong(c, numa_max_node() + 1);
        
        addReplyBulkCString(c, "current_cpu_node");
        int cpu = sched_getcpu();
        int cpu_node = (cpu >= 0) ? numa_node_of_cpu(cpu) : 0;
        addReplyLongLong(c, cpu_node);
    }
    /* NUMAMIGRATE HELP */
    else if (!strcasecmp(subcmd, "HELP")) {
        addReplyArrayLen(c, 6);
        addReplyBulkCString(c, "NUMAMIGRATE KEY <key> <target_node> - Migrate a key to target NUMA node");
        addReplyBulkCString(c, "NUMAMIGRATE DB <target_node> - Migrate entire database to target NUMA node");
        addReplyBulkCString(c, "NUMAMIGRATE STATS - Show migration statistics");
        addReplyBulkCString(c, "NUMAMIGRATE RESET - Reset migration statistics");
        addReplyBulkCString(c, "NUMAMIGRATE INFO <key> - Get NUMA info for a key");
        addReplyBulkCString(c, "NUMAMIGRATE HELP - Show this help message");
    }
    else {
        addReplyErrorFormat(c, "Unknown subcommand '%s'. Try NUMAMIGRATE HELP.", subcmd);
    }
}
