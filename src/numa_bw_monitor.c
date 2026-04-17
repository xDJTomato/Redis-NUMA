/* numa_bw_monitor.c - NUMA节点带宽实时监控模块实现
 *
 * 提供三种后端实现：
 *   - resctrl: Intel RDT resctrl 接口（最精确）
 *   - numastat: /sys 文件系统 numastat（通用 fallback）
 *   - manual: 手动配置（C-TAP 测量结果）
 *
 * Copyright (c) 2024, Redis-CXL Project
 */

#define _GNU_SOURCE
#include "numa_bw_monitor.h"

#ifdef HAVE_NUMA

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <numa.h>

/* ========== 日志输出 ========== */

extern void _serverLog(int level, const char *fmt, ...);
#define LL_DEBUG 0
#define LL_VERBOSE 1
#define LL_NOTICE 2
#define LL_WARNING 3
#define BW_LOG(level, fmt, ...) _serverLog(level, "[BW-Monitor] " fmt, ##__VA_ARGS__)
#define BW_LOG_SIMPLE(level, msg) _serverLog(level, "[BW-Monitor] " msg)

/* ========== 全局状态 ========== */

static numa_bw_monitor_t g_bw_monitor;

/* 默认最大带宽 50GB/s（保守估计） */
#define NUMA_BW_DEFAULT_MAX_MBPS    50000.0

/* ========== 辅助函数 ========== */

/* 获取当前时间（微秒） */
static uint64_t get_current_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + tv.tv_usec;
}

/* 检查文件是否存在 */
static int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* 读取 resctrl mbm_total_bytes */
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

/* 读取 numastat 页面访问数 */
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

/* 自动检测最佳后端 */
static int detect_best_backend(void) {
    /* 检查 resctrl 是否可用 */
    if (file_exists("/sys/fs/resctrl/mon_data")) {
        /* 检查是否有 mon_L3_XX 目录 */
        char path[256];
        snprintf(path, sizeof(path), "/sys/fs/resctrl/mon_data/mon_L3_00");
        if (file_exists(path)) {
            return NUMA_BW_BACKEND_RESCTRL;
        }
    }
    
    /* 检查 numastat 是否可用 */
    if (file_exists("/sys/devices/system/node/node0/numastat")) {
        return NUMA_BW_BACKEND_NUMASTAT;
    }
    
    /* 回退到手动模式 */
    return NUMA_BW_BACKEND_MANUAL;
}

/* 获取后端名称 */
static const char* backend_name(int backend) {
    switch (backend) {
        case NUMA_BW_BACKEND_RESCTRL:   return "resctrl";
        case NUMA_BW_BACKEND_NUMASTAT:  return "numastat";
        case NUMA_BW_BACKEND_MANUAL:    return "manual";
        default:                        return "unknown";
    }
}

/* clamp 值到 [0.0, 1.0] */
static double clamp_01(double val) {
    if (val < 0.0) return 0.0;
    if (val > 1.0) return 1.0;
    return val;
}

/* ========== resctrl 后端采样 ========== */

static void sample_resctrl(void) {
    uint64_t now = get_current_time_us();
    
    for (int i = 0; i < g_bw_monitor.num_nodes; i++) {
        numa_bw_node_t *node = &g_bw_monitor.nodes[i];
        uint64_t curr_bytes = read_resctrl_bytes(i);
        
        if (node->last_sample_us == 0) {
            /* 首次采样，只记录初始值 */
            node->total_bytes_prev = curr_bytes;
            node->last_sample_us = now;
            continue;
        }
        
        uint64_t delta_us = now - node->last_sample_us;
        if (delta_us == 0) continue;  /* 避免除零 */
        
        /* 计算带宽（MB/s） */
        if (curr_bytes >= node->total_bytes_prev) {
            uint64_t delta_bytes = curr_bytes - node->total_bytes_prev;
            double delta_sec = (double)delta_us / 1000000.0;
            node->current_bw_mbps = (double)delta_bytes / (1024.0 * 1024.0) / delta_sec;
        } else {
            /* 计数器回绕或重置，不计算 */
            node->current_bw_mbps = 0.0;
        }
        
        /* 计算利用率 */
        if (node->max_bandwidth_mbps > 0) {
            node->bw_usage = clamp_01(node->current_bw_mbps / node->max_bandwidth_mbps);
        } else {
            node->bw_usage = 0.0;
        }
        
        node->total_bytes_prev = curr_bytes;
        node->last_sample_us = now;
    }
}

/* ========== numastat 后端采样 ========== */

static void sample_numastat(void) {
    uint64_t now = get_current_time_us();
    
    for (int i = 0; i < g_bw_monitor.num_nodes; i++) {
        numa_bw_node_t *node = &g_bw_monitor.nodes[i];
        uint64_t curr_pages = read_numastat_pages(i);
        
        if (node->last_sample_us == 0) {
            /* 首次采样，只记录初始值 */
            node->total_bytes_prev = curr_pages;
            node->last_sample_us = now;
            continue;
        }
        
        uint64_t delta_us = now - node->last_sample_us;
        if (delta_us == 0) continue;  /* 避免除零 */
        
        /* 计算带宽（MB/s），假设页面大小为 4KB */
        if (curr_pages >= node->total_bytes_prev) {
            uint64_t delta_pages = curr_pages - node->total_bytes_prev;
            double delta_sec = (double)delta_us / 1000000.0;
            double delta_bytes = (double)delta_pages * 4096.0;
            node->current_bw_mbps = delta_bytes / (1024.0 * 1024.0) / delta_sec;
        } else {
            /* 计数器回绕或重置，不计算 */
            node->current_bw_mbps = 0.0;
        }
        
        /* 计算利用率 */
        if (node->max_bandwidth_mbps > 0) {
            node->bw_usage = clamp_01(node->current_bw_mbps / node->max_bandwidth_mbps);
        } else {
            node->bw_usage = 0.0;
        }
        
        node->total_bytes_prev = curr_pages;
        node->last_sample_us = now;
    }
}

