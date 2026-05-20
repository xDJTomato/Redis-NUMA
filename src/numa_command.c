/* numa_command.c - 统一 NUMA 命令入口
 *
 * 所有 NUMA 相关 Redis 命令均在本文件实现，按领域分三个二级域：
 *
 *   NUMA MIGRATE ...   - Key 级别迁移（原 NUMAMIGRATE）
 *   NUMA CONFIG  ...   - 内存分配策略 + composite-lru JSON 配置（原 NUMACONFIG + NUMAMIGRATE CONFIG）
 *   NUMA STRATEGY ...  - 策略插槽管理（新增）
 *   NUMA HELP          - 帮助信息
 *
 * 业务逻辑（统计/迁移执行等）仍在各自模块；本文件只负责参数解析和 addReply*。
 */

#define _GNU_SOURCE
#include "server.h"
#include "numa_key_migrate.h"
#include "numa_composite_lru.h"
#include "numa_tinylfu.h"
#include "numa_strategy_slots.h"
#include "numa_configurable_strategy.h"
#include "numa_pool.h"
#include <sched.h>
#include <numa.h>

/* ========== 外部函数声明 ========== */

extern int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
extern robj *lookupKeyRead(redisDb *db, robj *key);

/* numa_configurable_strategy 内部接口（原 numa_config_command.c 使用） */
extern const char *get_strategy_name(numa_config_strategy_type_t strategy);
extern numa_config_strategy_type_t parse_strategy_name(const char *name);

/* zmalloc.c: 分配路径统计 + direct path 大对象缓存统计 */
extern void numa_get_alloc_stats(size_t *slab_bytes, size_t *pool_bytes, size_t *direct_bytes,
                                 size_t *slab_count, size_t *pool_count, size_t *direct_count);
extern void numa_get_direct_cache_stats(size_t *hit, size_t *miss, size_t *evict);

/* ========== NUMA MIGRATE 子域 ========== */

/*
 * NUMA MIGRATE KEY <key> <node>
 * NUMA MIGRATE DB <node>
 * NUMA MIGRATE SCAN [COUNT n]
 * NUMA MIGRATE STATS
 * NUMA MIGRATE RESET
 * NUMA MIGRATE INFO <key>
 */
