/*
 * NUMA composite LRU strategy implementation (slot 1 default policy).
 *
 * Dual-channel migration decision:
 *   Fast channel: write to the candidate pool on access, processed first by serverCron
 *   Fallback channel: serverCron progressively scans scan_batch_size keys of key_heat_map each run
 *   Execution always re-reads the current PREFIX hotness, never relying on snapshots
 */

#define _GNU_SOURCE
#include "numa_composite_lru.h"
#include "zmalloc.h"
#include "numa_pool.h"        /* numa_pool_num_nodes() */
#include "numa_bw_monitor.h"
#include "numa_configurable_strategy.h"
#include "evict.h"        /* numaGetNodePressure() */
#include "numa_key_migrate.h"  /* numa_migrate_single_key() */
#include <string.h>
#include <sys/time.h>
#include <stdlib.h>
#include <stdio.h>
#include <sched.h>
#include <numa.h>

/* ========== Logging ========== */

#ifdef NUMA_STRATEGY_STANDALONE
#define _serverLog(level, fmt, ...) printf("[%s] " fmt "\n", level, ##__VA_ARGS__)
#else
extern void _serverLog(int level, const char *fmt, ...);
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#endif

/* ========== Helper functions ========== */

static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

static uint16_t get_lru_clock(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint16_t)(tv.tv_sec & 0xFFFF);
}

static uint16_t calculate_time_delta(uint16_t current, uint16_t last) {
    if (current >= last) return current - last;
    return (0xFFFF - last) + current + 1;
}

static uint8_t compute_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < LAZY_DECAY_STEP1_SECS) return 0;
    if (elapsed_secs < LAZY_DECAY_STEP2_SECS) return 1;
    if (elapsed_secs < LAZY_DECAY_STEP3_SECS) return 2;
    if (elapsed_secs < LAZY_DECAY_STEP4_SECS) return 3;
    return COMPOSITE_LRU_HOTNESS_MAX;
}

/* Main-thread NUMA node: only the main thread's node takes part in migration
 * decisions. When background/IO threads are scheduled to other NUMA nodes by
 * the OS, keys must not be migrated there, otherwise migration ping-pong
 * occurs (main thread migrates back, background thread migrates away, ad infinitum). */
static pthread_t s_main_thread_id;
static int       s_main_thread_node = 0;
static int       s_main_thread_inited = 0;

void composite_lru_set_main_thread(void) {
    s_main_thread_id = pthread_self();
    if (numa_available() >= 0)
        s_main_thread_node = numa_node_of_cpu(sched_getcpu());
    else
        s_main_thread_node = 0;
    s_main_thread_inited = 1;
}

static inline int is_main_thread(void) {
    return s_main_thread_inited && pthread_equal(pthread_self(), s_main_thread_id);
}

/* Get the NUMA node used for migration decisions: always the main thread's node. */
static int get_current_numa_node(void) {
    if (s_main_thread_inited) return s_main_thread_node;
    if (numa_available() < 0) return 0;
    return numa_node_of_cpu(sched_getcpu());
}

/* ========== Heat map dictionary callbacks ========== */

static uint64_t heat_map_hash(const void *key) {
    /* Hash by content: key names are sds strings stored as sdsdup copies. With
     * pointer hashing dictFetchValue would always miss, adding a new entry on
     * every threshold crossing and never updating the old one, causing memory
     * leaks and repeated idle migrations. */
    return dictGenHashFunction(key, sdslen((const sds)key));
}

static int heat_map_key_compare(dict *d, const void *key1, const void *key2) {
    (void)d;
    return sdscmp((const sds)key1, (const sds)key2) == 0;
}

static void *heat_map_key_dup(dict *d, const void *key) {
    (void)d;
    return sdsdup((const sds)key);
}

static void heat_map_key_destructor(dict *d, void *key) {
    (void)d;
    sdsfree((sds)key);
}

static void heat_map_val_destructor(dict *d, void *val) {
    (void)d;
    zfree(val);
}

static dictType heat_map_dict_type = {
    .hashFunction = heat_map_hash,
    .keyDup = heat_map_key_dup,
    .valDup = NULL,
    .keyCompare = heat_map_key_compare,
    .keyDestructor = heat_map_key_destructor,
    .valDestructor = heat_map_val_destructor
};

/* ========== JSON config parsing helpers ========== */

/* Strip leading and trailing whitespace. */
static char *trim_spaces(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n'))
        *end-- = '\0';
    return s;
}

/*
 * composite_lru_config_defaults - fill in the default configuration
 */
void composite_lru_config_defaults(composite_lru_config_t *cfg) {
    if (!cfg) return;
    cfg->decay_threshold_sec       = 10;
    cfg->migrate_hotness_threshold = 3;
    cfg->stability_count           = 3;
    cfg->hot_candidates_size       = 1024;
    cfg->scan_batch_size           = 2500;
    cfg->migration_rate_multiplier = 5;
    cfg->overload_threshold        = 0.8;
    cfg->bandwidth_threshold       = 0.9;
    cfg->pressure_threshold        = 0.7;
    cfg->auto_migrate_enabled      = 1;
    cfg->access_tracking_enabled   = 1;
    cfg->locality_stats_enabled    = 0;
    cfg->debug_logging_enabled     = 0;
}

/*
 * composite_lru_load_config - load the configuration from a JSON file
 *
 * Supports a flat top-level key-value format without depending on an external
 * JSON library. Fields that are absent or unparseable keep their defaults.
 */
