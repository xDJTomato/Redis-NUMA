/* numa_command.c - unified NUMA command entry point
 *
 * All NUMA-related Redis commands are implemented in this file, organized into sub-domains:
 *
 *   NUMA MIGRATE ...   - key-level migration (formerly NUMAMIGRATE)
 *   NUMA CONFIG  ...   - memory allocation policy (formerly NUMACONFIG)
 *   NUMA FLOW     ...  - NUMAflow DAG workflow load/run/list/status/default
 *   NUMA HELP          - help
 *
 * Migration strategy (caat/composite_lru/tinylfu/noop) lives entirely in the
 * NUMAflow atomic-op engine (numaflow/src/nf_strategy.c) as of the retirement
 * of the numa_strategy_slots vtable framework; NUMA FLOW is its only command
 * surface, including switching the auto-loaded default via NUMA FLOW DEFAULT.
 *
 * Business logic (statistics/migration execution, etc.) stays in its own modules; this file only handles argument parsing and addReply*.
 */

#define _GNU_SOURCE
#include "server.h"
#include "numa_key_migrate.h"
#include "numa_configurable_strategy.h"
#include "numa_flow.h"
#include "numa_pool.h"
#include <sched.h>
#include <numa.h>

/* ========== External function declarations ========== */

extern int getLongFromObjectOrReply(client *c, robj *o, long *target, const char *msg);
extern robj *lookupKeyRead(redisDb *db, robj *key);

/* numa_configurable_strategy internal interface (used by the former numa_config_command.c). */
extern const char *get_strategy_name(numa_config_strategy_type_t strategy);
extern numa_config_strategy_type_t parse_strategy_name(const char *name);

/* zmalloc.c: allocation path stats + direct path large-object cache stats. */
extern void numa_get_alloc_stats(size_t *slab_bytes, size_t *pool_bytes, size_t *direct_bytes,
                                 size_t *slab_count, size_t *pool_count, size_t *direct_count);
extern void numa_get_direct_cache_stats(size_t *hit, size_t *miss, size_t *evict);

