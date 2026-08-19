/* evict_numa.c - NUMA demotion migration implementation
 *
 * This module ties the eviction pool to the NUMA migration policy: when memory
 * is over the limit, cold data is migrated to another NUMA node instead of being evicted.
 *
 * Core features:
 * - Distance-first node selection (prefers closer nodes)
 * - Pressure awareness (avoids migrating to high-pressure nodes)
 * - Bandwidth awareness (avoids migrating to bandwidth-saturated nodes)
 * - Weighted scoring decision (distance + pressure + bandwidth, weights
 *   are configurable via the numa-demote-* settings)
 *
 * Copyright (c) 2024, Redis-CXL Project
 */

#include "server.h"
#include "evict.h"
#include "zmalloc.h"

#ifdef HAVE_NUMA
#include "numa_pool.h"
#include "numa_key_migrate.h"
#include "numa_bw_monitor.h"
#include <numa.h>
#include <stdio.h>
#include <string.h>

/* Node pressure cache (avoids frequent sysfs reads). */
static double g_node_pressure_cache[MAX_NUMA_NODES];
static long long g_pressure_cache_time[MAX_NUMA_NODES];
#define PRESSURE_CACHE_TTL_MS 1000  /* Cache TTL of 1 second. */

/* ========== Node info queries ========== */

/*
 * numaGetNodePressure - get the memory pressure of a node
 *
 * Reads from /sys/devices/system/node/nodeX/meminfo
 * Returns: 0.0 ~ 1.0, higher means more pressure
 */
double numaGetNodePressure(int node_id) {
    int max_node = numa_max_node();
    if (node_id < 0 || node_id > max_node) {
        return 1.0; /* Invalid nodes report full pressure. */
    }

    /* Check the cache. */
    long long now = server.mstime;
    if (g_pressure_cache_time[node_id] > 0 &&
        (now - g_pressure_cache_time[node_id]) < PRESSURE_CACHE_TTL_MS) {
        return g_node_pressure_cache[node_id];
    }

    double pressure;

    /*
     * When maxmemory > 0, use the per-node quota share as the denominator:
     *   per_node_quota = server.maxmemory / num_nodes
     *   pressure       = node_used_bytes / per_node_quota
     *
     * This way pressure reflects how much of Redis's own memory share on the
     * node is used, instead of the whole physical node memory (which on a
     * 441GB dual-socket server stays at ~9.7% and would never trigger migration).
     */
    if (server.maxmemory > 0) {
        int num_nodes = max_node + 1;
        size_t per_node_quota = server.maxmemory / (size_t)num_nodes;
        size_t node_used = zmalloc_used_memory_node(node_id);

        if (per_node_quota > 0) {
            pressure = (double)node_used / (double)per_node_quota;
            if (pressure > 1.0) pressure = 1.0;
        } else {
            pressure = 1.0;
        }
    } else {
        /* maxmemory not set: fall back to the physical node memory pressure. */
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/devices/system/node/node%d/meminfo", node_id);

        FILE *fp = fopen(path, "r");
        if (!fp) {
            pressure = 1.0;
        } else {
            unsigned long mem_total = 0, mem_free = 0;
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "MemTotal")) {
                    char *colon = strchr(line, ':');
                    if (colon) mem_total = strtoul(colon + 1, NULL, 10);
                } else if (strstr(line, "MemFree")) {
                    char *colon = strchr(line, ':');
                    if (colon) mem_free = strtoul(colon + 1, NULL, 10);
                }
            }
            fclose(fp);

            pressure = (mem_total > 0) ?
                       (1.0 - ((double)mem_free / (double)mem_total)) : 1.0;
        }
    }

    /* Update the cache. */
    g_node_pressure_cache[node_id] = pressure;
    g_pressure_cache_time[node_id] = now;

    return pressure;
}

/*
 * numaGetNodeFreeMemory - get the free memory of a node (KB)
 */
size_t numaGetNodeFreeMemory(int node_id) {
    int max_node = numa_max_node();
    if (node_id < 0 || node_id > max_node) {
        return 0;
    }
    
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/node/node%d/meminfo", node_id);
    
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    
    unsigned long mem_free = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "MemFree")) {
            char *colon = strchr(line, ':');
            if (colon) {
                mem_free = strtoul(colon + 1, NULL, 10);
            }
            break;
        }
    }
    fclose(fp);
    
    return (size_t)mem_free * 1024; /* Convert to bytes. */
}

/* ========== Node selection algorithm ========== */

/*
 * numaFindBestDemoteNode - find the best demotion target node
 *
 * Selection policy: distance-first + pressure awareness + bandwidth awareness
 * Uses a weighted score combining distance, pressure, and bandwidth factors
 */
