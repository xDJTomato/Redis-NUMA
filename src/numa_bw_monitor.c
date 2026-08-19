/* numa_bw_monitor.c - real-time NUMA node bandwidth monitoring module
 *
 * Provides three backend implementations:
 *   - resctrl: Intel RDT resctrl interface (most accurate)
 *   - numastat: /sys numastat filesystem (generic fallback)
 *   - manual: manual configuration (C-TAP measurement results)
 *
 * Copyright (c) 2024, Redis-CXL Project
 */

#define _GNU_SOURCE
#include "numa_bw_monitor.h"
#include "server.h"
#include "zmalloc.h"

#ifdef HAVE_NUMA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <numa.h>

/* ========== Logging ========== */

extern void _serverLog(int level, const char *fmt, ...);
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define BW_LOG(level, fmt, ...) _serverLog(level, "[BW-Monitor] " fmt, ##__VA_ARGS__)
#define BW_LOG_SIMPLE(level, msg) _serverLog(level, "[BW-Monitor] " msg)

/* ========== Global state ========== */

static numa_bw_monitor_t g_bw_monitor;

/* Default max bandwidth 50GB/s (conservative estimate). */
#define NUMA_BW_DEFAULT_MAX_MBPS    50000.0

/* ========== Helper functions ========== */

/* Get the current time (microseconds). */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* Check whether a file exists. */
static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Read the resctrl mbm_total_bytes counter. */
static uint64_t read_resctrl_bytes(int node_id) {
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/fs/resctrl/mon_data/mon_L3_%02d/mbm_total_bytes", node_id);
    
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    
    uint64_t val = 0;
    if (fscanf(fp, "%lu", &val) != 1) {
        val = 0;
    }
    fclose(fp);
    return val;
}

/* Read the numastat page access counts. */
static uint64_t read_numastat_pages(int node_id) {
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/node/node%d/numastat", node_id);
    
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    
    uint64_t total = 0;
    char name[64];
    uint64_t val;
    
    while (fscanf(fp, "%63s %lu", name, &val) == 2) {
        if (strcmp(name, "numa_hit") == 0 || strcmp(name, "numa_miss") == 0) {
            total += val;
        }
    }
    fclose(fp);
    return total;
}

/* Auto-detect the best backend. */
static int detect_best_backend(void) {
    /* Check whether resctrl is available. */
    if (file_exists("/sys/fs/resctrl/mon_data")) {
        /* Check whether a mon_L3_XX directory exists. */
        char path[256];
        snprintf(path, sizeof(path), "/sys/fs/resctrl/mon_data/mon_L3_00");
        if (file_exists(path)) {
            return NUMA_BW_BACKEND_RESCTRL;
        }
    }
    
    /* Check whether numastat is available. */
    if (file_exists("/sys/devices/system/node/node0/numastat")) {
        return NUMA_BW_BACKEND_NUMASTAT;
    }
    
    /* Fall back to manual mode. */
    return NUMA_BW_BACKEND_MANUAL;
}

/* Get the backend name. */
static const char* backend_name(int backend) {
    switch (backend) {
        case NUMA_BW_BACKEND_RESCTRL:   return "resctrl";
        case NUMA_BW_BACKEND_NUMASTAT:  return "numastat";
        case NUMA_BW_BACKEND_MANUAL:    return "manual";
        default:                        return "unknown";
    }
}

/* Clamp a value to [0.0, 1.0]. */
static double clamp_01(double val) {
    if (val < 0.0) return 0.0;
    if (val > 1.0) return 1.0;
    return val;
}

/* ========== resctrl backend sampling ========== */

static void sample_resctrl(void) {
    uint64_t now = get_current_time_us();
    
    for (int i = 0; i < g_bw_monitor.num_nodes; i++) {
        numa_bw_node_t *node = &g_bw_monitor.nodes[i];
        uint64_t curr_bytes = read_resctrl_bytes(i);
        
        if (node->last_sample_us == 0) {
            /* First sample, only record the initial value. */
            node->total_bytes_prev = curr_bytes;
            node->last_sample_us = now;
            continue;
        }
        
        uint64_t delta_us = now - node->last_sample_us;
        if (delta_us == 0) continue;  /* Avoid division by zero. */
        
        /* Compute the bandwidth (MB/s). */
        if (curr_bytes >= node->total_bytes_prev) {
            uint64_t delta_bytes = curr_bytes - node->total_bytes_prev;
            double delta_sec = (double)delta_us / 1000000.0;
            node->current_bw_mbps = (double)delta_bytes / (1024.0 * 1024.0) / delta_sec;
        } else {
            /* Counter wrapped or reset, skip the computation. */
            node->current_bw_mbps = 0.0;
        }
        
        /* Compute the utilization. */
        if (node->max_bandwidth_mbps > 0) {
            node->bw_usage = clamp_01(node->current_bw_mbps / node->max_bandwidth_mbps);
        } else {
            node->bw_usage = 0.0;
        }
        
        node->total_bytes_prev = curr_bytes;
        node->last_sample_us = now;
    }
}

/* ========== numastat backend sampling ========== */

static void sample_numastat(void) {
    uint64_t now = get_current_time_us();
    
    for (int i = 0; i < g_bw_monitor.num_nodes; i++) {
        numa_bw_node_t *node = &g_bw_monitor.nodes[i];
        uint64_t curr_pages = read_numastat_pages(i);
        
        if (node->last_sample_us == 0) {
            /* First sample, only record the initial value. */
            node->total_bytes_prev = curr_pages;
            node->last_sample_us = now;
            continue;
        }
        
        uint64_t delta_us = now - node->last_sample_us;
        if (delta_us == 0) continue;  /* Avoid division by zero. */
        
        /* Compute the bandwidth (MB/s), assuming a 4KB page size. */
        if (curr_pages >= node->total_bytes_prev) {
            uint64_t delta_pages = curr_pages - node->total_bytes_prev;
            double delta_sec = (double)delta_us / 1000000.0;
            double delta_bytes = (double)delta_pages * 4096.0;
            node->current_bw_mbps = delta_bytes / (1024.0 * 1024.0) / delta_sec;
        } else {
            /* Counter wrapped or reset, skip the computation. */
            node->current_bw_mbps = 0.0;
        }
        
        /* Compute the utilization. */
        if (node->max_bandwidth_mbps > 0) {
            node->bw_usage = clamp_01(node->current_bw_mbps / node->max_bandwidth_mbps);
        } else {
            node->bw_usage = 0.0;
        }
        
        node->total_bytes_prev = curr_pages;
        node->last_sample_us = now;
    }
}

/* ========== manual backend sampling ========== */

static void sample_manual(void) {
    /* The manual backend does not sample; it uses static values. */
    /* bw_usage keeps the user-set value or 0. */
    (void)0;  /* No-op to avoid compiler warnings. */
}

/* ========== Public interface implementation ========== */

/* Initialize the bandwidth monitor. */
int numa_bw_monitor_init(void) {
    if (g_bw_monitor.initialized) {
        BW_LOG_SIMPLE(LL_WARNING, "Already initialized");
        return 0;
    }
    
    /* Check NUMA availability. */
    if (numa_available() < 0) {
        BW_LOG_SIMPLE(LL_WARNING, "NUMA not available");
        return -1;
    }
    
    /* Get the number of nodes. */
    int max_node = numa_max_node();
    if (max_node < 0 || max_node >= NUMA_BW_MAX_NODES) {
        BW_LOG(LL_WARNING, "Invalid NUMA node count: %d", max_node + 1);
        return -1;
    }
    
    memset(&g_bw_monitor, 0, sizeof(g_bw_monitor));
    g_bw_monitor.num_nodes = max_node + 1;
    g_bw_monitor.sample_interval_ms = NUMA_BW_SAMPLE_INTERVAL_MS;
    
    /* Detect the best backend. */
    g_bw_monitor.backend = detect_best_backend();
    
    /* Initialize each node. */
    uint64_t now = get_current_time_us();
    for (int i = 0; i < g_bw_monitor.num_nodes; i++) {
        g_bw_monitor.nodes[i].max_bandwidth_mbps = NUMA_BW_DEFAULT_MAX_MBPS;
        g_bw_monitor.nodes[i].last_sample_us = now;
        
        /* First read of the current value as the baseline. */
        if (g_bw_monitor.backend == NUMA_BW_BACKEND_RESCTRL) {
            g_bw_monitor.nodes[i].total_bytes_prev = read_resctrl_bytes(i);
        } else if (g_bw_monitor.backend == NUMA_BW_BACKEND_NUMASTAT) {
            g_bw_monitor.nodes[i].total_bytes_prev = read_numastat_pages(i);
        }
    }
    
    g_bw_monitor.initialized = 1;
    
    BW_LOG(LL_NOTICE, "Initialized: nodes=%d, backend=%s",
           g_bw_monitor.num_nodes, backend_name(g_bw_monitor.backend));
    
    return 0;
}

/* Sample once. */
void numa_bw_monitor_sample(void) {
    if (!g_bw_monitor.initialized) return;
    
    /* Check the sampling interval. */
    uint64_t now = get_current_time_us();
    if (g_bw_monitor.num_nodes > 0) {
        uint64_t elapsed_ms = (now - g_bw_monitor.nodes[0].last_sample_us) / 1000;
        if (elapsed_ms < g_bw_monitor.sample_interval_ms) {
            return;  /* Not yet time to sample. */
        }
    }
    
    switch (g_bw_monitor.backend) {
        case NUMA_BW_BACKEND_RESCTRL:
            sample_resctrl();
            break;
        case NUMA_BW_BACKEND_NUMASTAT:
            sample_numastat();
            break;
        case NUMA_BW_BACKEND_MANUAL:
            sample_manual();
            break;
        default:
            break;
    }
}

/* Get the bandwidth utilization of a node. */
double numa_bw_get_usage(int node_id) {
    if (!g_bw_monitor.initialized) return -1.0;
    if (node_id < 0 || node_id >= g_bw_monitor.num_nodes) return -1.0;
    
    return g_bw_monitor.nodes[node_id].bw_usage;
}

/* Get the current bandwidth. */
double numa_bw_get_current_mbps(int node_id) {
    if (!g_bw_monitor.initialized) return -1.0;
    if (node_id < 0 || node_id >= g_bw_monitor.num_nodes) return -1.0;

    return g_bw_monitor.nodes[node_id].current_bw_mbps;
}

/* Node pressure cache: same 1s TTL evict_numa.c used to keep independently. */
#define NODE_PRESSURE_CACHE_TTL_MS 1000
static double g_node_pressure_cache[NUMA_BW_MAX_NODES];
static long long g_pressure_cache_time_ms[NUMA_BW_MAX_NODES];

double numa_bw_get_node_pressure(int node_id) {
    int max_node = numa_max_node();
    if (node_id < 0 || node_id > max_node || node_id >= NUMA_BW_MAX_NODES) {
        return 1.0; /* Invalid nodes report full pressure. */
    }

    long long now = mstime();
    if (g_pressure_cache_time_ms[node_id] > 0 &&
        (now - g_pressure_cache_time_ms[node_id]) < NODE_PRESSURE_CACHE_TTL_MS) {
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
     * large multi-hundred-GB dual-socket server would stay near-zero and
     * never trigger migration/demotion).
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

    g_node_pressure_cache[node_id] = pressure;
    g_pressure_cache_time_ms[node_id] = now;

    return pressure;
}

/* Set the max bandwidth of a node. */
void numa_bw_set_max_bandwidth(int node_id, double max_mbps) {
    if (!g_bw_monitor.initialized) return;
    if (node_id < 0 || node_id >= g_bw_monitor.num_nodes) return;
    if (max_mbps <= 0) return;
    
    g_bw_monitor.nodes[node_id].max_bandwidth_mbps = max_mbps;
    
    /* With the manual backend, set bw_usage to a fixed value (assume half is in use). */
    if (g_bw_monitor.backend == NUMA_BW_BACKEND_MANUAL) {
        g_bw_monitor.nodes[node_id].bw_usage = 0.5;  /* Default assumption of 50% utilization. */
        g_bw_monitor.nodes[node_id].current_bw_mbps = max_mbps * 0.5;
    }
    
    BW_LOG(LL_VERBOSE, "Node %d max bandwidth set to %.2f MB/s", node_id, max_mbps);
}

/* Get the backend name. */
const char* numa_bw_get_backend_name(void) {
    if (!g_bw_monitor.initialized) return "uninitialized";
    return backend_name(g_bw_monitor.backend);
}

/* Get the monitor pointer. */
const numa_bw_monitor_t* numa_bw_get_monitor(void) {
    if (!g_bw_monitor.initialized) return NULL;
    return &g_bw_monitor;
}

/* Clean up resources. */
void numa_bw_monitor_cleanup(void) {
    if (!g_bw_monitor.initialized) return;
    
    memset(&g_bw_monitor, 0, sizeof(g_bw_monitor));
    BW_LOG_SIMPLE(LL_NOTICE, "Cleaned up");
}

#else /* !HAVE_NUMA */

/* ========== Empty implementation when NUMA is disabled ========== */

int numa_bw_monitor_init(void) { return -1; }
void numa_bw_monitor_sample(void) { }
double numa_bw_get_usage(int node_id) { (void)node_id; return -1.0; }
double numa_bw_get_current_mbps(int node_id) { (void)node_id; return -1.0; }
double numa_bw_get_node_pressure(int node_id) { (void)node_id; return 1.0; }
void numa_bw_set_max_bandwidth(int node_id, double max_mbps) { (void)node_id; (void)max_mbps; }
const char* numa_bw_get_backend_name(void) { return "disabled"; }
const numa_bw_monitor_t* numa_bw_get_monitor(void) { return NULL; }
void numa_bw_monitor_cleanup(void) { }

#endif /* HAVE_NUMA */