int composite_lru_load_config(const char *path, composite_lru_config_t *out) {
    if (!path || !out) return NUMA_STRATEGY_EINVAL;

    composite_lru_config_defaults(out);

    FILE *fp = fopen(path, "r");
    if (!fp) {
        _serverLog(LL_WARNING, "[Composite LRU] Cannot open config file: %s", path);
        return NUMA_STRATEGY_ERR;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        /* Skip comment and empty lines. */
        char *p = trim_spaces(line);
        if (*p == '/' || *p == '#' || *p == '{' || *p == '}' || *p == '\0') continue;

        /* Try to match the "key": value format. */
        char *colon = strchr(p, ':');
        if (!colon) continue;

        /* Extract the key. */
        *colon = '\0';
        char *k = trim_spaces(p);
        /* Strip the quotes. */
        if (*k == '"') k++;
        char *ke = strchr(k, '"');
        if (ke) *ke = '\0';

        /* Extract the value. */
        char *v = trim_spaces(colon + 1);
        /* Strip the trailing comma. */
        char *ve = v + strlen(v) - 1;
        while (ve >= v && (*ve == ',' || *ve == ' ' || *ve == '\t')) *ve-- = '\0';

        /* Match the field name and set the value. */
        if (strcmp(k, "decay_threshold_sec") == 0) {
            out->decay_threshold_sec = (uint32_t)atoi(v);
        } else if (strcmp(k, "migrate_hotness_threshold") == 0) {
            int t = atoi(v);
            out->migrate_hotness_threshold = (t >= 1 && t <= 7) ? (uint8_t)t : 5;
        } else if (strcmp(k, "stability_count") == 0) {
            out->stability_count = (uint8_t)atoi(v);
        } else if (strcmp(k, "hot_candidates_size") == 0) {
            uint32_t sz = (uint32_t)atoi(v);
            out->hot_candidates_size = (sz > 0) ? sz : 256;
        } else if (strcmp(k, "scan_batch_size") == 0) {
            uint32_t bs = (uint32_t)atoi(v);
            out->scan_batch_size = (bs > 0) ? bs : 2500;
        } else if (strcmp(k, "migration_rate_multiplier") == 0) {
            uint32_t m = (uint32_t)atoi(v);
            out->migration_rate_multiplier = (m > 0) ? m : 5;
        } else if (strcmp(k, "overload_threshold") == 0) {
            out->overload_threshold = atof(v);
        } else if (strcmp(k, "bandwidth_threshold") == 0) {
            out->bandwidth_threshold = atof(v);
        } else if (strcmp(k, "pressure_threshold") == 0) {
            out->pressure_threshold = atof(v);
        } else if (strcmp(k, "auto_migrate_enabled") == 0) {
            out->auto_migrate_enabled = atoi(v);
        } else if (strcmp(k, "access_tracking_enabled") == 0) {
            out->access_tracking_enabled = atoi(v);
        } else if (strcmp(k, "locality_stats_enabled") == 0) {
            out->locality_stats_enabled = atoi(v);
        } else if (strcmp(k, "debug_logging_enabled") == 0) {
            out->debug_logging_enabled = atoi(v);
        } else if (strncmp(k, "max_bandwidth_node", 18) == 0) {
            /* Parse max_bandwidth_nodeX_mbps, X = node number. */
            int node_id = atoi(k + 18);  /* Digits following "max_bandwidth_node". */
            double mbps = atof(v);
            if (node_id >= 0 && node_id < NUMA_BW_MAX_NODES && mbps > 0) {
                numa_bw_set_max_bandwidth(node_id, mbps);
                _serverLog(LL_NOTICE,
                    "[Composite LRU] Set node %d max bandwidth: %.0f MB/s", node_id, mbps);
            }
        }
    }

    fclose(fp);
    _serverLog(LL_NOTICE,
        "[Composite LRU] Config loaded: %s (threshold=%d, candidates=%u, scan_batch=%u, multiplier=%u, auto=%d)",
        path, out->migrate_hotness_threshold, out->hot_candidates_size,
        out->scan_batch_size, out->migration_rate_multiplier, out->auto_migrate_enabled);
    return NUMA_STRATEGY_OK;
}

/*
 * composite_lru_apply_config - apply the configuration to a running strategy instance
 *
 * If the candidate pool size changed, rebuild the candidate pool array.
 */
int composite_lru_apply_config(numa_strategy_t *strategy, const composite_lru_config_t *cfg) {
    if (!strategy || !strategy->private_data || !cfg) return NUMA_STRATEGY_EINVAL;
    composite_lru_data_t *data = strategy->private_data;

    int rebuild_pool = (cfg->hot_candidates_size != data->config.hot_candidates_size);

    if (rebuild_pool) {
        /* Free the sds key names still held by the old pool before rebuilding to avoid leaks. */
        for (uint32_t i = 0; i < data->config.hot_candidates_size; i++) {
            if (data->hot_candidates[i].key) sdsfree(data->hot_candidates[i].key);
        }
        zfree(data->hot_candidates);
        data->hot_candidates = zcalloc(cfg->hot_candidates_size * sizeof(hot_candidate_t));
        if (!data->hot_candidates) {
            _serverLog(LL_WARNING, "[Composite LRU] apply_config: failed to allocate candidate pool");
            return NUMA_STRATEGY_ERR;
        }
        data->candidates_head  = 0;
        data->candidates_tail  = 0;
        data->candidates_count = 0;
    }

    /* Reset the scan cursor so the old config's step count is not reused. */
    if (data->scan_iter) {
        dictReleaseIterator(data->scan_iter);
        data->scan_iter = NULL;
    }

    data->config = *cfg;
    _serverLog(LL_NOTICE,
        "[Composite LRU] Config applied: decay=%us, threshold=%d, pool=%u, batch=%u, multiplier=%u, auto=%d",
        cfg->decay_threshold_sec, cfg->migrate_hotness_threshold,
        cfg->hot_candidates_size, cfg->scan_batch_size,
        cfg->migration_rate_multiplier, cfg->auto_migrate_enabled);
    return NUMA_STRATEGY_OK;
}

/* ========== Resource monitoring ========== */

