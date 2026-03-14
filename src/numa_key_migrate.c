/* numa_key_migrate.c - NUMA Key级别迁移实现
 *
 * 基于LRU集成热度跟踪的Redis Key级别迁移实现。
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

/* 外部Redis函数声明 */
extern void _serverLog(int level, const char *fmt, ...);
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define LL_DEBUG 0
#define KEY_MIGRATE_LOG(level, fmt, ...) _serverLog(level, fmt, ##__VA_ARGS__)

/* 外部zset函数声明 */
extern zskiplist *zslCreate(void);
extern void zslFree(zskiplist *zsl);
extern zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);

/* 全局上下文 */
static numa_key_migrate_ctx_t global_ctx = {0};

/* ========== 辅助函数 ========== */

/* 获取当前时间（微秒） */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* 计算时间差，处理LRU时钟回绕 */
static uint16_t calculate_time_delta(uint16_t current, uint16_t last) {
    if (current >= last) {
        return current - last;
    } else {
        /* 回绕情况 */
        return (0xFFFF - last) + current + 1;
    }
}

/*
 * compute_key_lazy_decay_steps - Key元数据阶梯式惰性衰减查表
 *
 * 与composite_lru相同的阶梯策略，保证热度语义一致：
 *  elapsed < 10s   : 衰减0
 *  elapsed < 60s   : 衰减1
 *  elapsed < 5min  : 衰减2
 *  elapsed < 30min : 衰减3
 *  elapsed >= 30min: 全清除
 */
static uint8_t compute_key_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < KEY_LAZY_DECAY_STEP1_SECS) return 0;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP2_SECS) return 1;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP3_SECS) return 2;
    if (elapsed_secs < KEY_LAZY_DECAY_STEP4_SECS) return 3;
    return HOTNESS_MAX_LEVEL; /* 长期空闲key全清除 */
}

/* 获取当前线程所在NUMA节点 */
static int get_current_numa_node(void) {
    int cpu = sched_getcpu();
    if (cpu < 0) return 0;
    return numa_node_of_cpu(cpu);
}

/* ========== 元数据管理 ========== */

/* robj指针哈希函数 */
static uint64_t key_obj_hash(const void *key) {
    return dictGenHashFunction(key, sizeof(void*));
}

/* robj指针比较函数 */
static int key_obj_compare(void *privdata, const void *key1, const void *key2) {
    (void)privdata;
    return key1 == key2 ? 0 : 1;
}

/* 元数据析构函数 */
static void metadata_destructor(void *privdata, void *val) {
    (void)privdata;
    zfree(val);
}

/* Key元数据字典类型 */
static dictType keyMetadataDictType = {
    key_obj_hash,           /* 哈希函数 */
    NULL,                   /* key复制 */
    NULL,                   /* val复制 */
    key_obj_compare,        /* key比较 */
    NULL,                   /* key析构 */
    metadata_destructor     /* val析构 */
};

/* 创建 key元数据 */
static key_numa_metadata_t* create_key_metadata(robj *key, robj *val) {
    key_numa_metadata_t *meta = zmalloc(sizeof(*meta));
    if (!meta) return NULL;
    
    meta->current_node = 0;  /* 默认节点0 */
    meta->hotness_level = HOTNESS_DEFAULT;
    meta->last_access_time = LRU_CLOCK() & 0xFFFF;
    meta->memory_footprint = 0;  /* 待更新 */
    meta->access_count = 1;
    
    return meta;
}

/* 获取或创建 key元数据 */
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

/* ========== 模块初始化 ========== */