static void numa_cmd_migrate(client *c) {
    /* argv: [0]=NUMA [1]=MIGRATE [2]=<subcmd> ... */
    if (c->argc < 3) {
        addReplyError(c, "Usage: NUMA MIGRATE <KEY|DB|SCAN|STATS|RESET|INFO> [args]");
        return;
    }

    if (!numa_key_migrate_is_initialized()) {
        addReplyError(c, "NUMA Key Migrate module not initialized");
        return;
    }

    const char *sub = c->argv[2]->ptr;

    /* NUMA MIGRATE KEY <key> <node> */
    if (!strcasecmp(sub, "KEY")) {
        if (c->argc != 5) {
            addReplyError(c, "Usage: NUMA MIGRATE KEY <key> <target_node>");
            return;
        }
        robj *key = c->argv[3];
        long target_node;
        if (getLongFromObjectOrReply(c, c->argv[4], &target_node, "Invalid target node") != C_OK)
            return;
        if (target_node < 0 || target_node > numa_max_node()) {
            addReplyErrorFormat(c, "Target node %ld out of range (0-%d)",
                target_node, numa_max_node());
            return;
        }
        int result = numa_migrate_single_key(c->db, key, (int)target_node);
        switch (result) {
            case NUMA_KEY_MIGRATE_OK:
                addReplyStatus(c, "OK");
                serverLog(LL_NOTICE,
                    "[NUMA] Key migrated to node %ld via command", target_node);
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
        return;
    }

    /* NUMA MIGRATE DB <node> */
    if (!strcasecmp(sub, "DB")) {
        if (c->argc != 4) {
            addReplyError(c, "Usage: NUMA MIGRATE DB <target_node>");
            return;
        }
        long target_node;
        if (getLongFromObjectOrReply(c, c->argv[3], &target_node, "Invalid target node") != C_OK)
            return;
        if (target_node < 0 || target_node > numa_max_node()) {
            addReplyErrorFormat(c, "Target node %ld out of range (0-%d)",
                target_node, numa_max_node());
            return;
        }
        int result = numa_migrate_entire_database(c->db, (int)target_node);
        if (result == NUMA_KEY_MIGRATE_OK) {
            addReplyStatus(c, "OK");
            serverLog(LL_NOTICE,
                "[NUMA] Database migrated to node %ld via command", target_node);
        } else {
            addReplyError(c, "Database migration failed or partially completed");
        }
        return;
    }

    /* NUMA MIGRATE STATS */
    if (!strcasecmp(sub, "STATS")) {
        numa_key_migrate_stats_t stats;
        numa_get_migration_statistics(&stats);

        /* Composite LRU 访问分布统计 */
        uint64_t acc_local = 0, acc_remote = 0;
        numa_strategy_t *clru = numa_strategy_slot_get(1);
        if (clru && clru->private_data) {
            composite_lru_data_t *d = clru->private_data;
            acc_local  = d->accesses_local;
            acc_remote = d->accesses_remote;
        }

        /* TinyLFU 统计 */
        uint64_t tlfu_accesses = 0, tlfu_filtered = 0, tlfu_enqueued = 0;
        uint64_t tlfu_migrated = 0, tlfu_failed = 0, tlfu_resets = 0;
        uint64_t tlfu_acc_local = 0, tlfu_acc_remote = 0;
        uint64_t tlfu_acc_node0 = 0, tlfu_acc_node1 = 0, tlfu_acc_node2 = 0, tlfu_acc_node3 = 0;
        uint64_t tlfu_acc_unknown = 0;
        int tlfu_enabled = 0;
        numa_strategy_t *tlfu = numa_strategy_slot_get(2);
        if (tlfu && tlfu->private_data) {
            tinylfu_data_t *td = tlfu->private_data;
            tlfu_enabled = tlfu->enabled;
            tlfu_accesses = td->stat_accesses;
            tlfu_filtered = td->stat_doorkeeper_filtered;
            tlfu_enqueued = td->stat_candidates_enqueued;
            tlfu_migrated = td->stat_migrations_done;
            tlfu_failed   = td->stat_migrations_failed;
            tlfu_resets   = td->stat_resets;
            tlfu_acc_local  = td->stat_accesses_local;
            tlfu_acc_remote = td->stat_accesses_remote;
            tlfu_acc_node0  = td->stat_accesses_node0;
            tlfu_acc_node1  = td->stat_accesses_node1;
            tlfu_acc_node2  = td->stat_accesses_node2;
            tlfu_acc_node3  = td->stat_accesses_node3;
            tlfu_acc_unknown = td->stat_accesses_unknown;
        }

        addReplyArrayLen(c, 50);
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
        addReplyBulkCString(c, "accesses_local");
        addReplyLongLong(c, acc_local);
        addReplyBulkCString(c, "accesses_remote");
        addReplyLongLong(c, acc_remote);
        {
            extern redisAtomic unsigned long long dboverwrite_realloc_count;
            extern redisAtomic unsigned long long dboverwrite_check_count;
            extern redisAtomic unsigned long long dbset_overwrite_seen_count;
            unsigned long long rc, cc, sc;
            atomicGet(dboverwrite_realloc_count, rc);
            atomicGet(dboverwrite_check_count, cc);
            atomicGet(dbset_overwrite_seen_count, sc);
            addReplyBulkCString(c, "dboverwrite_checks");
            addReplyLongLong(c, cc);
            addReplyBulkCString(c, "dboverwrite_reallocs");
            addReplyLongLong(c, rc);
            addReplyBulkCString(c, "dbset_overwrite_seen");
            addReplyLongLong(c, sc);
        }
        addReplyBulkCString(c, "tinylfu_enabled");
        addReplyLongLong(c, tlfu_enabled);
        addReplyBulkCString(c, "tinylfu_accesses");
        addReplyLongLong(c, tlfu_accesses);
        addReplyBulkCString(c, "tinylfu_doorkeeper_filtered");
        addReplyLongLong(c, tlfu_filtered);
        addReplyBulkCString(c, "tinylfu_candidates_enqueued");
        addReplyLongLong(c, tlfu_enqueued);
        addReplyBulkCString(c, "tinylfu_migrations_done");
        addReplyLongLong(c, tlfu_migrated);
        addReplyBulkCString(c, "tinylfu_migrations_failed");
        addReplyLongLong(c, tlfu_failed);
        addReplyBulkCString(c, "tinylfu_resets");
        addReplyLongLong(c, tlfu_resets);
        addReplyBulkCString(c, "tinylfu_accesses_local");
        addReplyLongLong(c, tlfu_acc_local);
        addReplyBulkCString(c, "tinylfu_accesses_remote");
        addReplyLongLong(c, tlfu_acc_remote);
        addReplyBulkCString(c, "tinylfu_accesses_node0");
        addReplyLongLong(c, tlfu_acc_node0);
        addReplyBulkCString(c, "tinylfu_accesses_node1");
        addReplyLongLong(c, tlfu_acc_node1);
        addReplyBulkCString(c, "tinylfu_accesses_node2");
        addReplyLongLong(c, tlfu_acc_node2);
        addReplyBulkCString(c, "tinylfu_accesses_node3");
        addReplyLongLong(c, tlfu_acc_node3);
        addReplyBulkCString(c, "tinylfu_accesses_unknown");
        addReplyLongLong(c, tlfu_acc_unknown);
        return;
    }

    /* NUMA MIGRATE RESET */
    if (!strcasecmp(sub, "RESET")) {
        numa_reset_migration_statistics();
        addReplyStatus(c, "OK");
        return;
    }

    /* NUMA MIGRATE INFO <key> */
    if (!strcasecmp(sub, "INFO")) {
        if (c->argc != 4) {
            addReplyError(c, "Usage: NUMA MIGRATE INFO <key>");
            return;
        }
        robj *key = c->argv[3];
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
            case OBJ_LIST:   type_name = "list";   break;
            case OBJ_SET:    type_name = "set";    break;
            case OBJ_ZSET:   type_name = "zset";   break;
            case OBJ_HASH:   type_name = "hash";   break;
            default:         type_name = "unknown"; break;
        }
        addReplyBulkCString(c, type_name);
        addReplyBulkCString(c, "current_node");
        {
            int node = -1;
            if (val->encoding == OBJ_ENCODING_RAW && val->ptr)
                node = numa_get_node_id(sdsAllocPtr(val->ptr));
            else if (val->encoding != OBJ_ENCODING_INT &&
                     val->encoding != OBJ_ENCODING_EMBSTR && val->ptr)
                node = numa_get_node_id(val->ptr);
            addReplyLongLong(c, node);
        }
        addReplyBulkCString(c, "hotness_level");
        {
            uint8_t h = 0;
            if (val->encoding == OBJ_ENCODING_RAW && val->ptr)
                h = numa_get_hotness(sdsAllocPtr(val->ptr));
            else if (val->encoding != OBJ_ENCODING_INT &&
                     val->encoding != OBJ_ENCODING_EMBSTR && val->ptr)
                h = numa_get_hotness(val->ptr);
            addReplyLongLong(c, h);
        }
        addReplyBulkCString(c, "access_count");
        {
            uint8_t ac = 0;
            if (val->encoding == OBJ_ENCODING_RAW && val->ptr)
                ac = numa_get_access_count(sdsAllocPtr(val->ptr));
            else if (val->encoding != OBJ_ENCODING_INT &&
                     val->encoding != OBJ_ENCODING_EMBSTR && val->ptr)
                ac = numa_get_access_count(val->ptr);
            addReplyLongLong(c, ac);
        }
        addReplyBulkCString(c, "numa_nodes_available");
        addReplyLongLong(c, numa_max_node() + 1);
        addReplyBulkCString(c, "current_cpu_node");
        int cpu = sched_getcpu();
        addReplyLongLong(c, (cpu >= 0) ? numa_node_of_cpu(cpu) : 0);
        return;
    }

    /* NUMA MIGRATE SCAN [COUNT n] */
    if (!strcasecmp(sub, "SCAN")) {
        uint32_t batch = 0;
        /* argv: NUMA MIGRATE SCAN [COUNT n] → argc 3 or 5 */
        if (c->argc >= 5 && !strcasecmp(c->argv[3]->ptr, "COUNT")) {
            long cnt;
            if (getLongFromObjectOrReply(c, c->argv[4], &cnt, "Invalid COUNT") != C_OK)
                return;
            if (cnt <= 0) {
                addReplyError(c, "COUNT must be positive");
                return;
            }
            batch = (uint32_t)cnt;
        } else if (c->argc != 3) {
            addReplyError(c, "Usage: NUMA MIGRATE SCAN [COUNT n]");
            return;
        }
        numa_strategy_t *strat = numa_strategy_slot_get(1);
        if (!strat) {
            addReplyError(c, "No active strategy on slot 1");
            return;
        }
        if (batch == 0) {
            composite_lru_data_t *d = strat->private_data;
            batch = (d && d->config.scan_batch_size) ? d->config.scan_batch_size : 200;
        }
        uint64_t scanned = 0, migrated = 0;
        if (composite_lru_scan_once(strat, batch, &scanned, &migrated) != NUMA_STRATEGY_OK) {
            addReplyError(c, "Scan failed");
            return;
        }
        addReplyArrayLen(c, 4);
        addReplyBulkCString(c, "scanned");
        addReplyLongLong(c, (long long)scanned);
        addReplyBulkCString(c, "migrated");
        addReplyLongLong(c, (long long)migrated);
        return;
    }

    addReplyErrorFormat(c, "Unknown NUMA MIGRATE subcommand '%s'", sub);
}