/* Check the resource state of the target node. */
static int check_resource_status(composite_lru_data_t *data, int node_id) {
    /* 1. Memory overload check. */
    double pressure = numaGetNodePressure(node_id);
    double bw_usage = (double)numa_config_get_cached_bw(node_id) / 100.0;
    if (pressure >= data->config.overload_threshold) {
        data->migrations_overloaded++;
        if (data->config.debug_logging_enabled) {
            _serverLog(LL_NOTICE,
                "[Composite LRU][debug] resource target=%d pressure=%.2f bw=%.2f combined=%.2f decision=OVERLOADED_THROTTLED",
                node_id, pressure, bw_usage, pressure * 0.6 + bw_usage * 0.4);
        }
        _serverLog(LL_DEBUG,
            "[Composite LRU] Node %d resource check: AVAILABLE_OVERLOADED_THROTTLED (pressure=%.2f >= %.2f)",
            node_id, pressure, data->config.overload_threshold);
        /* Target node memory overload: block migrations. Previously this
         * wrongly returned AVAILABLE, disabling overload protection so that
         * fuller nodes attracted even more migrations. */
        return RESOURCE_OVERLOADED;
    }

    /* 2. Bandwidth saturation check (reads the global cache, updated by serverCron every second). */
    if (bw_usage >= data->config.bandwidth_threshold) {
        if (data->config.debug_logging_enabled) {
            _serverLog(LL_NOTICE,
                "[Composite LRU][debug] resource target=%d pressure=%.2f bw=%.2f combined=%.2f decision=BW_SATURATED",
                node_id, pressure, bw_usage, pressure * 0.6 + bw_usage * 0.4);
        }
        _serverLog(LL_DEBUG,
            "[Composite LRU] Node %d resource check: BW_SATURATED (bw=%.2f >= %.2f)",
            node_id, bw_usage, data->config.bandwidth_threshold);
        return RESOURCE_BANDWIDTH_SATURATED;
    }

    /* 3. Combined migration pressure is observational only, it does not block hot migrations. */
    double combined = pressure * 0.6 + bw_usage * 0.4;
    if (combined >= data->config.pressure_threshold) {
        if (data->config.debug_logging_enabled) {
            _serverLog(LL_NOTICE,
                "[Composite LRU][debug] resource target=%d pressure=%.2f bw=%.2f combined=%.2f decision=AVAILABLE_WITH_PRESSURE",
                node_id, pressure, bw_usage, combined);
        }
        return RESOURCE_AVAILABLE;
    }

    if (data->config.debug_logging_enabled) {
        _serverLog(LL_NOTICE,
            "[Composite LRU][debug] resource target=%d pressure=%.2f bw=%.2f combined=%.2f decision=AVAILABLE",
            node_id, pressure, bw_usage, combined);
    }

    return RESOURCE_AVAILABLE;
}

/* ========== Hotness management ========== */

