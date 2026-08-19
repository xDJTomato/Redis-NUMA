/* numa_key_migrate.c - NUMA key-level migration implementation
 *
 * Redis key-level migration implementation with LRU-integrated hotness tracking.
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
#include "listpack.h"
#include <string.h>
#include <stdio.h>
#include <numa.h>
#include <sys/time.h>

/* External Redis function declarations. */
extern void _serverLog(int level, const char *fmt, ...);
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_DEBUG 0
#define KEY_MIGRATE_LOG(level, fmt, ...) _serverLog(level, fmt, ##__VA_ARGS__)

/* External zset function declarations. */
extern zskiplist *zslCreate(void);
extern void zslFree(zskiplist *zsl);
extern zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);

/* Global context. */
static numa_key_migrate_ctx_t global_ctx = {0};

/* ========== Helper functions ========== */

/* Get the current time (microseconds). */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Compute the time difference, handling LRU clock wrap-around. */
static uint16_t calculate_time_delta(uint16_t current, uint16_t last) {
    if (current >= last) {
        return current - last;
    } else {
        /* Wrap-around case. */
        return (0xFFFF - last) + current + 1;
    }
}

/*
 * compute_key_lazy_decay_steps - staircase lazy-decay lookup for key metadata
 *
 * Uses the same staircase policy as composite_lru to keep hotness semantics
 * consistent:
 *  elapsed < 10s   : decay 0
 *  elapsed < 60s   : decay 1
 *  elapsed < 5min  : decay 2
 *  elapsed < 30min : decay 3
 *  elapsed >= 30min: fully cleared
 */
static uint8_t compute_key_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < KEY_LAZY_DECAY_STEP1_SECS) return 0;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP2_SECS) return 1;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP3_SECS) return 2;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP4_SECS) return 3;
    return HOTNESS_MAX_LEVEL; /* Long-idle keys are fully cleared. */
}

/* Get the NUMA node of the current thread. */
static int get_current_numa_node(void) {
    int cpu = sched_getcpu();
    if (cpu < 0) return 0;
    return numa_node_of_cpu(cpu);
}

/* ========== Metadata management ========== */

/* robj pointer hash function. */
static uint64_t key_obj_hash(const void *key) {
    return dictGenHashFunction(key, sizeof(void*));
}

/* robj pointer compare function. */
static int key_obj_compare(dict *d, const void *key1, const void *key2) {
    (void)d;
    return key1 == key2 ? 0 : 1;
}

/* Metadata destructor. */
static void metadata_destructor(dict *d, void *val) {
    (void)d;
    zfree(val);
}

/* Key metadata dict type. */
static dictType keyMetadataDictType = {
    key_obj_hash,           /* Hash function. */
    NULL,                   /* key dup. */
    NULL,                   /* val dup. */
    key_obj_compare,        /* Key compare. */
    NULL,                   /* key destructor. */
    metadata_destructor     /* val destructor. */
};

/* Create key metadata. */
static key_numa_metadata_t* create_key_metadata(robj *key, robj *val) {
    key_numa_metadata_t *meta = zmalloc(sizeof(*meta));
    if (!meta) return NULL;
    
    meta->current_node = 0;  /* Node 0 by default. */
    meta->hotness_level = HOTNESS_DEFAULT;
    meta->last_access_time = LRU_CLOCK() & 0xFFFF;
    meta->memory_footprint = 0;  /* To be updated. */
    meta->access_count = 1;
    
    return meta;
}

/* Get or create key metadata. */
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

/* ========== Module initialization ========== */

int numa_key_migrate_init(void) {
    if (global_ctx.initialized) {
        return NUMA_KEY_MIGRATE_OK;
    }
    
    if (numa_available() == -1) {
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] NUMA not available");
        return NUMA_KEY_MIGRATE_ERR;
    }
    
    /* Initialize the metadata dict. */
    global_ctx.key_metadata = dictCreate(&keyMetadataDictType);
    if (!global_ctx.key_metadata) {
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] Failed to create metadata dict");
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* Initialize the mutex. */
    if (pthread_mutex_init(&global_ctx.mutex, NULL) != 0) {
        dictRelease(global_ctx.key_metadata);
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] Failed to initialize mutex");
        return NUMA_KEY_MIGRATE_ERR;
    }
    
    /* Initialize the statistics. */
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