/* ========== NUMA CONFIG 子域 ========== */

/*
 * NUMA CONFIG GET
 * NUMA CONFIG SET <param> <val> [val2]
 * NUMA CONFIG LOAD [/path]          -- composite-lru JSON 热加载
 * NUMA CONFIG REBALANCE
 * NUMA CONFIG STATS
 */
static void numa_cmd_config(client *c) {
    /* argv: [0]=NUMA [1]=CONFIG [2]=<subcmd> ... */
    if (c->argc < 3) {
        addReplyError(c, "Usage: NUMA CONFIG <GET|SET|LOAD|REBALANCE|STATS> [args]");
        return;
    }

    const char *sub = c->argv[2]->ptr;

    /* NUMA CONFIG GET */
    if (!strcasecmp(sub, "GET")) {
        if (numa_config_strategy_init() != C_OK) {
            addReplyError(c, "Failed to initialize NUMA configurable strategy system");
            return;
        }
        const numa_strategy_config_t *cfg = numa_config_get_current();
        if (!cfg) {
            addReplyError(c, "NUMA configuration not available");
            return;
        }
        addReplyArrayLen(c, 24);
        addReplyBulkCString(c, "strategy");
        addReplyBulkCString(c, get_strategy_name(cfg->strategy_type));
        addReplyBulkCString(c, "nodes");
        addReplyLongLong(c, cfg->num_nodes);
        addReplyBulkCString(c, "balance_threshold");
        addReplyLongLong(c, (long long)(cfg->balance_threshold * 100));
        addReplyBulkCString(c, "auto_rebalance");
        addReplyBulkCString(c, cfg->auto_rebalance ? "yes" : "no");
        addReplyBulkCString(c, "cxl_optimization");
        addReplyBulkCString(c, cfg->enable_cxl_optimization ? "enabled" : "disabled");
        addReplyBulkCString(c, "rebalance_interval");
        addReplyLongLong(c, cfg->rebalance_interval_us);
        addReplyBulkCString(c, "min_allocation_size");
        addReplyLongLong(c, cfg->min_allocation_size);
        addReplyBulkCString(c, "enabled_nodes");
        uint64_t enabled_mask = numa_config_get_enabled_nodes_mask();
        if (enabled_mask == 0) {
            addReplyBulkCString(c, "all");
        } else {
            sds nodes = sdsempty();
            for (int i = 0; i < cfg->num_nodes && i < 64; i++) {
                if (!(enabled_mask & (1ULL << i))) continue;
                if (sdslen(nodes)) nodes = sdscatlen(nodes, ",", 1);
                nodes = sdscatfmt(nodes, "%i", i);
            }
            addReplyBulkCBuffer(c, nodes, sdslen(nodes));
            sdsfree(nodes);
        }
        numa_strategy_t *strat = numa_strategy_slot_get(1);
        char vbuf[64];
        addReplyBulkCString(c, "access_tracking_enabled");
        if (strat && composite_lru_get_config(strat, "access_tracking_enabled", vbuf, sizeof(vbuf)) == NUMA_STRATEGY_OK)
            addReplyBulkCString(c, vbuf);
        else
            addReplyBulkCString(c, "unavailable");
        addReplyBulkCString(c, "locality_stats_enabled");
        if (strat && composite_lru_get_config(strat, "locality_stats_enabled", vbuf, sizeof(vbuf)) == NUMA_STRATEGY_OK)
            addReplyBulkCString(c, vbuf);
        else
            addReplyBulkCString(c, "unavailable");
        addReplyBulkCString(c, "debug_logging_enabled");
        if (strat && composite_lru_get_config(strat, "debug_logging_enabled", vbuf, sizeof(vbuf)) == NUMA_STRATEGY_OK)
            addReplyBulkCString(c, vbuf);
        else
            addReplyBulkCString(c, "unavailable");
        addReplyBulkCString(c, "node_weights");
        addReplyArrayLen(c, cfg->num_nodes);
        for (int i = 0; i < cfg->num_nodes; i++) {
            addReplyArrayLen(c, 2);
            addReplyLongLong(c, i);
            addReplyLongLong(c, cfg->node_weights ? cfg->node_weights[i] : 100);
        }
        return;
    }

    /* NUMA CONFIG SET <param> <val> [val2] */
    if (!strcasecmp(sub, "SET")) {
        if (c->argc < 5) {
            addReplyError(c, "Usage: NUMA CONFIG SET <parameter> <value> [value2]");
            return;
        }
        if (numa_config_strategy_init() != C_OK) {
            addReplyError(c, "Failed to initialize NUMA configurable strategy system");
            return;
        }
        const char *param = c->argv[3]->ptr;
        const char *val   = c->argv[4]->ptr;

        if (!strcasecmp(param, "strategy")) {
            numa_config_strategy_type_t st = parse_strategy_name(val);
            if ((int)st < 0) {
                addReplyErrorFormat(c, "Unknown strategy name: %s", val);
                return;
            }
            if (numa_config_set_strategy(st) == C_OK)
                addReplyStatus(c, "OK");
            else
                addReplyError(c, "Failed to set strategy");
            return;
        }
        if (!strcasecmp(param, "cxl_optimization")) {
            int enable = (!strcasecmp(val, "on") || !strcasecmp(val, "yes") || atoi(val));
            if (numa_config_set_cxl_optimization(enable) == C_OK)
                addReplyStatus(c, "OK");
            else
                addReplyError(c, "Failed to set CXL optimization");
            return;
        }
        if (!strcasecmp(param, "balance_threshold")) {
            double thr = atof(val) / 100.0;
            if (thr < 0.0 || thr > 1.0) {
                addReplyError(c, "Balance threshold must be between 0 and 100");
                return;
            }
            if (numa_config_set_balance_threshold(thr) == C_OK)
                addReplyStatus(c, "OK");
            else
                addReplyError(c, "Failed to set balance threshold");
            return;
        }
        if (!strcasecmp(param, "weight")) {
            /* NUMA CONFIG SET weight <node> <weight> → argc=6 */
            if (c->argc < 6) {
                addReplyError(c, "Usage: NUMA CONFIG SET weight <node_id> <weight>");
                return;
            }
            long node_id, weight;
            if (getLongFromObjectOrReply(c, c->argv[4], &node_id, "Invalid node ID") != C_OK ||
                getLongFromObjectOrReply(c, c->argv[5], &weight,  "Invalid weight")  != C_OK)
                return;
            const numa_strategy_config_t *cur = numa_config_get_current();
            if (!cur || node_id < 0 || node_id >= cur->num_nodes) {
                addReplyErrorFormat(c, "Node ID %ld out of range", node_id);
                return;
            }
            if (weight < 0 || weight > 1000) {
                addReplyError(c, "Weight must be between 0 and 1000");
                return;
            }
            int *nw = zmalloc(cur->num_nodes * sizeof(int));
            if (!nw) { addReplyError(c, "Memory allocation failed"); return; }
            if (cur->node_weights)
                memcpy(nw, cur->node_weights, cur->num_nodes * sizeof(int));
            else
                for (int i = 0; i < cur->num_nodes; i++) nw[i] = 100;
            nw[node_id] = (int)weight;
            int ret = numa_config_set_node_weights(nw, cur->num_nodes);
            zfree(nw);
            if (ret == C_OK) addReplyStatus(c, "OK");
            else addReplyError(c, "Failed to set node weight");
            return;
        }
        if (!strcasecmp(param, "enabled_nodes")) {
            uint64_t mask = 0;
            const numa_strategy_config_t *cur = numa_config_get_current();
            if (!cur) {
                addReplyError(c, "NUMA configuration not available");
                return;
            }
            if (!strcasecmp(val, "all")) {
                mask = 0;
            } else {
                char *spec = zstrdup(val);
                char *saveptr = NULL;
                char *tok = strtok_r(spec, ",", &saveptr);
                while (tok) {
                    char *end = NULL;
                    long node = strtol(tok, &end, 10);
                    if (*tok == '\0' || *end != '\0' || node < 0 || node >= cur->num_nodes || node >= 64) {
                        zfree(spec);
                        addReplyErrorFormat(c, "Invalid enabled_nodes entry: %s", tok);
                        return;
                    }
                    mask |= (1ULL << node);
                    tok = strtok_r(NULL, ",", &saveptr);
                }
                zfree(spec);
                if (mask == 0) {
                    addReplyError(c, "enabled_nodes must be 'all' or a non-empty comma-separated node list");
                    return;
                }
            }
            if (numa_config_set_enabled_nodes_mask(mask) == C_OK)
                addReplyStatus(c, "OK");
            else
                addReplyError(c, "Failed to set enabled nodes");
            return;
        }
        if (!strcasecmp(param, "access_tracking") ||
            !strcasecmp(param, "access_tracking_enabled") ||
            !strcasecmp(param, "locality_stats") ||
            !strcasecmp(param, "locality_stats_enabled") ||
            !strcasecmp(param, "debug_logging") ||
            !strcasecmp(param, "debug_logging_enabled") ||
            !strcasecmp(param, "auto_migrate_enabled")) {
            numa_strategy_t *strat = numa_strategy_slot_get(1);
            if (!strat) {
                addReplyError(c, "No active strategy on slot 1");
                return;
            }
            char normalized[64];
            const char *key = param;
            if (!strcasecmp(param, "access_tracking")) key = "access_tracking_enabled";
            if (!strcasecmp(param, "locality_stats")) key = "locality_stats_enabled";
            if (!strcasecmp(param, "debug_logging")) key = "debug_logging_enabled";
            snprintf(normalized, sizeof(normalized), "%s", key);
            if (composite_lru_set_config(strat, normalized, val) == NUMA_STRATEGY_OK)
                addReplyStatus(c, "OK");
            else
                addReplyError(c, "Failed to set composite-lru config");
            return;
        }
        addReplyErrorFormat(c, "Unknown NUMA CONFIG SET parameter: %s", param);
        return;
    }

    /* NUMA CONFIG LOAD [/path] -- composite-lru JSON 热加载 */
    if (!strcasecmp(sub, "LOAD")) {
        const char *path = (c->argc >= 4) ? c->argv[3]->ptr : NULL;
#ifdef HAVE_NUMA
        if (!path) path = server.numa_migrate_config_file;
#endif
        if (!path) {
            addReplyError(c, "No config file path specified and none configured");
            return;
        }
        numa_strategy_t *strat = numa_strategy_slot_get(1);
        if (!strat) {
            addReplyError(c, "No active strategy on slot 1");
            return;
        }
        composite_lru_config_t cfg;
        if (composite_lru_load_config(path, &cfg) != NUMA_STRATEGY_OK) {
            addReplyErrorFormat(c, "Failed to load config from: %s", path);
            return;
        }
        if (composite_lru_apply_config(strat, &cfg) != NUMA_STRATEGY_OK) {
            addReplyError(c, "Failed to apply config");
            return;
        }
        addReplyStatus(c, "OK");
        serverLog(LL_NOTICE, "[NUMA] composite-lru config hot-reloaded from: %s", path);
        return;
    }

    /* NUMA CONFIG REBALANCE */
    if (!strcasecmp(sub, "REBALANCE")) {
        if (numa_config_strategy_init() != C_OK) {
            addReplyError(c, "Failed to initialize NUMA configurable strategy system");
            return;
        }
        if (numa_config_trigger_rebalance() == C_OK)
            addReplyStatus(c, "OK");
        else
            addReplyError(c, "Failed to trigger rebalance");
        return;
    }

    /* NUMA CONFIG STATS */
    if (!strcasecmp(sub, "STATS")) {
        if (numa_config_strategy_init() != C_OK) {
            addReplyError(c, "Failed to initialize NUMA configurable strategy system");
            return;
        }
        const numa_strategy_config_t *cfg = numa_config_get_current();
        if (!cfg) {
            addReplyError(c, "NUMA configuration not available");
            return;
        }
        uint64_t *allocs = zcalloc(cfg->num_nodes * sizeof(uint64_t));
        size_t   *bytes  = zcalloc(cfg->num_nodes * sizeof(size_t));
        if (!allocs || !bytes) {
            zfree(allocs); zfree(bytes);
            addReplyError(c, "Memory allocation failed");
            return;
        }
        numa_config_get_statistics(allocs, bytes, cfg->num_nodes);

        /* 扁平 key-value 输出：每节点 3 个字段 + 4 个路径统计 + 3 个 direct cache 统计 = 3*num_nodes + 7 */
        int total_fields = cfg->num_nodes * 3 + 7;
        addReplyArrayLen(c, total_fields * 2);

        for (int i = 0; i < cfg->num_nodes; i++) {
            char key[64];
            snprintf(key, sizeof(key), "node%d_allocations", i);
            addReplyBulkCString(c, key);
            addReplyLongLong(c, allocs[i]);

            snprintf(key, sizeof(key), "node%d_bytes", i);
            addReplyBulkCString(c, key);
            addReplyLongLong(c, bytes[i]);

            snprintf(key, sizeof(key), "node%d_live", i);
            addReplyBulkCString(c, key);
            addReplyLongLong(c, zmalloc_used_memory_node(i));
        }
        zfree(allocs);
        zfree(bytes);

        /* 分配路径统计 */
        size_t slab_bytes, pool_bytes, direct_bytes;
        size_t slab_count, pool_count, direct_count;
        numa_get_alloc_stats(&slab_bytes, &pool_bytes, &direct_bytes,
                             &slab_count, &pool_count, &direct_count);

        addReplyBulkCString(c, "alloc_slab_bytes");
        addReplyLongLong(c, slab_bytes);
        addReplyBulkCString(c, "alloc_direct_bytes");
        addReplyLongLong(c, direct_bytes);
        addReplyBulkCString(c, "alloc_slab_count");
        addReplyLongLong(c, slab_count);
        addReplyBulkCString(c, "alloc_direct_count");
        addReplyLongLong(c, direct_count);

        /* Direct path 大对象缓存统计 */
        size_t dc_hit, dc_miss, dc_evict;
        numa_get_direct_cache_stats(&dc_hit, &dc_miss, &dc_evict);
        addReplyBulkCString(c, "direct_cache_hit");
        addReplyLongLong(c, dc_hit);
        addReplyBulkCString(c, "direct_cache_miss");
        addReplyLongLong(c, dc_miss);
        addReplyBulkCString(c, "direct_cache_evict");
        addReplyLongLong(c, dc_evict);
        return;
    }

    addReplyErrorFormat(c, "Unknown NUMA CONFIG subcommand '%s'", sub);
}