/*
 * composite_lru_record_access - hotness update on the access path
 *
 * Design principle: only update hotness, never enqueue.
 * If this access makes hotness cross the threshold and the memory lives on a
 * remote node, the key is written to the candidate pool (fast channel).
 *
 * val:      robj pointer, used for PREFIX hotness tracking (robj lifetime is stable)
 * data_ptr: actual data pointer (e.g. the raw SDS body), used to detect the NUMA
 *           node where the data lives; when NULL, falls back to reading node info
 *           from the val PREFIX
 */
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val, void *data_ptr, uint16_t current_time) {
    if (!strategy || !strategy->private_data || !key) return;

    /* Single-node fast path: no remote NUMA access is possible, PREFIX hotness tracking is pointless. */
    if (numa_pool_num_nodes() <= 1) return;

    composite_lru_data_t *data = strategy->private_data;
    if (!data->config.access_tracking_enabled) return;
    int current_node = get_current_numa_node();

    if (val) {
        /* ---- PREFIX path (main path) ---- */
        uint8_t hotness = numa_get_hotness(val);
        int mem_node = data_ptr ? numa_get_node_id(data_ptr) : numa_get_node_id(val);

        /* Staircase lazy decay: settle the decay debt accumulated since the last access in one go. */
        uint16_t last_access = numa_get_last_access(val);
        uint16_t elapsed = calculate_time_delta(current_time, last_access);
        uint8_t decay = compute_lazy_decay_steps(elapsed);
        if (decay > 0) {
            uint8_t decayed = (decay >= hotness) ? 0 : (hotness - decay);
            if (decayed != hotness) {
                numa_set_hotness(val, decayed);
                data->decay_operations++;
                hotness = decayed;
            }
        }

        /* Record the hotness before decay to detect threshold crossings. */
        uint8_t hotness_before = hotness;

        /* Any access increases hotness (local or remote). */
        if (hotness < COMPOSITE_LRU_HOTNESS_MAX) {
            hotness++;
            numa_set_hotness(val, hotness);
        }

        /* Update the PREFIX access statistics.
         * Optimization: on local access with hotness already at MAX, skip the
         * access_count increment and only update last_access for decay settlement. */
        uint8_t is_local = (mem_node == current_node);
        if (data->config.locality_stats_enabled) {
            if (is_local) {
                data->accesses_local++;
            } else {
                data->accesses_remote++;
            }
        }
        if (is_local && hotness >= COMPOSITE_LRU_HOTNESS_MAX) {
            uint8_t ac = numa_get_access_count(val);
            if (ac < UINT8_MAX) {
                numa_increment_access_count(val);
            }
        } else {
            numa_increment_access_count(val);
        }
        numa_set_last_access(val, current_time);
        data->heat_updates++;

        /*
         * Fast-channel write conditions:
         *   1. Memory lives on a remote node (mem_node != current_node)
         *   2. This access exactly crosses the threshold (before < threshold <= after)
         *
         * Also write to key_heat_map for fallback retry by the scan channel
         * (only on threshold crossing, not on every access, to avoid dict
         * lookup overhead on the hot path).
         */
        uint8_t thr = data->config.migrate_hotness_threshold;
        if (data->config.debug_logging_enabled) {
            _serverLog(LL_NOTICE,
                "[Composite LRU][debug] access key=%p val=%p mem_node=%d cpu_node=%d hotness_before=%u hotness_after=%u threshold=%u crossed=%d",
                key, val, mem_node, current_node, hotness_before, hotness, thr,
                (mem_node != current_node && hotness_before < thr && hotness >= thr));
        }
        if (mem_node != current_node &&
            hotness_before < thr && hotness >= thr) {
            uint32_t idx = data->candidates_head % data->config.hot_candidates_size;
            if (data->hot_candidates[idx].key) sdsfree(data->hot_candidates[idx].key);
            data->hot_candidates[idx].key             = sdsdup((sds)key);
            data->hot_candidates[idx].val             = val;
            data->hot_candidates[idx].data_ptr        = data_ptr;
            data->hot_candidates[idx].target_node     = current_node;
            data->hot_candidates[idx].hotness_snapshot = hotness;
            data->candidates_head = (idx + 1) % data->config.hot_candidates_size;
            if (data->candidates_count < data->config.hot_candidates_size) {
                data->candidates_count++;
            } else {
                data->candidates_tail = (data->candidates_tail + 1) % data->config.hot_candidates_size;
            }
            data->candidates_written++;
            _serverLog(LL_DEBUG,
                "[Composite LRU] Candidate written: val=%p mem_node=%d cpu_node=%d hotness=%d",
                val, mem_node, current_node, hotness);

            /* Synchronously write to key_heat_map: scan-channel fallback (retry when the fast-channel candidate was dropped). */
            composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, key);
            if (!info) {
                info = zmalloc(sizeof(*info));
                if (info) {
                    info->hotness           = hotness;
                    info->stability_counter = 0;
                    info->last_access       = current_time;
                    info->access_count      = 1;
                    info->current_node      = mem_node;
                    info->preferred_node    = current_node;
                    /* Key-name copying is handled by keyDup, so manual sdsdup
                     * does not diverge from the dict hash/compare/free semantics. */
                    dictAdd(data->key_heat_map, key, info);
                }
            } else {
                info->hotness        = hotness;
                info->current_node   = mem_node;
                info->preferred_node = current_node;
            }
        }
    } else {
        /* ---- Dictionary fallback path (when val is NULL) ---- */
        composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, key);

        if (!info) {
            info = zmalloc(sizeof(*info));
            if (!info) return;
            info->hotness         = 1;
            info->stability_counter = 0;
            info->last_access     = current_time;
            info->access_count    = 1;
            info->current_node    = current_node;
            info->preferred_node  = -1;
            dictAdd(data->key_heat_map, key, info);
            data->heat_updates++;
            return;
        }

        info->access_count++;
        uint16_t old_last = info->last_access;
        info->last_access = current_time;
        data->heat_updates++;

        /* Lazy decay. */
        uint16_t elapsed = calculate_time_delta(current_time, old_last);
        uint8_t decay = compute_lazy_decay_steps(elapsed);
        if (decay > 0) {
            uint8_t before = info->hotness;
            info->hotness = (decay >= info->hotness) ? 0 : (info->hotness - decay);
            if (info->hotness != before)
                data->decay_operations++;
        }

        uint8_t hotness_before = info->hotness;
        if (info->hotness < COMPOSITE_LRU_HOTNESS_MAX)
            info->hotness++;
        info->stability_counter = 0;

        /* Dictionary-path candidate pool write (hotness just crossed the threshold and the access is remote). */
        uint8_t thr = data->config.migrate_hotness_threshold;
        if (info->current_node != current_node &&
            hotness_before < thr && info->hotness >= thr) {
            info->preferred_node = current_node;
            uint32_t idx = data->candidates_head % data->config.hot_candidates_size;
            if (data->hot_candidates[idx].key) sdsfree(data->hot_candidates[idx].key);
            data->hot_candidates[idx].key             = sdsdup((sds)key);
            data->hot_candidates[idx].val             = NULL;  /* No val pointer on the dictionary path. */
            data->hot_candidates[idx].target_node     = current_node;
            data->hot_candidates[idx].hotness_snapshot = info->hotness;
            data->candidates_head = (idx + 1) % data->config.hot_candidates_size;
            if (data->candidates_count < data->config.hot_candidates_size) {
                data->candidates_count++;
            } else {
                data->candidates_tail = (data->candidates_tail + 1) % data->config.hot_candidates_size;
            }
            data->candidates_written++;
        }
    }
}

/* composite_lru_decay_heat is kept for explicit external calls (dictionary-path fallback decay). */
void composite_lru_decay_heat(composite_lru_data_t *data) {
    if (!data || !data->key_heat_map) return;

    dictIterator *di = dictGetSafeIterator(data->key_heat_map);
    dictEntry *de = NULL;
    uint16_t current_time = get_lru_clock();
    uint16_t decay_thr_sec = (uint16_t)data->config.decay_threshold_sec;

    while ((de = dictNext(di)) != NULL) {
        composite_lru_heat_info_t *info = dictGetVal(de);
        uint16_t elapsed = calculate_time_delta(current_time, info->last_access);
        if (elapsed > decay_thr_sec) {
            info->stability_counter++;
            if (info->stability_counter > data->config.stability_count) {
                if (info->hotness > COMPOSITE_LRU_HOTNESS_MIN) {
                    info->hotness--;
                    data->decay_operations++;
                }
                info->stability_counter = 0;
            }
        } else {
            info->stability_counter = 0;
        }
    }
    dictReleaseIterator(di);
}

/* ========== Progressive scan (fallback channel) ========== */