/* ========== manual 后端采样 ========== */

static void sample_manual(void) {
    /* manual 后端不进行采样，使用静态值 */
    /* bw_usage 保持用户设置的值或 0 */
    (void)0;  /* 空操作，避免编译警告 */
}

/* ========== 公共接口实现 ========== */

/* 初始化带宽监控器 */
int numa_bw_monitor_init(void) {
    if (g_bw_monitor.initialized) {
        BW_LOG_SIMPLE(LL_WARNING, "Already initialized");
        return 0;
    }
    
    /* 检查 NUMA 可用性 */
    if (numa_available() < 0) {
        BW_LOG_SIMPLE(LL_WARNING, "NUMA not available");
        return -1;
    }
    
    /* 获取节点数 */
    int max_node = numa_max_node();
    if (max_node < 0 || max_node >= NUMA_BW_MAX_NODES) {
        BW_LOG(LL_WARNING, "Invalid NUMA node count: %d", max_node + 1);
        return -1;
    }
    
    memset(&g_bw_monitor, 0, sizeof(g_bw_monitor));
    g_bw_monitor.num_nodes = max_node + 1;
    g_bw_monitor.sample_interval_ms = NUMA_BW_SAMPLE_INTERVAL_MS;
    
    /* 检测最佳后端 */
    g_bw_monitor.backend = detect_best_backend();
    
    /* 初始化每个节点 */
    uint64_t now = get_current_time_us();
    for (int i = 0; i < g_bw_monitor.num_nodes; i++) {
        g_bw_monitor.nodes[i].max_bandwidth_mbps = NUMA_BW_DEFAULT_MAX_MBPS;
        g_bw_monitor.nodes[i].last_sample_us = now;
        
        /* 首次读取当前值作为基准 */
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

/* 采样一次 */
void numa_bw_monitor_sample(void) {
    if (!g_bw_monitor.initialized) return;
    
    /* 检查采样间隔 */
    uint64_t now = get_current_time_us();
    if (g_bw_monitor.num_nodes > 0) {
        uint64_t elapsed_ms = (now - g_bw_monitor.nodes[0].last_sample_us) / 1000;
        if (elapsed_ms < g_bw_monitor.sample_interval_ms) {
            return;  /* 还未到采样时间 */
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

/* 获取节点带宽利用率 */
double numa_bw_get_usage(int node_id) {
    if (!g_bw_monitor.initialized) return -1.0;
    if (node_id < 0 || node_id >= g_bw_monitor.num_nodes) return -1.0;
    
    return g_bw_monitor.nodes[node_id].bw_usage;
}

/* 获取当前带宽 */
double numa_bw_get_current_mbps(int node_id) {
    if (!g_bw_monitor.initialized) return -1.0;
    if (node_id < 0 || node_id >= g_bw_monitor.num_nodes) return -1.0;
    
    return g_bw_monitor.nodes[node_id].current_bw_mbps;
}

/* 设置节点最大带宽 */
void numa_bw_set_max_bandwidth(int node_id, double max_mbps) {
    if (!g_bw_monitor.initialized) return;
    if (node_id < 0 || node_id >= g_bw_monitor.num_nodes) return;
    if (max_mbps <= 0) return;
    
    g_bw_monitor.nodes[node_id].max_bandwidth_mbps = max_mbps;
    
    /* 如果是 manual 后端，设置 bw_usage 为固定值（假设当前使用一半） */
    if (g_bw_monitor.backend == NUMA_BW_BACKEND_MANUAL) {
        g_bw_monitor.nodes[node_id].bw_usage = 0.5;  /* 默认假设 50% 利用率 */
        g_bw_monitor.nodes[node_id].current_bw_mbps = max_mbps * 0.5;
    }
    
    BW_LOG(LL_VERBOSE, "Node %d max bandwidth set to %.2f MB/s", node_id, max_mbps);
}

/* 获取后端名称 */
const char* numa_bw_get_backend_name(void) {
    if (!g_bw_monitor.initialized) return "uninitialized";
    return backend_name(g_bw_monitor.backend);
}

/* 获取监控器指针 */
const numa_bw_monitor_t* numa_bw_get_monitor(void) {
    if (!g_bw_monitor.initialized) return NULL;
    return &g_bw_monitor;
}

/* 清理资源 */
void numa_bw_monitor_cleanup(void) {
    if (!g_bw_monitor.initialized) return;
    
    memset(&g_bw_monitor, 0, sizeof(g_bw_monitor));
    BW_LOG_SIMPLE(LL_NOTICE, "Cleaned up");
}

#else /* !HAVE_NUMA */

/* ========== NUMA 未启用时的空实现 ========== */

int numa_bw_monitor_init(void) { return -1; }
void numa_bw_monitor_sample(void) { }
double numa_bw_get_usage(int node_id) { (void)node_id; return -1.0; }
double numa_bw_get_current_mbps(int node_id) { (void)node_id; return -1.0; }
void numa_bw_set_max_bandwidth(int node_id, double max_mbps) { (void)node_id; (void)max_mbps; }
const char* numa_bw_get_backend_name(void) { return "disabled"; }
const numa_bw_monitor_t* numa_bw_get_monitor(void) { return NULL; }
void numa_bw_monitor_cleanup(void) { }

#endif /* HAVE_NUMA */
