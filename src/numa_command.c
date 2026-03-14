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
#include "numa_strategy_slots.h"
#include "numa_configurable_strategy.h"
#include <sched.h>
#include <numa.h>

/* ========== 外部函数声明 ========== */

extern int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
extern robj *lookupKeyRead(redisDb *db, robj *key);

/* numa_configurable_strategy 内部接口（原 numa_config_command.c 使用） */
extern const char *get_strategy_name(numa_config_strategy_type_t strategy);
extern numa_config_strategy_type_t parse_strategy_name(const char *name);

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
        addReplyLongLong(c, meta ? meta->current_node : -1);
        addReplyBulkCString(c, "hotness_level");
        addReplyLongLong(c, meta ? meta->hotness_level : 0);
        addReplyBulkCString(c, "access_count");
        addReplyLongLong(c, meta ? meta->access_count : 0);
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
        addReplyArrayLen(c, 16);
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
        addReplyArrayLen(c, cfg->num_nodes * 2);
        for (int i = 0; i < cfg->num_nodes; i++) {
            addReplyArrayLen(c, 2);
            addReplyBulkCString(c, "node");
            addReplyLongLong(c, i);
            addReplyArrayLen(c, 4);
            addReplyBulkCString(c, "allocations");
            addReplyLongLong(c, allocs[i]);
            addReplyBulkCString(c, "bytes");
            addReplyLongLong(c, bytes[i]);
        }
        zfree(allocs);
        zfree(bytes);
        return;
    }

    addReplyErrorFormat(c, "Unknown NUMA CONFIG subcommand '%s'", sub);
}

/* ========== NUMA STRATEGY 子域 ========== */

/*
 * NUMA STRATEGY SLOT <slot_id> <strategy_name>  -- 向插槽插入策略
 * NUMA STRATEGY LIST                             -- 列出所有插槽状态
 */
static void numa_cmd_strategy(client *c) {
    if (c->argc < 3) {
        addReplyError(c, "Usage: NUMA STRATEGY <SLOT|LIST> [args]");
        return;
    }

    const char *sub = c->argv[2]->ptr;

    /* NUMA STRATEGY SLOT <id> <name> */
    if (!strcasecmp(sub, "SLOT")) {
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
    addReplyArrayLen(c, 17);
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
    addReplyBulkCString(c, "NUMA CONFIG LOAD [/path]           - Hot-reload composite-lru JSON config");
    addReplyBulkCString(c, "NUMA CONFIG REBALANCE              - Trigger manual rebalance");
    addReplyBulkCString(c, "NUMA CONFIG STATS                  - Show per-node allocation statistics");
    /* STRATEGY */
    addReplyBulkCString(c, "NUMA STRATEGY SLOT <id> <name>     - Insert strategy into slot");
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