/*
 * composite_lru_scan_once - advance one batch of the progressive scan
 *
 * Scans up to batch_size key_heat_map entries from the current scan_iter
 * position. Keys whose hotness reached the threshold and live on a remote
 * node are migrated directly via numa_migrate_single_key. When the scan
 * reaches the end, scan_iter is reset to NULL so the next call restarts
 * from the beginning.
 *
 * @scanned_out : number of keys scanned this round (may be NULL)
 * @migrated_out: number of migrations triggered this round (may be NULL)
 */
int composite_lru_scan_once(numa_strategy_t *strategy, uint32_t batch_size,
                            uint64_t *scanned_out, uint64_t *migrated_out) {
    if (!strategy || !strategy->private_data) return NUMA_STRATEGY_EINVAL;
    composite_lru_data_t *data = strategy->private_data;

    if (!data->key_heat_map || dictSize(data->key_heat_map) == 0) {
        if (scanned_out)  *scanned_out  = 0;
        if (migrated_out) *migrated_out = 0;
        return NUMA_STRATEGY_OK;
    }

    /* If the cursor is NULL, start a new scan round from the beginning. */
    if (!data->scan_iter) {
        data->scan_iter = dictGetSafeIterator(data->key_heat_map);
        if (!data->scan_iter) return NUMA_STRATEGY_ERR;
    }

    uint64_t scanned  = 0;
    uint64_t migrated = 0;
    uint64_t budget_used = 0;
    uint8_t  thr = data->config.migrate_hotness_threshold;
    dictEntry *de = NULL;

    while (budget_used < batch_size && (de = dictNext(data->scan_iter)) != NULL) {
        composite_lru_heat_info_t *info = dictGetVal(de);
        scanned++;
        budget_used++;
        data->scan_keys_checked++;

        /* Re-read hotness at execution time (never rely on snapshots). */
        if (info->hotness >= thr &&
            info->preferred_node >= 0 &&
            info->current_node != info->preferred_node) {

            int status = check_resource_status(data, info->preferred_node);
            if (status == RESOURCE_BANDWIDTH_SATURATED) {
                data->migrations_bw_blocked++;
            }
            if (status == RESOURCE_AVAILABLE) {
                if (data->db) {
                    dictEntry *live = dictFind(data->db->dict, dictGetKey(de));
                    if (live) {
                        robj *cur_val = dictGetVal(live);
                        uint32_t cost_units = numa_object_migration_cost_units(cur_val);
                        if (cost_units > batch_size) cost_units = batch_size;
                        if (budget_used > 1 && budget_used + cost_units - 1 > batch_size) {
                            break;
                        }
                        budget_used += cost_units - 1;
                    }
                }
                _serverLog(LL_DEBUG,
                    "[Composite LRU] Scan migrate (dict): key=%p node=%d->%d hotness=%d",
                    dictGetKey(de), info->current_node, info->preferred_node, info->hotness);
                int rc = -1;
                if (data->db) {
                    rc = numa_migrate_key_by_name(data->db,
                             (const char *)dictGetKey(de), info->preferred_node);
                }
                if (data->config.debug_logging_enabled) {
                    _serverLog(LL_NOTICE,
                        "[Composite LRU][debug] scan key=%p current_node=%d preferred_node=%d hotness=%u rc=%d",
                        dictGetKey(de), info->current_node, info->preferred_node, info->hotness, rc);
                }
                if (rc == 0) {
                    info->current_node = info->preferred_node;
                    info->preferred_node = -1;
                    data->migrations_completed++;
                } else {
                    data->migrations_failed++;
                }
                data->migrations_triggered++;
                migrated++;
            }
        }
    }

    /* If the iterator is exhausted, release it, set NULL, and restart next time. */
    if (de == NULL) {
        dictReleaseIterator(data->scan_iter);
        data->scan_iter = NULL;
    }

    if (scanned_out)  *scanned_out  = scanned;
    if (migrated_out) *migrated_out = migrated;
    return NUMA_STRATEGY_OK;
}

/* ========== Strategy vtable implementation ========== */

/* Strategy initialization. */
int composite_lru_init(numa_strategy_t *strategy) {
    composite_lru_data_t *data = zmalloc(sizeof(*data));
    if (!data) return NUMA_STRATEGY_ERR;

    memset(data, 0, sizeof(*data));

    /* Load the default configuration. */
    composite_lru_config_defaults(&data->config);

    /* Create the hot candidate pool (ring buffer). */
    data->hot_candidates = zcalloc(data->config.hot_candidates_size * sizeof(hot_candidate_t));
    if (!data->hot_candidates) {
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }
    data->candidates_head  = 0;
    data->candidates_count = 0;
    data->scan_iter        = NULL;

    /* Create the dictionary fallback heat map. */
    data->key_heat_map = dictCreate(&heat_map_dict_type);
    if (!data->key_heat_map) {
        zfree(data->hot_candidates);
        zfree(data);
        return NUMA_STRATEGY_ERR;
    }

    data->last_decay_time = get_current_time_us();
    strategy->private_data = data;

    _serverLog(LL_NOTICE,
        "[Composite LRU] Strategy initialized: threshold=%d, candidates_size=%u, scan_batch=%u, multiplier=%u, auto=%d",
        data->config.migrate_hotness_threshold,
        data->config.hot_candidates_size,
        data->config.scan_batch_size,
        data->config.migration_rate_multiplier,
        data->config.auto_migrate_enabled);
    return NUMA_STRATEGY_OK;
}

/*
 * composite_lru_execute_step - execute the Composite LRU migration strategy in small steps
 *
 * Flow:
 *   1. If auto_migrate_enabled == 0, return immediately
 *   2. Fast channel: process the candidate pool within budget, re-read PREFIX
 *      hotness, migrate only if the conditions still hold
 *   3. Fallback channel: advance the key_heat_map progressive scan within the
 *      remaining budget
 */