/* ========== NUMA STRATEGY 子域 ========== */

/*
 * NUMA STRATEGY SLOT <slot_id> <strategy_name>  -- 向插槽插入策略
 * NUMA STRATEGY SLOT ENABLE <slot_id>           -- 启用插槽
 * NUMA STRATEGY SLOT DISABLE <slot_id>          -- 禁用插槽
 * NUMA STRATEGY LIST                             -- 列出所有插槽状态
 */
static void numa_cmd_strategy(client *c) {
    if (c->argc < 3) {
        addReplyError(c, "Usage: NUMA STRATEGY <SLOT|LIST> [args]");
        return;
    }

    const char *sub = c->argv[2]->ptr;

    /* NUMA STRATEGY SLOT ... */
    if (!strcasecmp(sub, "SLOT")) {
        if (c->argc < 4) {
            addReplyError(c, "Usage: NUMA STRATEGY SLOT <ENABLE|DISABLE|slot_id> ...");
            return;
        }

        const char *arg3 = c->argv[3]->ptr;

        /* NUMA STRATEGY SLOT ENABLE <slot_id> */
        if (!strcasecmp(arg3, "ENABLE")) {
            if (c->argc != 5) {
                addReplyError(c, "Usage: NUMA STRATEGY SLOT ENABLE <slot_id>");
                return;
            }
            long slot_id;
            if (getLongFromObjectOrReply(c, c->argv[4], &slot_id, "Invalid slot ID") != C_OK)
                return;
            int ret = numa_strategy_slot_enable((int)slot_id);
            if (ret == NUMA_STRATEGY_OK)
                addReplyStatus(c, "OK");
            else
                addReplyErrorFormat(c, "Failed to enable slot %ld (err=%d)", slot_id, ret);
            return;
        }

        /* NUMA STRATEGY SLOT DISABLE <slot_id> */
        if (!strcasecmp(arg3, "DISABLE")) {
            if (c->argc != 5) {
                addReplyError(c, "Usage: NUMA STRATEGY SLOT DISABLE <slot_id>");
                return;
            }
            long slot_id;
            if (getLongFromObjectOrReply(c, c->argv[4], &slot_id, "Invalid slot ID") != C_OK)
                return;
            int ret = numa_strategy_slot_disable((int)slot_id);
            if (ret == NUMA_STRATEGY_OK)
                addReplyStatus(c, "OK");
            else
                addReplyErrorFormat(c, "Failed to disable slot %ld (err=%d)", slot_id, ret);
            return;
        }

        /* NUMA STRATEGY SLOT <id> <name> — 向插槽插入策略 */
        if (c->argc != 5) {
            addReplyError(c, "Usage: NUMA STRATEGY SLOT <slot_id> <strategy_name>");
            return;
        }
        long slot_id;
        if (getLongFromObjectOrReply(c, c->argv[3], &slot_id, "Invalid slot ID") != C_OK)
            return;
        const char *name = c->argv[4]->ptr;
        int ret = numa_strategy_slot_insert((int)slot_id, name);
        if (ret == NUMA_STRATEGY_OK)
            addReplyStatus(c, "OK");
        else
            addReplyErrorFormat(c, "Failed to insert strategy '%s' into slot %ld (err=%d)",
                name, slot_id, ret);
        return;
    }

    /* NUMA STRATEGY LIST */
    if (!strcasecmp(sub, "LIST")) {
        char buf[4096];
        if (numa_strategy_slot_list(buf, sizeof(buf)) == NUMA_STRATEGY_OK) {
            addReplyBulkCString(c, buf);
        } else {
            addReplyBulkCString(c, "(no strategies registered)");
        }
        return;
    }

    addReplyErrorFormat(c, "Unknown NUMA STRATEGY subcommand '%s'", sub);
}