/*
 * numa_on_key_delete - clean up NUMA metadata when a key is deleted
 *
 * Must be called from dbSyncDelete/dbAsyncDelete to remove stale entries from
 * the metadata dict. Without this hook, expired or DELeted keys leave ghost
 * entries that waste memory, and reused pointer addresses may yield wrong
 * hotness readings.
 */
void numa_on_key_delete(robj *key) {
    if (!global_ctx.initialized || !key) return;
    pthread_mutex_lock(&global_ctx.mutex);
    int ret = dictDelete(global_ctx.key_metadata, key);
    pthread_mutex_unlock(&global_ctx.mutex);
    if (ret == DICT_OK) {
        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate] Metadata cleaned for deleted key=%p", (void*)key);
    }
}

/* ========== Hotness tracking ========== */

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

    /* Update access statistics - save the old timestamp before overwriting. */
    meta->access_count++;
    uint16_t old_last_access = meta->last_access_time;
    meta->last_access_time = current_timestamp;

    /* Staircase lazy decay: settle the decay debt accumulated since the last access. */
    uint16_t elapsed = calculate_time_delta(current_timestamp, old_last_access);
    uint8_t decay = compute_key_lazy_decay_steps(elapsed);
    if (decay > 0) {
        uint8_t before = meta->hotness_level;
        meta->hotness_level = (decay >= meta->hotness_level) ? 0 : (meta->hotness_level - decay);
        if (meta->hotness_level != before) {
            KEY_MIGRATE_LOG(LL_DEBUG,
                "[NUMA Key Migrate] Lazy decay: key=%p, elapsed=%us, decay=%d, hotness %d->%d",
                (void*)key, (unsigned)elapsed, decay, before, meta->hotness_level);
        }
    }

    /* Any access increases hotness (local or remote). */
    if (meta->hotness_level < HOTNESS_MAX_LEVEL) {
        meta->hotness_level++;
    }

    /* Remote access: record a migration candidate when hotness reaches the threshold. */
    if (meta->current_node != current_cpu_node) {
        if (meta->hotness_level >= MIGRATION_HOTNESS_THRESHOLD) {
            /* TODO: schedule migration evaluation. */
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
        
        /* Decay hotness when the key has not been accessed for a while. */
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

/* ========== Type-specific migration adapters ========== */

void *numa_object_sample_alloc_ptr(robj *val) {
    if (!val || !val->ptr) return NULL;

    if (val->type == OBJ_STRING) {
        if (val->encoding == OBJ_ENCODING_RAW) return sdsAllocPtr(val->ptr);
        return NULL;
    }

    switch (val->encoding) {
    case OBJ_ENCODING_ZIPLIST:
    case OBJ_ENCODING_LISTPACK:
    case OBJ_ENCODING_INTSET:
    case OBJ_ENCODING_QUICKLIST:
    case OBJ_ENCODING_SKIPLIST:
    case OBJ_ENCODING_STREAM:
        return val->ptr;
    case OBJ_ENCODING_HT: {
        dict *d = val->ptr;
        if (dictSize(d) == 0) return val->ptr;
        dictIterator *iter = dictGetIterator(d);
        dictEntry *sample = dictNext(iter);
        dictReleaseIterator(iter);
        if (!sample) return val->ptr;
        if (val->type == OBJ_HASH) {
            sds value = dictGetVal(sample);
            return value ? sdsAllocPtr(value) : val->ptr;
        }
        if (val->type == OBJ_SET) {
            sds member = dictGetKey(sample);
            return member ? sdsAllocPtr(member) : val->ptr;
        }
        return val->ptr;
    }
    default:
        return val->ptr;
    }
}

size_t numa_object_sample_alloc_size(robj *val) {
    void *sample = numa_object_sample_alloc_ptr(val);
    if (!sample) return 0;

    if (val->type == OBJ_STRING && val->encoding == OBJ_ENCODING_RAW)
        return sdsAllocSize(val->ptr);

    if (val->encoding == OBJ_ENCODING_ZIPLIST)
        return ziplistBlobLen(val->ptr);

    if (val->encoding == OBJ_ENCODING_LISTPACK)
        return lpBytes(val->ptr);

    if (val->encoding == OBJ_ENCODING_INTSET)
        return intsetBlobLen(val->ptr);

    if (val->encoding == OBJ_ENCODING_HT) {
        dict *d = val->ptr;
        uint64_t sample_bytes = 0;
        uint64_t sample_count = 0;
        dictIterator *iter = dictGetIterator(d);
        dictEntry *de;
        while (sample_count < 8 && (de = dictNext(iter)) != NULL) {
            if (val->type == OBJ_HASH) {
                sds field = dictGetKey(de);
                sds value = dictGetVal(de);
                if (field) sample_bytes += sdsAllocSize(field);
                if (value) sample_bytes += sdsAllocSize(value);
            } else if (val->type == OBJ_SET) {
                sds member = dictGetKey(de);
                if (member) sample_bytes += sdsAllocSize(member);
            }
            sample_bytes += dictEntryMemUsage();
            sample_count++;
        }
        dictReleaseIterator(iter);
        if (sample_count > 0) {
            uint64_t elements = dictSize(d);
            uint64_t avg = (sample_bytes + sample_count - 1) / sample_count;
            uint64_t estimated = avg * elements;
            estimated += dictSlots(d) * sizeof(dictEntry *);
            if (estimated > SIZE_MAX) return SIZE_MAX;
            return (size_t)estimated;
        }
    }

    return 0;
}

uint32_t numa_object_migration_cost_units(robj *val) {
    size_t size = numa_object_sample_alloc_size(val);
    if (size == 0) return 1;

    /* 64KiB is the baseline direct-path boundary. Larger objects consume
     * proportionally more step budget but are still eligible for migration. */
    const size_t unit = 64 * 1024;
    uint64_t units = (size + unit - 1) / unit;
    if (units < 1) units = 1;
    if (units > 1024) units = 1024;
    return (uint32_t)units;
}

/* Migrate the STRING type. */
int migrate_string_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;

    if (val_obj->encoding != OBJ_ENCODING_RAW &&
        val_obj->encoding != OBJ_ENCODING_EMBSTR) {
        return NUMA_KEY_MIGRATE_OK;
    }

    /* EMBSTR stores the SDS header and the string payload inside the robj
     * allocation itself, so sdsAllocPtr() would return a pointer into the
     * middle of the robj block rather than the zmalloc allocation base.  We
     * cannot realloc that pointer.  Convert it to a RAW SDS allocated on the
     * target node instead; the old embedded bytes stay in the (still valid)
     * robj block and the string data is now node-local on the target. */
    if (val_obj->encoding == OBJ_ENCODING_EMBSTR) {
        sds old_str = val_obj->ptr;
        if (numa_get_node_id(val_obj) == target_node)
            return NUMA_KEY_MIGRATE_OK;

        size_t len = sdslen(old_str);
        numa_alloc_push_node(target_node);
        sds new_str = sdsnewlen(old_str, len);
        numa_alloc_pop_node();
        if (!new_str)
            return NUMA_KEY_MIGRATE_ENOMEM;

        val_obj->ptr = new_str;
        val_obj->encoding = OBJ_ENCODING_RAW;
        numa_set_migrated(sdsAllocPtr(new_str), 1);
        return NUMA_KEY_MIGRATE_OK;
    }

    /* RAW: the SDS header is a standalone zmalloc allocation, safe to move. */
    sds old_str = val_obj->ptr;
    void *old_base = sdsAllocPtr(old_str);
    int cur_node = numa_get_node_id(old_base);
    if (cur_node == target_node)
        return NUMA_KEY_MIGRATE_OK;

    size_t total = sdsAllocSize(old_str);
    ptrdiff_t str_offset = (char *)old_str - (char *)old_base;

    void *new_base = numa_zrealloc_onnode(old_base, total, target_node);
    if (!new_base) {
        return NUMA_KEY_MIGRATE_ENOMEM;
    }

    sds new_str = (char *)new_base + str_offset;
    val_obj->ptr = new_str;

    /* Mark the moved data allocation, not the robj metadata (the robj was not
     * moved and normally remains on the metadata/DRAM node). */
    numa_set_migrated(sdsAllocPtr(new_str), 1);
    return NUMA_KEY_MIGRATE_OK;
}

/* Migrate the HASH type. */
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter. */
    
    if (val_obj->encoding == OBJ_ENCODING_ZIPLIST || val_obj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *old_zl = val_obj->ptr;
        int cur_node = numa_get_node_id(old_zl);
        if (cur_node == target_node)
            return NUMA_KEY_MIGRATE_OK;

        size_t zl_len = val_obj->encoding == OBJ_ENCODING_LISTPACK ?
            lpBytes(old_zl) : ziplistBlobLen(old_zl);

        unsigned char *new_zl = numa_zmalloc_onnode(zl_len, target_node);
        if (!new_zl) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        memcpy(new_zl, old_zl, zl_len);
        val_obj->ptr = new_zl;
        zfree(old_zl);
        numa_set_node_id(val_obj, target_node);
        numa_set_migrated(val_obj, 1);

        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate] Hash (%s) migrated, size: %zu bytes",
            val_obj->encoding == OBJ_ENCODING_LISTPACK ? "listpack" : "ziplist", zl_len);
        return NUMA_KEY_MIGRATE_OK;

    } else if (val_obj->encoding == OBJ_ENCODING_HT) {
        dict *old_dict = val_obj->ptr;
        int cur_node = numa_get_node_id(old_dict);
        if (cur_node == target_node)
            return NUMA_KEY_MIGRATE_OK;

        /* Hashtable encoding: migrate the dict and all sds field/value pairs.
         * Since SDS headers are complex, use the standard sds functions. */
        numa_alloc_push_node(target_node);
        dict *new_dict = dictCreate(old_dict->type);
        if (!new_dict) {
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        /* Pre-expand to avoid rehashing during migration. */
        if (dictExpand(new_dict, dictSize(old_dict)) != DICT_OK) {
            dictRelease(new_dict);
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        dictIterator *iter = dictGetIterator(old_dict);
        dictEntry *entry;
        size_t migrated_pairs = 0;

        while ((entry = dictNext(iter)) != NULL) {
            sds old_field = dictGetKey(entry);
            sds old_value = dictGetVal(entry);

            /* Create a new sds with the standard allocator. */
            sds new_field = sdsnewlen(old_field, sdslen(old_field));
            if (!new_field) {
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ENOMEM;
            }

            sds new_value = sdsnewlen(old_value, sdslen(old_value));
            if (!new_value) {
                sdsfree(new_field);
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            numa_set_migrated(sdsAllocPtr(new_value), 1);

            /* Add to the new dict (ownership of new_field and new_value). */
            if (dictAdd(new_dict, new_field, new_value) != DICT_OK) {
                sdsfree(new_field);
                sdsfree(new_value);
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ERR;
            }

            migrated_pairs++;
        }

        dictReleaseIterator(iter);

        /* Swap the dict and free the old one. */
        val_obj->ptr = new_dict;
        dictRelease(old_dict);

        numa_alloc_pop_node();
        numa_set_node_id(val_obj, target_node);
        numa_set_migrated(val_obj, 1);

        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate] Hash (hashtable) migrated, %zu pairs", migrated_pairs);
        return NUMA_KEY_MIGRATE_OK;
        
    } else {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown hash encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
}

/* Migrate the LIST type. */
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter. */

    if (val_obj->encoding != OBJ_ENCODING_QUICKLIST) {
        KEY_MIGRATE_LOG(LL_WARNING,
            "[NUMA Key Migrate] Unknown list encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }

    numa_alloc_push_node(target_node);

    quicklist *old_ql = val_obj->ptr;

    /* Create a new quicklist with the standard allocator. */
    quicklist *new_ql = zmalloc(sizeof(quicklist));
    if (!new_ql) {
        numa_alloc_pop_node();
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* Copy the quicklist header. */
    new_ql->head = NULL;
    new_ql->tail = NULL;
    new_ql->count = old_ql->count;
    new_ql->len = 0;
    new_ql->fill = old_ql->fill;
    new_ql->compress = old_ql->compress;
    new_ql->bookmark_count = 0;
    
    /* Iterate over all quicklist nodes and migrate them. */
    quicklistNode *old_node = old_ql->head;
    quicklistNode *prev_new_node = NULL;
    size_t migrated_nodes = 0;
    
    while (old_node) {
        /* Allocate a new node with the standard zmalloc. */
        quicklistNode *new_node = zmalloc(sizeof(quicklistNode));
        if (!new_node) {
            /* Clean up on failure. */
            quicklistNode *cleanup = new_ql->head;
            while (cleanup) {
                quicklistNode *next = cleanup->next;
                if (cleanup->entry) zfree(cleanup->entry);
                zfree(cleanup);
                cleanup = next;
            }
            zfree(new_ql);
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* Copy the node metadata. */
        new_node->count = old_node->count;
        new_node->sz = old_node->sz;
        new_node->encoding = old_node->encoding;
        new_node->container = old_node->container;
        new_node->recompress = old_node->recompress;
        new_node->attempted_compress = old_node->attempted_compress;
        new_node->extra = old_node->extra;
        new_node->prev = prev_new_node;
        new_node->next = NULL;
        
        /* Migrate the ziplist data. */
        if (old_node->encoding == QUICKLIST_NODE_ENCODING_LZF) {
            /* LZF-compressed encoding. */
            quicklistLZF *old_lzf = (quicklistLZF *)old_node->entry;
            size_t lzf_sz = sizeof(quicklistLZF) + old_lzf->sz;
            new_node->entry = zmalloc(lzf_sz);
            if (!new_node->entry) {
                zfree(new_node);
                quicklistNode *cleanup = new_ql->head;
                while (cleanup) {
                    quicklistNode *next = cleanup->next;
                    if (cleanup->entry) zfree(cleanup->entry);
                    zfree(cleanup);
                    cleanup = next;
                }
                zfree(new_ql);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            memcpy(new_node->entry, old_node->entry, lzf_sz);
        } else {
            /* Plain ziplist. */
            new_node->entry = zmalloc(old_node->sz);
            if (!new_node->entry) {
                zfree(new_node);
                quicklistNode *cleanup = new_ql->head;
                while (cleanup) {
                    quicklistNode *next = cleanup->next;
                    if (cleanup->entry) zfree(cleanup->entry);
                    zfree(cleanup);
                    cleanup = next;
                }
                zfree(new_ql);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            memcpy(new_node->entry, old_node->entry, old_node->sz);
        }
        
        /* Link the node. */
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
    
    /* Free the old quicklist. */
    old_node = old_ql->head;
    while (old_node) {
        quicklistNode *next = old_node->next;
        if (old_node->entry) zfree(old_node->entry);
        zfree(old_node);
        old_node = next;
    }
    zfree(old_ql);
    
    /* Update the object pointer. */
    val_obj->ptr = new_ql;

    numa_alloc_pop_node();

    KEY_MIGRATE_LOG(LL_DEBUG,
        "[NUMA Key Migrate] List (quicklist) migrated, %zu nodes", migrated_nodes);
    return NUMA_KEY_MIGRATE_OK;
}

/* Migrate the SET type. */
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter. */
    
    if (val_obj->encoding == OBJ_ENCODING_INTSET) {
        /* Intset encoding: migrate as a whole. */
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
        /* Hashtable encoding: migrate the dict and all sds elements. */
        numa_alloc_push_node(target_node);

        dict *old_dict = val_obj->ptr;
        dict *new_dict = dictCreate(old_dict->type);
        if (!new_dict) {
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        if (dictExpand(new_dict, dictSize(old_dict)) != DICT_OK) {
            dictRelease(new_dict);
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        dictIterator *iter = dictGetIterator(old_dict);
        dictEntry *entry;
        size_t migrated_members = 0;

        while ((entry = dictNext(iter)) != NULL) {
            sds old_member = dictGetKey(entry);
            sds new_member = sdsnewlen(old_member, sdslen(old_member));
            if (!new_member) {
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ENOMEM;
            }

            if (dictAdd(new_dict, new_member, NULL) != DICT_OK) {
                sdsfree(new_member);
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ERR;
            }

            migrated_members++;
        }

        dictReleaseIterator(iter);

        val_obj->ptr = new_dict;
        dictRelease(old_dict);

        numa_alloc_pop_node();

        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate] Set (hashtable) migrated, %zu members", migrated_members);
        return NUMA_KEY_MIGRATE_OK;
        
    } else {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown set encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
}

/* Migrate the ZSET type. */
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* Unused parameter. */

    if (val_obj->encoding == OBJ_ENCODING_ZIPLIST || val_obj->encoding == OBJ_ENCODING_LISTPACK) {
        /* Ziplist/listpack encoding: migrate as a whole. */
        unsigned char *old_zl = val_obj->ptr;
        size_t zl_len = val_obj->encoding == OBJ_ENCODING_LISTPACK ?
            lpBytes(old_zl) : ziplistBlobLen(old_zl);

        unsigned char *new_zl = numa_zmalloc_onnode(zl_len, target_node);
        if (!new_zl) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        memcpy(new_zl, old_zl, zl_len);
        val_obj->ptr = new_zl;
        zfree(old_zl);

        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate] Zset (%s) migrated, size: %zu bytes",
            val_obj->encoding == OBJ_ENCODING_LISTPACK ? "listpack" : "ziplist", zl_len);
        return NUMA_KEY_MIGRATE_OK;
        
    } else if (val_obj->encoding == OBJ_ENCODING_SKIPLIST) {
        /* Skiplist encoding: migrate the zset struct, dict, and skiplist. */
        numa_alloc_push_node(target_node);

        zset *old_zs = val_obj->ptr;

        zset *new_zs = zmalloc(sizeof(zset));
        if (!new_zs) {
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        new_zs->zsl = zslCreate();
        if (!new_zs->zsl) {
            zfree(new_zs);
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        new_zs->dict = dictCreate(old_zs->dict->type);
        if (!new_zs->dict) {
            zslFree(new_zs->zsl);
            zfree(new_zs);
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        if (dictExpand(new_zs->dict, dictSize(old_zs->dict)) != DICT_OK) {
            dictRelease(new_zs->dict);
            zslFree(new_zs->zsl);
            zfree(new_zs);
            numa_alloc_pop_node();
            return NUMA_KEY_MIGRATE_ENOMEM;
        }

        zskiplistNode *old_node = old_zs->zsl->tail;
        size_t migrated_elements = 0;

        while (old_node) {
            sds old_ele = old_node->ele;
            sds new_ele = sdsnewlen(old_ele, sdslen(old_ele));
            if (!new_ele) {
                dictRelease(new_zs->dict);
                zslFree(new_zs->zsl);
                zfree(new_zs);
                numa_alloc_pop_node();
                return NUMA_KEY_MIGRATE_ENOMEM;
            }

            zskiplistNode *new_sl_node = zslInsert(new_zs->zsl, old_node->score, new_ele);
            dictAdd(new_zs->dict, new_ele, &new_sl_node->score);

            migrated_elements++;
            old_node = old_node->backward;
        }

        dictRelease(old_zs->dict);
        zslFree(old_zs->zsl);
        zfree(old_zs);

        val_obj->ptr = new_zs;

        numa_alloc_pop_node();

        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate] Zset (skiplist) migrated, %zu elements", migrated_elements);
        return NUMA_KEY_MIGRATE_OK;
        
    } else {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown zset encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
}

/* ========== Migration execution ========== */

int numa_migrate_single_key(redisDb *db, robj *key, int target_node) {
    if (!global_ctx.initialized || !db || !key) {
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    if (target_node < 0 || target_node > numa_max_node()) {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Invalid target node %d", target_node);
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    /* Look up the key in the database. */
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
    
    /* Type-specific migration. */
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
    
    /* Update the statistics. */
    pthread_mutex_lock(&global_ctx.mutex);
    
    global_ctx.stats.total_migrations++;
    if (result == NUMA_KEY_MIGRATE_OK) {
        global_ctx.stats.successful_migrations++;
        
        /* Update the key metadata (lock already held, access the dict directly). */
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

int numa_migrate_key_by_name(redisDb *db, const char *keyname, int target_node) {
    if (!global_ctx.initialized || !db || !keyname) {
        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate][debug] by-name rejected initialized=%d db=%p key=%p target=%d",
            global_ctx.initialized, (void *)db, (const void *)keyname, target_node);
        return NUMA_KEY_MIGRATE_EINVAL;
    }

    if (target_node < 0 || target_node > numa_max_node()) {
        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate][debug] by-name invalid target key=%s target=%d max=%d",
            keyname, target_node, numa_max_node());
        return NUMA_KEY_MIGRATE_EINVAL;
    }

    dictEntry *de = dictFind(db->dict, keyname);
    if (!de) {
        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate][debug] by-name lookup miss key=%s target=%d",
            keyname, target_node);
        return NUMA_KEY_MIGRATE_ENOENT;
    }

    robj *val = dictGetVal(de);
    if (!val) {
        KEY_MIGRATE_LOG(LL_DEBUG,
            "[NUMA Key Migrate][debug] by-name null value key=%s target=%d",
            keyname, target_node);
        return NUMA_KEY_MIGRATE_ENOENT;
    }

    uint64_t start_time = get_current_time_us();
    void *before_ptr = numa_object_sample_alloc_ptr(val);
    if (!before_ptr) before_ptr = val;
    int before_node = numa_get_node_id(before_ptr);
    if (before_node == target_node)
        return NUMA_KEY_MIGRATE_OK;

    int result = NUMA_KEY_MIGRATE_OK;

    KEY_MIGRATE_LOG(LL_DEBUG,
        "[NUMA Key Migrate][debug] by-name begin key=%s type=%d encoding=%d ptr=%p target=%d",
        keyname, val->type, val->encoding, val->ptr, target_node);

    switch (val->type) {
        case OBJ_STRING:
            result = migrate_string_type(NULL, val, target_node);
            break;
        case OBJ_HASH:
            result = migrate_hash_type(NULL, val, target_node);
            break;
        case OBJ_LIST:
            result = migrate_list_type(NULL, val, target_node);
            break;
        case OBJ_SET:
            result = migrate_set_type(NULL, val, target_node);
            break;
        case OBJ_ZSET:
            result = migrate_zset_type(NULL, val, target_node);
            break;
        default:
            result = NUMA_KEY_MIGRATE_ETYPE;
    }

    uint64_t elapsed = get_current_time_us() - start_time;
    void *after_ptr = numa_object_sample_alloc_ptr(val);
    if (!after_ptr) after_ptr = val;
    int after_node = numa_get_node_id(after_ptr);

    KEY_MIGRATE_LOG(LL_DEBUG,
        "[NUMA Key Migrate][debug] by-name end key=%s result=%d elapsed_us=%llu",
        keyname, result, (unsigned long long)elapsed);

    pthread_mutex_lock(&global_ctx.mutex);
    if (result == NUMA_KEY_MIGRATE_OK && after_node == target_node) {
        global_ctx.stats.total_migrations++;
        global_ctx.stats.successful_migrations++;
        global_ctx.stats.total_migration_time_us += elapsed;
    } else if (result != NUMA_KEY_MIGRATE_OK) {
        global_ctx.stats.total_migrations++;
        global_ctx.stats.failed_migrations++;
        global_ctx.stats.total_migration_time_us += elapsed;
    }
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
        sds key = dictGetKey(entry);
        int result = numa_migrate_key_by_name(db, key, target_node);
        
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

/* ========== Query interface ========== */

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

/* ========== Public query interface ========== */

int numa_key_migrate_is_initialized(void) {
    return global_ctx.initialized;
}