int composite_lru_execute_step(numa_strategy_t *strategy, uint64_t deadline_us, uint32_t budget) {
    if (!strategy || !strategy->private_data) return NUMA_STRATEGY_STEP_ERROR;
    composite_lru_data_t *data = strategy->private_data;

    if (!data->config.auto_migrate_enabled) return NUMA_STRATEGY_STEP_IDLE;

    static uint64_t last_log_time = 0;
    static uint64_t exec_count = 0;
    exec_count++;
    uint64_t now = get_current_time_us();
    if (now - last_log_time > 10000000) {
        _serverLog(LL_VERBOSE,
            "[NUMA Strategy Slot 1] Composite LRU executed "
            "(count: %llu, candidates: %u, heat_updates: %llu, "
            "migrations: %llu, bw_blocked: %llu, overloaded: %llu, "
            "candidates_written: %llu, scan_checked: %llu, heat_map_size: %lu, "
            "local: %llu, remote: %llu)",
            (unsigned long long)exec_count,
            data->candidates_count,
            (unsigned long long)data->heat_updates,
            (unsigned long long)data->migrations_triggered,
            (unsigned long long)data->migrations_bw_blocked,
            (unsigned long long)data->migrations_overloaded,
            (unsigned long long)data->candidates_written,
            (unsigned long long)data->scan_keys_checked,
            (unsigned long)dictSize(data->key_heat_map),
            (unsigned long long)data->accesses_local,
            (unsigned long long)data->accesses_remote);
        last_log_time = now;
    }

    uint32_t pool_size = data->config.hot_candidates_size;
    uint32_t processed = 0;
    uint32_t budget_used = 0;
    uint32_t migrated = 0;
    int hit_deadline = 0;

    int main_node = get_current_numa_node();
    int load_weight = numa_config_get_cached_pressure_weight(main_node);
    if (load_weight < 1) load_weight = 1;
    if (load_weight > 100) load_weight = 100;
    uint32_t migration_multiplier = data->config.migration_rate_multiplier;
    if (migration_multiplier < 1) migration_multiplier = 1;
    if (migration_multiplier > 100) migration_multiplier = 100;

    uint32_t migration_budget;
    if (budget > 0) {
        migration_budget = budget;
    } else {
        migration_budget = (pool_size * (uint32_t)load_weight * migration_multiplier + 99) / 100;
        if (migration_budget < 64) migration_budget = 64;
    }
    if (migration_budget > pool_size) migration_budget = pool_size;

    while (data->candidates_count > 0 && budget_used < migration_budget) {
        if (deadline_us && get_current_time_us() >= deadline_us) {
            hit_deadline = 1;
            break;
        }

        hot_candidate_t *cand = &data->hot_candidates[data->candidates_tail];
        data->candidates_tail = (data->candidates_tail + 1) % pool_size;
        data->candidates_count--;
        processed++;

        if (!cand->key) {
            memset(cand, 0, sizeof(*cand));
            continue;
        }

        uint8_t cur_hotness;
        int mem_node;
        if (data->db) {
            dictEntry *de = dictFind(data->db->dict, cand->key);
            if (!de) {
                sdsfree(cand->key);
                memset(cand, 0, sizeof(*cand));
                continue;
            }
            robj *cur_val = dictGetVal(de);
            cur_hotness = numa_get_hotness(cur_val);
            uint32_t cost_units = numa_object_migration_cost_units(cur_val);
            if (cost_units > migration_budget) cost_units = migration_budget;
            if (budget_used > 0 && budget_used + cost_units > migration_budget) {
                data->candidates_tail = (data->candidates_tail + pool_size - 1) % pool_size;
                data->candidates_count++;
                break;
            }
            budget_used += cost_units;
            void *cur_data_ptr = NULL;
            if (cur_val->encoding == OBJ_ENCODING_RAW && cur_val->ptr)
                cur_data_ptr = sdsAllocPtr(cur_val->ptr);
            void *node_src = cur_data_ptr ? cur_data_ptr : cur_val;
            mem_node = numa_get_node_id(node_src);
        } else {
            composite_lru_heat_info_t *info = dictFetchValue(data->key_heat_map, cand->key);
            if (!info) {
                sdsfree(cand->key);
                memset(cand, 0, sizeof(*cand));
                continue;
            }
            cur_hotness = info->hotness;
            mem_node = info->current_node;
            budget_used++;
        }

        int effective_threshold = data->config.migrate_hotness_threshold;
        double src_bw = (double)numa_config_get_cached_bw(mem_node) / 100.0;
        if (src_bw > 0.7 && effective_threshold > 1) {
            effective_threshold -= 1;
            _serverLog(LL_DEBUG,
                "[Composite LRU] Source node %d bw=%.2f > 0.7, lowering threshold to %d",
                mem_node, src_bw, effective_threshold);
        }
        if (cur_hotness >= effective_threshold && mem_node != cand->target_node) {
            int status = check_resource_status(data, cand->target_node);
            if (status == RESOURCE_BANDWIDTH_SATURATED) {
                data->migrations_bw_blocked++;
            }
            if (status == RESOURCE_AVAILABLE) {
                _serverLog(LL_DEBUG,
                    "[Composite LRU] Fast-path migrate: key=%p node=%d->%d hotness=%d",
                    cand->key, mem_node, cand->target_node, cur_hotness);
                if (data->db && cand->key) {
                    if (data->config.debug_logging_enabled) {
                        _serverLog(LL_NOTICE,
                            "[Composite LRU][debug] fast invoke db=%p key=%s mem_node=%d target=%d",
                            (void *)data->db, (const char *)cand->key, mem_node, cand->target_node);
                    }
                    int rc = numa_migrate_key_by_name(data->db,
                                 (const char *)cand->key, cand->target_node);
                    if (data->config.debug_logging_enabled) {
                        _serverLog(LL_NOTICE,
                            "[Composite LRU][debug] fast key=%s mem_node=%d target_node=%d hotness=%u threshold=%d rc=%d",
                            (const char *)cand->key, mem_node, cand->target_node, cur_hotness, effective_threshold, rc);
                    }
                    if (rc == 0) {
                        data->migrations_completed++;
                        migrated++;
                    } else {
                        data->migrations_failed++;
                    }
                } else if (data->config.debug_logging_enabled) {
                    _serverLog(LL_NOTICE,
                        "[Composite LRU][debug] fast skipped db=%p key=%s target=%d",
                        (void *)data->db, (const char *)cand->key, cand->target_node);
                }
                data->migrations_triggered++;
            }
        }

        sdsfree(cand->key);
        memset(cand, 0, sizeof(*cand));
    }

    if (data->candidates_count == 0) {
        data->candidates_head = 0;
        data->candidates_tail = 0;
    }

    if (!hit_deadline && budget_used < migration_budget) {
        uint32_t scan_budget = data->config.scan_batch_size * (uint32_t)load_weight * migration_multiplier / 100;
        if (scan_budget < 64) scan_budget = 64;
        uint32_t remaining = migration_budget - budget_used;
        if (scan_budget > remaining) scan_budget = remaining;
        if (deadline_us && get_current_time_us() >= deadline_us) {
            hit_deadline = 1;
        } else if (scan_budget > 0) {
            uint64_t scanned = 0, scan_migrated = 0;
            int ret = composite_lru_scan_once(strategy, scan_budget, &scanned, &scan_migrated);
            if (ret < 0) return NUMA_STRATEGY_STEP_ERROR;
            processed += (uint32_t)scanned;
            migrated += (uint32_t)scan_migrated;
        }
    }

    if (hit_deadline) return NUMA_STRATEGY_STEP_TIMEOUT;
    if (data->candidates_count > 0) return NUMA_STRATEGY_STEP_AGAIN;
    if (processed > 0 || migrated > 0) return NUMA_STRATEGY_STEP_PROGRESS;
    return NUMA_STRATEGY_STEP_IDLE;
}