int numaFindBestDemoteNode(size_t object_size, int current_node) {
    int num_nodes = numa_pool_num_nodes();
    if (num_nodes <= 1) return -1; /* No demotion needed on a single node. */
    
    /* Candidate node structure. */
    typedef struct {
        int node_id;
        int distance;      /* NUMA distance (smaller is closer). */
        double pressure;   /* Memory pressure (0~1). */
        size_t free_mem;   /* Free memory. */
        double bw_usage;   /* Bandwidth utilization (0~1). */
        double score;      /* Combined score (smaller is better). */
    } node_candidate_t;
    
    node_candidate_t candidates[MAX_NUMA_NODES];
    int candidate_count = 0;
    
    /* Collect info for all candidate nodes. */
    for (int i = 0; i < num_nodes; i++) {
        if (i == current_node) continue; /* Skip the current node. */
        
        double pressure = numaGetNodePressure(i);
        size_t free_mem = numaGetNodeFreeMemory(i);
        
        /* Check whether pressure exceeds the limit. */
        double threshold = server.numa_demote_pressure_threshold / 100.0;
        if (pressure >= threshold) {
            serverLog(LL_DEBUG,
                "[NUMA Demote] Node %d skipped: pressure %.2f >= threshold %.2f",
                i, pressure, threshold);
            continue;
        }

        /* Get the bandwidth utilization. */
        double bw_usage = numa_bw_get_usage(i);
        double bw_threshold = server.numa_bw_saturation_threshold / 100.0;
        if (bw_usage >= bw_threshold) {
            serverLog(LL_DEBUG,
                "[NUMA Demote] Node %d skipped: bw_usage %.2f >= threshold %.2f",
                i, bw_usage, bw_threshold);
            continue;
        }
        
        /* Check whether there is enough space. */
        if (free_mem < object_size * 2) {
            serverLog(LL_DEBUG,
                "[NUMA Demote] Node %d skipped: free_mem %zu < required %zu",
                i, free_mem, object_size * 2);
            continue;
        }
        
        /* Get the NUMA distance. */
        int dist = numa_distance(current_node, i);
        
        candidates[candidate_count].node_id = i;
        candidates[candidate_count].distance = dist;
        candidates[candidate_count].pressure = pressure;
        candidates[candidate_count].free_mem = free_mem;
        candidates[candidate_count].bw_usage = bw_usage;
        candidate_count++;
    }
    
    if (candidate_count == 0) {
        serverLog(LL_DEBUG, "[NUMA Demote] No candidate nodes available");
        return -1;
    }
    
    /* === Score computation === */
    /*
     * Combined score = normalized distance * distance_weight + normalized pressure * pressure_weight + normalized bandwidth * bandwidth_weight
     * Lower scores are selected first
     */
    
    /* Find the max distance, pressure, and bandwidth for normalization. */
    int max_distance = 0;
    double max_pressure = 0.0;
    double max_bw_usage = 0.0;
    for (int i = 0; i < candidate_count; i++) {
        if (candidates[i].distance > max_distance) {
            max_distance = candidates[i].distance;
        }
        if (candidates[i].pressure > max_pressure) {
            max_pressure = candidates[i].pressure;
        }
        if (candidates[i].bw_usage > max_bw_usage) {
            max_bw_usage = candidates[i].bw_usage;
        }
    }
    
    /* Avoid division by zero. */
    if (max_distance == 0) max_distance = 1;
    if (max_pressure < 0.01) max_pressure = 1.0;
    if (max_bw_usage < 0.01) max_bw_usage = 1.0;
    
    /* Compute the combined score of each candidate node. */
    for (int i = 0; i < candidate_count; i++) {
        double dist_norm = (double)candidates[i].distance / (double)max_distance;
        double pres_norm = candidates[i].pressure / max_pressure;
        double bw_norm = candidates[i].bw_usage / max_bw_usage;
            
        /* Read the weights from the server configuration. */
        int dist_weight = server.numa_demote_distance_weight;
        int pres_weight = server.numa_demote_pressure_weight;
        int bw_weight = server.numa_demote_bandwidth_weight;
            
        if (server.numa_demote_prefer_closer) {
            /*
             * Strategy A: weighted mode - uses the configured three-factor weights
             * Suitable for latency-sensitive scenarios
             */
            candidates[i].score =
                dist_norm * dist_weight / 100.0 +
                pres_norm * pres_weight / 100.0 +
                bw_norm   * bw_weight   / 100.0;
        } else {
            /*
             * Strategy B: balanced mode - distance, pressure, and bandwidth are equally important
             */
            candidates[i].score = (dist_norm + pres_norm + bw_norm) / 3.0;
        }
    
        serverLog(LL_DEBUG,
            "[NUMA Demote] Node %d: dist=%d(%.2f), pressure=%.2f(%.2f), bw=%.2f(%.2f), score=%.3f",
            candidates[i].node_id,
            candidates[i].distance, dist_norm,
            candidates[i].pressure, pres_norm,
            candidates[i].bw_usage, bw_norm,
            candidates[i].score);
    }
    
    /* Select the node with the lowest score. */
    int best_idx = 0;
    double best_score = candidates[0].score;
    for (int i = 1; i < candidate_count; i++) {
        if (candidates[i].score < best_score) {
            best_score = candidates[i].score;
            best_idx = i;
        }
    }
    
    serverLog(LL_VERBOSE,
        "[NUMA Demote] Selected node %d: distance=%d, pressure=%.2f, bw=%.2f, score=%.3f",
        candidates[best_idx].node_id,
        candidates[best_idx].distance,
        candidates[best_idx].pressure,
        candidates[best_idx].bw_usage,
        candidates[best_idx].score);
    
    return candidates[best_idx].node_id;
}

