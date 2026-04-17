/* evict.h - 内存淘汰策略与NUMA联动接口
 *
 * 本头文件定义淘汰池扩展结构体及NUMA降级接口，
 * 支持在内存超限时优先迁移到其他NUMA节点而非直接淘汰。
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2024, Redis-CXL Project
 * All rights reserved.
 */

#ifndef __EVICT_H__
#define __EVICT_H__

#include <stdint.h>
#include <stddef.h>

/* 淘汰池大小 */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255

/* 淘汰池扩展条目（与 evict.c 中的定义保持一致） */
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) */
    char *key;                  /* Key name (sds) */
    char *cached;               /* Cached SDS object for key name. */
    int dbid;                   /* Key DB number. */
    
    /* === NUMA 联动扩展字段 === */
    int current_node;           /* 当前所在 NUMA 节点 (-1 = 未知) */
    size_t object_size;         /* 对象大小 (用于迁移决策) */
    uint8_t numa_migrated;      /* 已迁移次数 (避免反复迁移) */
};

/* NUMA 降级状态码 */
typedef enum {
    NUMA_DEMOTE_OK = 0,         /* 降级成功 */
    NUMA_DEMOTE_NO_NODE,        /* 无可用节点 */
    NUMA_DEMOTE_FAILED,         /* 迁移失败 */
    NUMA_DEMOTE_SKIP            /* 跳过 (对象太小/不支持/未启用) */
} numa_demote_result_t;

/* NUMA 降级配置 */
typedef struct {
    int enabled;                /* 是否启用 NUMA 降级 */
    size_t min_demote_size;     /* 最小降级对象大小 (默认 1KB) */
    uint8_t max_migrate_count;  /* 最大迁移次数 (默认 3) */
    double node_pressure_threshold; /* 节点压力阈值 (默认 0.9) */
    
    /* === 距离优先配置 === */
    int distance_weight;        /* 距离权重 (默认 40, 范围 0-100) */
    int pressure_weight;        /* 压力权重 (默认 30, 范围 0-100) */
    int bandwidth_weight;       /* 带宽权重 (默认 30, 范围 0-100) */
    double bw_saturation_threshold; /* 带宽饱和排除阈值 (默认 0.95) */
    int prefer_closer_node;     /* 是否优先选择更近节点 (默认 1) */
} numa_demote_config_t;

/* 默认配置值 */
#define NUMA_DEMOTE_DEFAULT_MIN_SIZE       1024    /* 1KB */
#define NUMA_DEMOTE_DEFAULT_MAX_MIGRATE    3
#define NUMA_DEMOTE_DEFAULT_PRESSURE_THRES 0.9
#define NUMA_DEMOTE_DEFAULT_DISTANCE_WEIGHT 40     /* 从 70 降至 40 */
#define NUMA_DEMOTE_DEFAULT_PRESSURE_WEIGHT 30
#define NUMA_DEMOTE_DEFAULT_BANDWIDTH_WEIGHT 30    /* 新增带宽权重 */
#define NUMA_DEMOTE_DEFAULT_BW_SAT_THRESHOLD 0.95 /* 带宽饱和排除阈值 */

/* 最大 NUMA 节点数 */
#define MAX_NUMA_NODES 16

/* ========== 公共接口 ========== */

/*
 * evictionTryNumaDemote - 尝试将对象降级到其他 NUMA 节点
 *
 * @db: 数据库指针
 * @key: 键名 (sds)
 * @val: 值对象
 * @target_node: 输出参数，返回目标节点ID
 *
 * 返回值: numa_demote_result_t
 */
numa_demote_result_t evictionTryNumaDemote(void *db, char *key, void *val, int *target_node);

/*
 * numaFindBestDemoteNode - 找到最佳降级目标节点
 *
 * 选择策略: 距离优先 + 压力感知
 * 使用加权评分综合距离和压力因素
 *
 * @object_size: 对象大小
 * @current_node: 当前节点
 *
 * 返回值: 最佳节点 ID, -1 表示无可用节点
 */
int numaFindBestDemoteNode(size_t object_size, int current_node);

/*
 * numaGetNodePressure - 获取节点内存压力
 *
 * 返回值: 0.0 ~ 1.0, 越大表示压力越高
 */
double numaGetNodePressure(int node_id);

/*
 * numaGetNodeFreeMemory - 获取节点空闲内存
 *
 * 返回值: 空闲内存字节数
 */
size_t numaGetNodeFreeMemory(int node_id);

/*
 * evictionDemoteConfigDefaults - 获取默认降级配置
 */
void evictionDemoteConfigDefaults(numa_demote_config_t *config);

/*
 * evictionSetDemoteConfig - 设置降级配置
 */
void evictionSetDemoteConfig(const numa_demote_config_t *config);

/*
 * evictionGetDemoteConfig - 获取当前降级配置
 */
void evictionGetDemoteConfig(numa_demote_config_t *config);

/*
 * numaGetNodeBandwidthUsage - 获取节点带宽利用率
 *
 * 返回值: 0.0 ~ 1.0, -1.0 表示无效节点
 */
double numaGetNodeBandwidthUsage(int node_id);

#endif /* __EVICT_H__ */