int composite_lru_execute(numa_strategy_t *strategy) {
    int ret = composite_lru_execute_step(strategy, 0, 0);
    return ret < 0 ? NUMA_STRATEGY_ERR : NUMA_STRATEGY_OK;
}

/* Strategy cleanup. */
void composite_lru_cleanup(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return;
    composite_lru_data_t *data = strategy->private_data;

    _serverLog(LL_NOTICE,
        "[Composite LRU] Cleanup: heat_updates=%llu, migrations=%llu, decays=%llu, candidates=%llu, scan_checked=%llu",
        (unsigned long long)data->heat_updates,
        (unsigned long long)data->migrations_triggered,
        (unsigned long long)data->decay_operations,
        (unsigned long long)data->candidates_written,
        (unsigned long long)data->scan_keys_checked);

    if (data->scan_iter) {
        dictReleaseIterator(data->scan_iter);
        data->scan_iter = NULL;
    }
    if (data->hot_candidates) {
        /* Free the sds key names still remaining in the ring buffer. */
        for (uint32_t i = 0; i < data->config.hot_candidates_size; i++) {
            if (data->hot_candidates[i].key) sdsfree(data->hot_candidates[i].key);
        }
        zfree(data->hot_candidates);
        data->hot_candidates = NULL;
    }
    if (data->key_heat_map) {
        dictRelease(data->key_heat_map);
        data->key_heat_map = NULL;
    }

    zfree(data);
    strategy->private_data = NULL;
}

/* Get the strategy name. */
static const char* composite_lru_get_name(numa_strategy_t *strategy) {
    (void)strategy;
    return "composite-lru";
}

/* Get the strategy description. */
static const char* composite_lru_get_description(numa_strategy_t *strategy) {
    (void)strategy;
    return "Slot 1 default policy: stability-first composite LRU hotness management";
}

/* Set configuration parameters (compatible with the old interface, forwards to the config struct). */
int composite_lru_set_config(numa_strategy_t *strategy,
                                    const char *key, const char *value) {
    if (!strategy || !strategy->private_data || !key || !value)
        return NUMA_STRATEGY_EINVAL;

    composite_lru_data_t *data = strategy->private_data;

    if (strcmp(key, "decay_threshold") == 0 ||
        strcmp(key, "decay_threshold_sec") == 0) {
        data->config.decay_threshold_sec = (uint32_t)atoi(value);
    } else if (strcmp(key, "stability_count") == 0) {
        data->config.stability_count = (uint8_t)atoi(value);
    } else if (strcmp(key, "migrate_threshold") == 0 ||
               strcmp(key, "migrate_hotness_threshold") == 0) {
        int t = atoi(value);
        data->config.migrate_hotness_threshold = (t >= 1 && t <= 7) ? (uint8_t)t : 5;
    } else if (strcmp(key, "overload_threshold") == 0) {
        data->config.overload_threshold = atof(value);
    } else if (strcmp(key, "bandwidth_threshold") == 0) {
        data->config.bandwidth_threshold = atof(value);
    } else if (strcmp(key, "pressure_threshold") == 0) {
        data->config.pressure_threshold = atof(value);
    } else if (strcmp(key, "hot_candidates_size") == 0) {
        uint32_t sz = (uint32_t)atoi(value);
        if (sz > 0 && sz != data->config.hot_candidates_size) {
            /* Rebuild the candidate pool. */
            composite_lru_config_t newcfg = data->config;
            newcfg.hot_candidates_size = sz;
            composite_lru_apply_config(strategy, &newcfg);
        }
    } else if (strcmp(key, "scan_batch_size") == 0) {
        uint32_t bs = (uint32_t)atoi(value);
        if (bs > 0) data->config.scan_batch_size = bs;
    } else if (strcmp(key, "migration_rate_multiplier") == 0) {
        uint32_t m = (uint32_t)atoi(value);
        if (m > 0) data->config.migration_rate_multiplier = m;
    } else if (strcmp(key, "auto_migrate_enabled") == 0) {
        data->config.auto_migrate_enabled = atoi(value);
    } else if (strcmp(key, "access_tracking_enabled") == 0) {
        data->config.access_tracking_enabled = atoi(value);
    } else if (strcmp(key, "locality_stats_enabled") == 0) {
        data->config.locality_stats_enabled = atoi(value);
    } else if (strcmp(key, "debug_logging_enabled") == 0) {
        data->config.debug_logging_enabled = atoi(value);
    } else {
        return NUMA_STRATEGY_EINVAL;
    }

    _serverLog(LL_VERBOSE, "[Composite LRU] Config set: %s = %s", key, value);
    return NUMA_STRATEGY_OK;
}

