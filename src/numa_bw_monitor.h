/* numa_bw_monitor.h - real-time NUMA node bandwidth monitoring module
 *
 * Provides real-time collection and query interfaces for NUMA node bandwidth
 * utilization. Supports multiple backends: resctrl (Intel RDT), numastat
 * (generic fallback), and manual configuration. serverCron calls
 * numa_bw_monitor_sample() every second; consumers read the utilization
 * (0.0~1.0) via numa_bw_get_usage().
 */
#ifndef NUMA_BW_MONITOR_H
#define NUMA_BW_MONITOR_H

#include <stdint.h>
#include <stddef.h>

#define NUMA_BW_MAX_NODES       16
#define NUMA_BW_SAMPLE_INTERVAL_MS  1000    /* Default sampling interval of 1 second. */

/* Backend types. */
#define NUMA_BW_BACKEND_RESCTRL     0   /* Intel RDT resctrl (most accurate). */
#define NUMA_BW_BACKEND_NUMASTAT    1   /* /sys numastat delta (generic). */
#define NUMA_BW_BACKEND_MANUAL      2   /* Manual setting (C-TAP measurement results). */

/* Per-node bandwidth state. */
typedef struct {
    double max_bandwidth_mbps;      /* Max bandwidth (MB/s), C-TAP baseline. */
    double current_bw_mbps;         /* Current bandwidth (MB/s), real-time sample. */
    double bw_usage;                /* Utilization = current/max (0.0~1.0). */
    uint64_t last_sample_us;        /* Last sample time (microseconds). */
    uint64_t total_bytes_prev;      /* Cumulative bytes/pages at the last sample. */
} numa_bw_node_t;

/* Global monitor. */
typedef struct {
    numa_bw_node_t nodes[NUMA_BW_MAX_NODES];
    int num_nodes;
    int backend;                    /* Backend in use. */
    uint32_t sample_interval_ms;    /* Sampling interval. */
    int initialized;                /* Whether initialized. */
} numa_bw_monitor_t;

/* ========== Public interface ========== */

/* Initialize the bandwidth monitor, auto-detecting the best backend. Returns 0 on success. */
int  numa_bw_monitor_init(void);

/* Sample once (called by serverCron every second). */
void numa_bw_monitor_sample(void);

/* Get the bandwidth utilization of a node (0.0~1.0), -1 for an invalid node. */
double numa_bw_get_usage(int node_id);

/* Get the current bandwidth (MB/s). */
double numa_bw_get_current_mbps(int node_id);

/* Set the max bandwidth baseline of a node (from C-TAP measurements or the config file). */
void numa_bw_set_max_bandwidth(int node_id, double max_mbps);

/* Get the current backend type string. */
const char* numa_bw_get_backend_name(void);

/* Get the global monitor pointer (read-only). */
const numa_bw_monitor_t* numa_bw_get_monitor(void);

/* Clean up resources. */
void numa_bw_monitor_cleanup(void);

#endif /* NUMA_BW_MONITOR_H */