int numa_key_migrate_init(void) {
    if (global_ctx.initialized) {
        return NUMA_KEY_MIGRATE_OK;
    }
    
    if (numa_available() == -1) {
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] NUMA not available");
        return NUMA_KEY_MIGRATE_ERR;
    }
    
    /* 初始化元数据字典 */
    global_ctx.key_metadata = dictCreate(&keyMetadataDictType, NULL);
    if (!global_ctx.key_metadata) {
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] Failed to create metadata dict");
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* 初始化互斥锁 */
    if (pthread_mutex_init(&global_ctx.mutex, NULL) != 0) {
        dictRelease(global_ctx.key_metadata);
        KEY_MIGRATE_LOG(LL_WARNING, "[NUMA Key Migrate] Failed to initialize mutex");
        return NUMA_KEY_MIGRATE_ERR;
    }
    
    /* 初始化统计信息 */
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
 * numa_on_key_delete - 当key被删除时清理NUMA元数据
 *
 * 必须在 dbSyncDelete/dbAsyncDelete 中调用，以删除元数据字典中的过期条目。
 * 若无此钉子，过期或已DEL的key将残留幽灵条目，平白占用内存，
 * 且指针地址复用时可能产生错误热度读数。
 */
void numa_on_key_delete(robj *key) {
    if (!global_ctx.initialized || !key) return;
    pthread_mutex_lock(&global_ctx.mutex);
    int ret = dictDelete(global_ctx.key_metadata, key);
    pthread_mutex_unlock(&global_ctx.mutex);
    if (ret == DICT_OK) {
        KEY_MIGRATE_LOG(LL_VERBOSE,
            "[NUMA Key Migrate] Metadata cleaned for deleted key=%p", (void*)key);
    }
}