/* Get configuration parameters. */
int composite_lru_get_config(numa_strategy_t *strategy,
                                    const char *key, char *buf, size_t buf_len) {
    if (!strategy || !strategy->private_data || !key || !buf || buf_len == 0)
        return NUMA_STRATEGY_EINVAL;

    composite_lru_data_t *data = strategy->private_data;

    if (strcmp(key, "decay_threshold") == 0 ||
        strcmp(key, "decay_threshold_sec") == 0) {
        snprintf(buf, buf_len, "%u", data->config.decay_threshold_sec);
    } else if (strcmp(key, "stability_count") == 0) {
        snprintf(buf, buf_len, "%u", data->config.stability_count);
    } else if (strcmp(key, "migrate_threshold") == 0 ||
               strcmp(key, "migrate_hotness_threshold") == 0) {
        snprintf(buf, buf_len, "%u", data->config.migrate_hotness_threshold);
    } else if (strcmp(key, "overload_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->config.overload_threshold);
    } else if (strcmp(key, "bandwidth_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->config.bandwidth_threshold);
    } else if (strcmp(key, "pressure_threshold") == 0) {
        snprintf(buf, buf_len, "%.2f", data->config.pressure_threshold);
    } else if (strcmp(key, "hot_candidates_size") == 0) {
        snprintf(buf, buf_len, "%u", data->config.hot_candidates_size);
    } else if (strcmp(key, "scan_batch_size") == 0) {
        snprintf(buf, buf_len, "%u", data->config.scan_batch_size);
    } else if (strcmp(key, "migration_rate_multiplier") == 0) {
        snprintf(buf, buf_len, "%u", data->config.migration_rate_multiplier);
    } else if (strcmp(key, "auto_migrate_enabled") == 0) {
        snprintf(buf, buf_len, "%d", data->config.auto_migrate_enabled);
    } else if (strcmp(key, "access_tracking_enabled") == 0) {
        snprintf(buf, buf_len, "%d", data->config.access_tracking_enabled);
    } else if (strcmp(key, "locality_stats_enabled") == 0) {
        snprintf(buf, buf_len, "%d", data->config.locality_stats_enabled);
    } else if (strcmp(key, "debug_logging_enabled") == 0) {
        snprintf(buf, buf_len, "%d", data->config.debug_logging_enabled);
    } else if (strcmp(key, "heat_updates") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->heat_updates);
    } else if (strcmp(key, "migrations_triggered") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->migrations_triggered);
    } else if (strcmp(key, "decay_operations") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->decay_operations);
    } else if (strcmp(key, "candidates_written") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->candidates_written);
    } else if (strcmp(key, "scan_keys_checked") == 0) {
        snprintf(buf, buf_len, "%llu", (unsigned long long)data->scan_keys_checked);
    } else {
        return NUMA_STRATEGY_EINVAL;
    }

    return NUMA_STRATEGY_OK;
}

/* Strategy vtable. */
static const numa_strategy_vtable_t composite_lru_vtable = {
    .init = composite_lru_init,
    .execute = composite_lru_execute,
    .execute_step = composite_lru_execute_step,
    .cleanup = composite_lru_cleanup,
    .get_name = composite_lru_get_name,
    .get_description = composite_lru_get_description,
    .set_config = composite_lru_set_config,
    .get_config = composite_lru_get_config
};

/* ========== Factory functions ========== */

/* Create a strategy instance. */
numa_strategy_t* composite_lru_create(void) {
    numa_strategy_t *strategy = zmalloc(sizeof(*strategy));
    if (!strategy) return NULL;
    
    memset(strategy, 0, sizeof(*strategy));
    strategy->slot_id = 1;  /* Slot 1 by default. */
    strategy->name = "composite-lru";
    strategy->description = "Stability-first composite LRU strategy (slot 1 default)";
    strategy->type = STRATEGY_TYPE_PERIODIC;
    strategy->priority = STRATEGY_PRIORITY_HIGH;
    strategy->enabled = 1;
    strategy->execute_interval_us = 1000000;  /* 1 second. */
    strategy->step_budget = 512;
    strategy->max_runtime_us_per_step = 500;
    strategy->vtable = &composite_lru_vtable;
    
    return strategy;
}

/* Destroy a strategy instance. */
void composite_lru_destroy(numa_strategy_t *strategy) {
    if (!strategy) return;
    
    if (strategy->vtable && strategy->vtable->cleanup) {
        strategy->vtable->cleanup(strategy);
    }
    
    zfree(strategy);
}

/* Strategy factory. */
static numa_strategy_factory_t composite_lru_factory = {
    .name = "composite-lru",
    .description = "Stability-first composite LRU hotness management (slot 1 default)",
    .type = STRATEGY_TYPE_PERIODIC,
    .default_priority = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,  /* 1 second. */
    .create = composite_lru_create,
    .destroy = composite_lru_destroy
};

/* ========== Public registration interface ========== */

/* Register the strategy factory. */
int numa_composite_lru_register(void) {
    return numa_strategy_register_factory(&composite_lru_factory);
}

/* ========== Statistics queries ========== */

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