/* ========== Demotion execution interface ========== */

/*
 * evictionTryNumaDemote - try to demote an object to another NUMA node
 *
 * @db: database pointer (redisDb*)
 * @key: key name (sds)
 * @val: value object (robj*)
 * @target_node: output parameter, receives the target node ID
 *
 * Returns: numa_demote_result_t
 */
numa_demote_result_t evictionTryNumaDemote(void *db, char *key, void *val, int *target_node) {
    if (!server.numa_demote_enabled) {
        return NUMA_DEMOTE_SKIP;
    }
    
    redisDb *rdb = (redisDb *)db;
    robj *val_obj = (robj *)val;
    
    if (!rdb || !key || !val_obj || !target_node) {
        return NUMA_DEMOTE_SKIP;
    }
    
    /* Get the object size. */
    size_t obj_size = objectComputeSize(NULL, val_obj, 0, rdb->id);
    if (obj_size < server.numa_demote_min_size) {
        return NUMA_DEMOTE_SKIP; /* Too small to be worth migrating. */
    }
    
    /* Get the current NUMA node (strings use the SDS allocation base). */
    int current_node = -1;
    if (val_obj->ptr) {
        if (val_obj->type == OBJ_STRING) {
            current_node = numa_get_node_id(sdsAllocPtr(val_obj->ptr));
        } else {
            current_node = numa_get_node_id(val_obj->ptr);
        }
    }
    if (current_node < 0) {
        current_node = numa_pool_get_node();
    }
    
    /* Find the best target node. */
    int best_node = numaFindBestDemoteNode(obj_size, current_node);
    if (best_node < 0) {
        *target_node = -1;
        return NUMA_DEMOTE_NO_NODE;
    }
    
    /* Perform the migration. */
    robj keyobj;
    initStaticStringObject(keyobj, key);
    
    int result = numa_migrate_single_key(rdb, &keyobj, best_node);
    
    if (result == NUMA_KEY_MIGRATE_OK) {
        *target_node = best_node;
        server.stat_numa_demotions++;
        server.stat_numa_demote_bytes += obj_size;
        
        /* Track the distance distribution. */
        int dist = numa_distance(current_node, best_node);
        if (dist <= 20) {
            server.stat_numa_demote_near++;
        } else {
            server.stat_numa_demote_far++;
        }
        
        serverLog(LL_VERBOSE,
            "[NUMA Demote] Key demoted: node %d -> %d, size=%zu, distance=%d",
            current_node, best_node, obj_size, dist);
        return NUMA_DEMOTE_OK;
    }
    
    server.stat_numa_demote_failed++;
    return NUMA_DEMOTE_FAILED;
}

/*
 * numaGetNodeBandwidthUsage - get the bandwidth utilization of a node
 *
 * Returns: 0.0 ~ 1.0, -1.0 for an invalid node
 */
double numaGetNodeBandwidthUsage(int node_id) {
    return numa_bw_get_usage(node_id);
}

#else /* !HAVE_NUMA */

/* Empty implementation for non-NUMA environments. */

double numaGetNodePressure(int node_id) {
    (void)node_id;
    return 1.0;
}

size_t numaGetNodeFreeMemory(int node_id) {
    (void)node_id;
    return 0;
}

int numaFindBestDemoteNode(size_t object_size, int current_node) {
    (void)object_size;
    (void)current_node;
    return -1;
}

numa_demote_result_t evictionTryNumaDemote(void *db, char *key, void *val, int *target_node) {
    (void)db; (void)key; (void)val;
    if (target_node) *target_node = -1;
    return NUMA_DEMOTE_SKIP;
}

double numaGetNodeBandwidthUsage(int node_id) {
    (void)node_id;
    return -1.0;
}

#endif /* HAVE_NUMA */