/* ========== NUMA MIGRATE sub-domain ========== */

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

        addReplyArrayLen(c, 18);
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

        /* For INT/EMBSTR strings the payload lives inside the robj allocation;
         * fall back to the robj base in that case.  For RAW strings and the
         * other types, sample the actual data allocation. */
        void *sample = numa_object_sample_alloc_ptr(val);
        if (!sample) sample = val;

        addReplyBulkCString(c, "current_node");
        addReplyLongLong(c, sample ? numa_get_node_id(sample) : -1);
        addReplyBulkCString(c, "hotness_level");
        addReplyLongLong(c, sample ? numa_get_hotness(sample) : 0);
        addReplyBulkCString(c, "access_count");
        addReplyLongLong(c, sample ? numa_get_access_count(sample) : 0);
        addReplyBulkCString(c, "numa_nodes_available");
        addReplyLongLong(c, numa_max_node() + 1);
        addReplyBulkCString(c, "current_cpu_node");
        int cpu = sched_getcpu();
        addReplyLongLong(c, (cpu >= 0) ? numa_node_of_cpu(cpu) : 0);
        return;
    }

    /* NUMA MIGRATE SCAN -- runs the "default" NUMAflow entry once on demand.
     * COUNT is accepted for CLI compatibility but ignored: NUMAflow's per-run
     * budget is a property of the workflow graph, not of this command. */
    if (!strcasecmp(sub, "SCAN")) {
        if (c->argc == 5 && strcasecmp(c->argv[3]->ptr, "COUNT") != 0) {
            addReplyError(c, "Usage: NUMA MIGRATE SCAN [COUNT n]");
            return;
        } else if (c->argc != 3 && c->argc != 5) {
            addReplyError(c, "Usage: NUMA MIGRATE SCAN [COUNT n]");
            return;
        }
        uint64_t scanned = 0, migrated = 0;
        if (numa_flow_run_default(&scanned, &migrated) != C_OK) {
            addReplyError(c, "No default NUMA FLOW strategy loaded");
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

/* ========== NUMA CONFIG sub-domain ========== */

/*
 * NUMA CONFIG GET
 * NUMA CONFIG SET <param> <val> [val2]
 * NUMA CONFIG LOAD [/path]          -- hot-load the composite-lru JSON
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
        addReplyArrayLen(c, 20);
        addReplyBulkCString(c, "strategy");
        addReplyBulkCString(c, get_strategy_name(cfg->strategy_type));
        addReplyBulkCString(c, "strategy_note");
        if (cfg->strategy_type == NUMA_STRATEGY_CONFIG_ADAPTIVE ||
            cfg->strategy_type == NUMA_STRATEGY_CONFIG_LATENCY_AWARE) {
            addReplyBulkCString(c,
                "placeholder in the kernel allocator (behaves as LOCAL_FIRST); "
                "full implementation is the matching alloc_* op in NUMAflow, "
                "load it via NUMA FLOW LOAD");
        } else {
            addReplyBulkCString(c, "");
        }
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
        addReplyErrorFormat(c, "Unknown NUMA CONFIG SET parameter: %s", param);
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

        /* Flat key-value output: 3 fields per node + 4 path stats + 3 direct cache stats = 3*num_nodes + 7. */
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

        /* Allocation path stats. */
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

        /* Direct path large-object cache stats. */
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

/* ========== NUMA HELP ========== */

static void numa_cmd_help(client *c) {
    addReplyArrayLen(c, 22);
    /* MIGRATE */
    addReplyBulkCString(c, "NUMA MIGRATE KEY <key> <node>      - Migrate a key to target NUMA node");
    addReplyBulkCString(c, "NUMA MIGRATE DB <node>             - Migrate entire database to target NUMA node");
    addReplyBulkCString(c, "NUMA MIGRATE SCAN [COUNT n]        - Run the default NUMA FLOW strategy once");
    addReplyBulkCString(c, "NUMA MIGRATE STATS                 - Show migration statistics");
    addReplyBulkCString(c, "NUMA MIGRATE RESET                 - Reset migration statistics");
    addReplyBulkCString(c, "NUMA MIGRATE INFO <key>            - Get NUMA metadata for a key");
    /* CONFIG */
    addReplyBulkCString(c, "NUMA CONFIG GET                    - Show current allocator config");
    addReplyBulkCString(c, "NUMA CONFIG SET strategy <name>    - Set allocation strategy");
    addReplyBulkCString(c, "NUMA CONFIG SET weight <node> <w>  - Set node weight");
    addReplyBulkCString(c, "NUMA CONFIG SET cxl_optimization <on|off>");
    addReplyBulkCString(c, "NUMA CONFIG SET balance_threshold <percent>");
    addReplyBulkCString(c, "NUMA CONFIG SET enabled_nodes <all|n[,m]>");
    addReplyBulkCString(c, "NUMA CONFIG REBALANCE              - Trigger manual rebalance");
    addReplyBulkCString(c, "NUMA CONFIG STATS                  - Show per-node allocation statistics");
    /* FLOW */
    addReplyBulkCString(c, "NUMA FLOW LOAD <name> <path> [interval_sec] [ADAPT] - Load a workflow");
    addReplyBulkCString(c, "NUMA FLOW RUN [name]               - Run one or all loaded workflows");
    addReplyBulkCString(c, "NUMA FLOW LIST                     - List loaded workflows");
    addReplyBulkCString(c, "NUMA FLOW STATUS <name>            - Show one workflow's last-run stats");
    addReplyBulkCString(c, "NUMA FLOW UNLOAD <name>            - Unload a workflow");
    addReplyBulkCString(c, "NUMA FLOW ADAPT <name> <ON|OFF>    - Toggle self-adaptation for a workflow");
    addReplyBulkCString(c, "NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop> - Switch the default strategy");
    /* HELP */
    addReplyBulkCString(c, "NUMA HELP                          - Show this help message");
}

/* ========== Top-level entry point ========== */

/*
 * numaCommand - top-level routing of the NUMA command
 *
 * Usage: NUMA <MIGRATE|CONFIG|FLOW|HELP> [subcommand] [args...]
 */
void numaCommand(client *c) {
    if (c->argc < 2) {
        addReplyError(c, "Usage: NUMA <MIGRATE|CONFIG|FLOW|HELP> [args...]");
        return;
    }

    const char *domain = c->argv[1]->ptr;

    if (!strcasecmp(domain, "MIGRATE")) {
        numa_cmd_migrate(c);
    } else if (!strcasecmp(domain, "CONFIG")) {
        numa_cmd_config(c);
    } else if (!strcasecmp(domain, "FLOW")) {
        numa_flow_command(c);
    } else if (!strcasecmp(domain, "HELP")) {
        numa_cmd_help(c);
    } else {
        addReplyErrorFormat(c,
            "Unknown NUMA domain '%s'. Try NUMA HELP.", domain);
    }
}