/* ========== NUMA HELP ========== */

static void numa_cmd_help(client *c) {
    addReplyArrayLen(c, 23);
    /* MIGRATE */
    addReplyBulkCString(c, "NUMA MIGRATE KEY <key> <node>      - Migrate a key to target NUMA node");
    addReplyBulkCString(c, "NUMA MIGRATE DB <node>             - Migrate entire database to target NUMA node");
    addReplyBulkCString(c, "NUMA MIGRATE SCAN [COUNT n]        - Trigger one round of progressive key scan");
    addReplyBulkCString(c, "NUMA MIGRATE STATS                 - Show migration statistics");
    addReplyBulkCString(c, "NUMA MIGRATE RESET                 - Reset migration statistics");
    addReplyBulkCString(c, "NUMA MIGRATE INFO <key>            - Get NUMA metadata for a key");
    /* CONFIG */
    addReplyBulkCString(c, "NUMA CONFIG GET                    - Show current allocator config");
    addReplyBulkCString(c, "NUMA CONFIG SET strategy <name>    - Set allocation strategy");
    addReplyBulkCString(c, "NUMA CONFIG SET weight <node> <w>  - Set node weight");
    addReplyBulkCString(c, "NUMA CONFIG SET cxl_optimization <on|off>");
    addReplyBulkCString(c, "NUMA CONFIG SET balance_threshold <percent>");
    addReplyBulkCString(c, "NUMA CONFIG SET access_tracking <0|1>");
    addReplyBulkCString(c, "NUMA CONFIG SET locality_stats <0|1>");
    addReplyBulkCString(c, "NUMA CONFIG SET debug_logging <0|1>");
    addReplyBulkCString(c, "NUMA CONFIG SET enabled_nodes <all|n[,m]>");
    addReplyBulkCString(c, "NUMA CONFIG LOAD [/path]           - Hot-reload composite-lru JSON config");
    addReplyBulkCString(c, "NUMA CONFIG REBALANCE              - Trigger manual rebalance");
    addReplyBulkCString(c, "NUMA CONFIG STATS                  - Show per-node allocation statistics");
    /* STRATEGY */
    addReplyBulkCString(c, "NUMA STRATEGY SLOT <id> <name>     - Insert strategy into slot");
    addReplyBulkCString(c, "NUMA STRATEGY SLOT ENABLE <id>     - Enable a strategy slot");
    addReplyBulkCString(c, "NUMA STRATEGY SLOT DISABLE <id>    - Disable a strategy slot");
    addReplyBulkCString(c, "NUMA STRATEGY LIST                 - List all registered strategy slots");
    /* HELP */
    addReplyBulkCString(c, "NUMA HELP                          - Show this help message");
}

/* ========== 顶层入口 ========== */

/*
 * numaCommand - NUMA 命令顶层路由
 *
 * 用法：NUMA <MIGRATE|CONFIG|STRATEGY|HELP> [subcommand] [args...]
 */
void numaCommand(client *c) {
    if (c->argc < 2) {
        addReplyError(c, "Usage: NUMA <MIGRATE|CONFIG|STRATEGY|HELP> [args...]");
        return;
    }

    const char *domain = c->argv[1]->ptr;

    if (!strcasecmp(domain, "MIGRATE")) {
        numa_cmd_migrate(c);
    } else if (!strcasecmp(domain, "CONFIG")) {
        numa_cmd_config(c);
    } else if (!strcasecmp(domain, "STRATEGY")) {
        numa_cmd_strategy(c);
    } else if (!strcasecmp(domain, "HELP")) {
        numa_cmd_help(c);
    } else {
        addReplyErrorFormat(c,
            "Unknown NUMA domain '%s'. Try NUMA HELP.", domain);
    }
}
