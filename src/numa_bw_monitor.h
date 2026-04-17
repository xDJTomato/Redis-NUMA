/* numa_bw_monitor.h - NUMA节点带宽实时监控模块
 *
 * 提供 NUMA 节点带宽利用率的实时采集与查询接口。
 * 支持多后端：resctrl（Intel RDT）、numastat（通用 fallback）、手动配置。
 * serverCron 每秒调用 numa_bw_monitor_sample() 采样，
 * 消费方通过 numa_bw_get_usage() 获取节点带宽利用率（0.0~1.0）。
 */
#ifndef NUMA_BW_MONITOR_H
#define NUMA_BW_MONITOR_H

#include <stdint.h>
#include <stddef.h>

#define NUMA_BW_MAX_NODES       16
#define NUMA_BW_SAMPLE_INTERVAL_MS  1000    /* 默认采样间隔 1 秒 */

/* 后端类型 */
#define NUMA_BW_BACKEND_RESCTRL     0   /* Intel RDT resctrl (最精确) */
#define NUMA_BW_BACKEND_NUMASTAT    1   /* /sys numastat delta (通用) */
#define NUMA_BW_BACKEND_MANUAL      2   /* 手动设置 (C-TAP 测量结果) */

/* 单节点带宽状态 */
typedef struct {
    double max_bandwidth_mbps;      /* 最大带宽(MB/s)，C-TAP 基线 */
    double current_bw_mbps;         /* 当前带宽(MB/s)，实时采样 */
    double bw_usage;                /* 利用率 = current/max (0.0~1.0) */
    uint64_t last_sample_us;        /* 上次采样时间（微秒）*/
    uint64_t total_bytes_prev;      /* 上次采样的累计字节/页数 */
} numa_bw_node_t;

/* 全局监控器 */
typedef struct {
    numa_bw_node_t nodes[NUMA_BW_MAX_NODES];
    int num_nodes;
    int backend;                    /* 当前使用的后端 */
    uint32_t sample_interval_ms;    /* 采样间隔 */
    int initialized;                /* 是否已初始化 */
} numa_bw_monitor_t;

/* ========== 公共接口 ========== */

/* 初始化带宽监控器，自动检测最佳后端。成功返回0 */
int  numa_bw_monitor_init(void);

/* 采样一次（由 serverCron 每秒调用）*/
void numa_bw_monitor_sample(void);

/* 获取节点带宽利用率 (0.0~1.0)，-1 表示无效节点 */
double numa_bw_get_usage(int node_id);

/* 获取当前带宽 (MB/s) */
double numa_bw_get_current_mbps(int node_id);

/* 设置节点最大带宽基线（来自 C-TAP 测量或配置文件）*/
void numa_bw_set_max_bandwidth(int node_id, double max_mbps);

/* 获取当前后端类型字符串 */
const char* numa_bw_get_backend_name(void);

/* 获取全局监控器指针（只读） */
const numa_bw_monitor_t* numa_bw_get_monitor(void);

/* 清理资源 */
void numa_bw_monitor_cleanup(void);

#endif /* NUMA_BW_MONITOR_H */