/* ========== 热度跟踪 ========== */

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

    /* 更新访问统计 - 在覆写前保存旧时间戳 */
    meta->access_count++;
    uint16_t old_last_access = meta->last_access_time;
    meta->last_access_time = current_timestamp;

    /* 阶梯式惰性衰减：结算自上次访问积累的衰减债务 */
    uint16_t elapsed = calculate_time_delta(current_timestamp, old_last_access);
    uint8_t decay = compute_key_lazy_decay_steps(elapsed);
    if (decay > 0) {
        uint8_t before = meta->hotness_level;
        meta->hotness_level = (decay >= meta->hotness_level) ? 0 : (meta->hotness_level - decay);
        if (meta->hotness_level != before) {
            KEY_MIGRATE_LOG(LL_VERBOSE,
                "[NUMA Key Migrate] Lazy decay: key=%p, elapsed=%us, decay=%d, hotness %d->%d",
                (void*)key, (unsigned)elapsed, decay, before, meta->hotness_level);
        }
    }

    /* 任意访问时热度必定增加（无论本地还是远程） */
    if (meta->hotness_level < HOTNESS_MAX_LEVEL) {
        meta->hotness_level++;
    }

    /* 远程访问：热度达到阈值时记录迁移候选 */
    if (meta->current_node != current_cpu_node) {
        if (meta->hotness_level >= MIGRATION_HOTNESS_THRESHOLD) {
            /* TODO: 调度迁移评估 */
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
        
        /* 一段时间未访问时衰减热度 */
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

/* ========== 类型特定迁移适配器 ========== */

/* 迁移 STRING 类型 */
int migrate_string_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* 未使用参数 */
    (void)target_node;  /* sds分配时不直接使用 */
    
    if (val_obj->encoding != OBJ_ENCODING_RAW && 
        val_obj->encoding != OBJ_ENCODING_EMBSTR) {
        /* 整数编码，无需迁移 */
        return NUMA_KEY_MIGRATE_OK;
    }
    
    sds old_str = val_obj->ptr;
    
    /* 创建新sds（使用标准分配，处理复杂SDS头部结构） */
    sds new_str = sdsnewlen(old_str, sdslen(old_str));
    if (!new_str) {
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* 更新指针 */
    val_obj->ptr = new_str;
    
    /* 释放旧内存 */
    sdsfree(old_str);
    
    return NUMA_KEY_MIGRATE_OK;
}

/* 迁移 HASH 类型 */
int migrate_hash_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* 未使用参数 */
    
    if (val_obj->encoding == OBJ_ENCODING_ZIPLIST) {
        /* Ziplist编码：整体迁移 */
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
        /* 哈希表编码：迁移dict及所有sds字段/値对
         * 因SDS头部结构复杂，使用标准sds函数 */
        dict *old_dict = val_obj->ptr;
        dict *new_dict = dictCreate(old_dict->type, old_dict->privdata);
        if (!new_dict) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* 预展开以避免迁移中重哈希 */
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
            
            /* 使用标准分配创建新sds */
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
            
            /* 添加到新dict（所有权new_field和new_value） */
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
        
        /* 交换dict并释放旧的 */
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

/* 迁移 LIST 类型 */
int migrate_list_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* 未使用参数 */
    (void)target_node;  /* zmalloc分配时不直接使用 */
    
    if (val_obj->encoding != OBJ_ENCODING_QUICKLIST) {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Unknown list encoding: %d", val_obj->encoding);
        return NUMA_KEY_MIGRATE_ETYPE;
    }
    
    quicklist *old_ql = val_obj->ptr;
    
    /* 使用标准分配创建新quicklist */
    quicklist *new_ql = zmalloc(sizeof(quicklist));
    if (!new_ql) {
        return NUMA_KEY_MIGRATE_ENOMEM;
    }
    
    /* 复制quicklist头部 */
    new_ql->head = NULL;
    new_ql->tail = NULL;
    new_ql->count = old_ql->count;
    new_ql->len = 0;
    new_ql->fill = old_ql->fill;
    new_ql->compress = old_ql->compress;
    new_ql->bookmark_count = 0;
    
    /* 遍历所有quicklist节点并迁移 */
    quicklistNode *old_node = old_ql->head;
    quicklistNode *prev_new_node = NULL;
    size_t migrated_nodes = 0;
    
    while (old_node) {
        /* 使用标准zmalloc分配新节点 */
        quicklistNode *new_node = zmalloc(sizeof(quicklistNode));
        if (!new_node) {
            /* 失败时清理 */
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
        
        /* 复制节点元数据 */
        new_node->count = old_node->count;
        new_node->sz = old_node->sz;
        new_node->encoding = old_node->encoding;
        new_node->container = old_node->container;
        new_node->recompress = old_node->recompress;
        new_node->attempted_compress = old_node->attempted_compress;
        new_node->extra = old_node->extra;
        new_node->prev = prev_new_node;
        new_node->next = NULL;
        
        /* 迁移ziplist数据 */
        if (old_node->encoding == QUICKLIST_NODE_ENCODING_LZF) {
            /* LZF压缩编码 */
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
            /* 原始ziplist */
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
        
        /* 链接节点 */
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
    
    /* 释放旧quicklist */
    old_node = old_ql->head;
    while (old_node) {
        quicklistNode *next = old_node->next;
        if (old_node->zl) zfree(old_node->zl);
        zfree(old_node);
        old_node = next;
    }
    zfree(old_ql);
    
    /* 更新对象指针 */
    val_obj->ptr = new_ql;
    
    KEY_MIGRATE_LOG(LL_DEBUG, 
        "[NUMA Key Migrate] List (quicklist) migrated, %zu nodes", migrated_nodes);
    return NUMA_KEY_MIGRATE_OK;
}

/* 迁移 SET 类型 */
int migrate_set_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* 未使用参数 */
    
    if (val_obj->encoding == OBJ_ENCODING_INTSET) {
        /* Intset编码：整体迁移 */
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
        /* 哈希表编码：迁移dict及所有sds元素
         * 因SDS头部结构复杂，使用标准sds函数 */
        dict *old_dict = val_obj->ptr;
        dict *new_dict = dictCreate(old_dict->type, old_dict->privdata);
        if (!new_dict) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* 预展开以避免重哈希 */
        if (dictExpand(new_dict, dictSize(old_dict)) != DICT_OK) {
            dictRelease(new_dict);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        dictIterator *iter = dictGetIterator(old_dict);
        dictEntry *entry;
        size_t migrated_members = 0;
        
        while ((entry = dictNext(iter)) != NULL) {
            sds old_member = dictGetKey(entry);
            
            /* 使用标准分配创建新sds
             * 注：真正NUMA迁移需要NUMA感知的sds分配器 */
            sds new_member = sdsnewlen(old_member, sdslen(old_member));
            if (!new_member) {
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            
            /* 添加到新dict（set的value为NULL） */
            if (dictAdd(new_dict, new_member, NULL) != DICT_OK) {
                sdsfree(new_member);
                dictReleaseIterator(iter);
                dictRelease(new_dict);
                return NUMA_KEY_MIGRATE_ERR;
            }
            
            migrated_members++;
        }
        
        dictReleaseIterator(iter);
        
        /* 交换dict并释放旧的 */
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

/* 迁移 ZSET 类型 */
int migrate_zset_type(robj *key_obj, robj *val_obj, int target_node) {
    (void)key_obj;  /* 未使用参数 */
    (void)target_node;  /* sds分配时不直接使用 */
    
    if (val_obj->encoding == OBJ_ENCODING_ZIPLIST) {
        /* Ziplist编码：整体迁移 */
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
        /* 跳表编码：迁移zset结构、dict和跳表
         * 因SDS头部结构复杂，使用标准sds函数 */
        zset *old_zs = val_obj->ptr;
        
        /* 分配新zset结构 */
        zset *new_zs = zmalloc(sizeof(zset));
        if (!new_zs) {
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* 创建新跳表 */
        new_zs->zsl = zslCreate();
        if (!new_zs->zsl) {
            zfree(new_zs);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* 创建新dict */
        new_zs->dict = dictCreate(old_zs->dict->type, old_zs->dict->privdata);
        if (!new_zs->dict) {
            zslFree(new_zs->zsl);
            zfree(new_zs);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* 预展开dict */
        if (dictExpand(new_zs->dict, dictSize(old_zs->dict)) != DICT_OK) {
            dictRelease(new_zs->dict);
            zslFree(new_zs->zsl);
            zfree(new_zs);
            return NUMA_KEY_MIGRATE_ENOMEM;
        }
        
        /* 从尾到头遍历跳表以获得最佳插入顺序 */
        zskiplistNode *old_node = old_zs->zsl->tail;
        size_t migrated_elements = 0;
        
        while (old_node) {
            /* 使用标准sds创建新元素字符串 */
            sds old_ele = old_node->ele;
            sds new_ele = sdsnewlen(old_ele, sdslen(old_ele));
            if (!new_ele) {
                dictRelease(new_zs->dict);
                zslFree(new_zs->zsl);
                zfree(new_zs);
                return NUMA_KEY_MIGRATE_ENOMEM;
            }
            
            /* 插入新跳表 */
            zskiplistNode *new_sl_node = zslInsert(new_zs->zsl, old_node->score, new_ele);
            
            /* 添加到dict（元素 -> 分数指针） */
            dictAdd(new_zs->dict, new_ele, &new_sl_node->score);
            
            migrated_elements++;
            old_node = old_node->backward;
        }
        
        /* 释放旧zset */
        dictRelease(old_zs->dict);
        zslFree(old_zs->zsl);
        zfree(old_zs);
        
        /* 更新对象指针 */
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

/* ========== 迁移执行 ========== */

int numa_migrate_single_key(redisDb *db, robj *key, int target_node) {
    if (!global_ctx.initialized || !db || !key) {
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    if (target_node < 0 || target_node > numa_max_node()) {
        KEY_MIGRATE_LOG(LL_WARNING, 
            "[NUMA Key Migrate] Invalid target node %d", target_node);
        return NUMA_KEY_MIGRATE_EINVAL;
    }
    
    /* 在数据库中查找键 */
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
    
    /* 类型特定迁移 */
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
    
    /* 更新统计信息 */
    pthread_mutex_lock(&global_ctx.mutex);
    
    global_ctx.stats.total_migrations++;
    if (result == NUMA_KEY_MIGRATE_OK) {
        global_ctx.stats.successful_migrations++;
        
        /* 更新key元数据（已持锁，直接访问dict） */
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

/* ========== 查询接口 ========== */

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

/* ========== 公共查询接口 ========== */

int numa_key_migrate_is_initialized(void) {
    return global_ctx.initialized;
}
