/* evict_numa.c - NUMA降级迁移实现
 *
 * 本模块实现淘汰池与NUMA迁移策略的联动，在内存超限时
 * 优先将冷数据迁移到其他NUMA节点而非直接淘汰。
 *
 * 核心特性：
 * - 距离优先节点选择（优先选择更近的节点）
 * - 压力感知（避免迁移到高压节点）
 * - 带宽感知（避免迁移到带宽饱和节点）
 * - 加权评分决策（距离40% + 压力30% + 带宽30%）
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

/* ========== 全局配置 ========== */

static numa_demote_config_t g_demote_config = {
    .enabled = 1,
    .min_demote_size = NUMA_DEMOTE_DEFAULT_MIN_SIZE,
    .max_migrate_count = NUMA_DEMOTE_DEFAULT_MAX_MIGRATE,
    .node_pressure_threshold = NUMA_DEMOTE_DEFAULT_PRESSURE_THRES,
    .distance_weight = NUMA_DEMOTE_DEFAULT_DISTANCE_WEIGHT,
    .pressure_weight = NUMA_DEMOTE_DEFAULT_PRESSURE_WEIGHT,
    .bandwidth_weight = NUMA_DEMOTE_DEFAULT_BANDWIDTH_WEIGHT,
    .bw_saturation_threshold = NUMA_DEMOTE_DEFAULT_BW_SAT_THRESHOLD,
    .prefer_closer_node = 1
};

/* 节点压力缓存（避免频繁读取sysfs） */
static double g_node_pressure_cache[MAX_NUMA_NODES];
static long long g_pressure_cache_time[MAX_NUMA_NODES];
#define PRESSURE_CACHE_TTL_MS 1000  /* 缓存有效期1秒 */

/* ========== 配置接口 ========== */

void evictionDemoteConfigDefaults(numa_demote_config_t *config) {
    if (!config) return;
    config->enabled = 1;
    config->min_demote_size = NUMA_DEMOTE_DEFAULT_MIN_SIZE;
    config->max_migrate_count = NUMA_DEMOTE_DEFAULT_MAX_MIGRATE;
    config->node_pressure_threshold = NUMA_DEMOTE_DEFAULT_PRESSURE_THRES;
    config->distance_weight = NUMA_DEMOTE_DEFAULT_DISTANCE_WEIGHT;
    config->pressure_weight = NUMA_DEMOTE_DEFAULT_PRESSURE_WEIGHT;
    config->bandwidth_weight = NUMA_DEMOTE_DEFAULT_BANDWIDTH_WEIGHT;
    config->bw_saturation_threshold = NUMA_DEMOTE_DEFAULT_BW_SAT_THRESHOLD;
    config->prefer_closer_node = 1;
}

void evictionSetDemoteConfig(const numa_demote_config_t *config) {
    if (!config) return;
    g_demote_config = *config;
}

void evictionGetDemoteConfig(numa_demote_config_t *config) {
    if (!config) return;
    *config = g_demote_config;
}

/* ========== 节点信息查询 ========== */

/*
 * numaGetNodePressure - 获取节点内存压力
 *
 * 从 /sys/devices/system/node/nodeX/meminfo 读取
 * 返回值: 0.0 ~ 1.0, 越大表示压力越高
 */
double numaGetNodePressure(int node_id) {
    int max_node = numa_max_node();
    if (node_id < 0 || node_id > max_node) {
        return 1.0; /* 无效节点返回满压力 */
    }
    
    /* 检查缓存 */
    long long now = server.mstime;
    if (g_pressure_cache_time[node_id] > 0 &&
        (now - g_pressure_cache_time[node_id]) < PRESSURE_CACHE_TTL_MS) {
        return g_node_pressure_cache[node_id];
    }
    
    /* 读取 sysfs */
    char path[128];
    snprintf(path, sizeof(path),
             "/sys/devices/system/node/node%d/meminfo", node_id);
    
    FILE *fp = fopen(path, "r");
    if (!fp) {
        g_node_pressure_cache[node_id] = 1.0;
        g_pressure_cache_time[node_id] = now;
        return 1.0;
    }
    
    unsigned long mem_total = 0, mem_free = 0;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "MemTotal")) {
            /* 格式: Node X MemTotal: XXXX kB */
            char *colon = strchr(line, ':');
            if (colon) {
                mem_total = strtoul(colon + 1, NULL, 10);
            }
        } else if (strstr(line, "MemFree")) {
            char *colon = strchr(line, ':');
            if (colon) {
                mem_free = strtoul(colon + 1, NULL, 10);
            }
        }
    }
    fclose(fp);
    
    double pressure = 1.0;
    if (mem_total > 0) {
        pressure = 1.0 - ((double)mem_free / (double)mem_total);
    }
    
    /* 更新缓存 */
    g_node_pressure_cache[node_id] = pressure;
    g_pressure_cache_time[node_id] = now;
    
    return pressure;
}

/*
 * numaGetNodeFreeMemory - 获取节点空闲内存（KB）
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
    
    return (size_t)mem_free * 1024; /* 转换为字节 */
}

/* ========== 节点选择算法 ========== */

/*
 * numaFindBestDemoteNode - 找到最佳降级目标节点
 *
 * 选择策略: 距离优先 + 压力感知 + 带宽感知
 * 使用加权评分综合距离、压力和带宽因素
 */
int numaFindBestDemoteNode(size_t object_size, int current_node) {
    int num_nodes = numa_pool_num_nodes();
    if (num_nodes <= 1) return -1; /* 单节点无需降级 */
    
    /* 候选节点结构 */
    typedef struct {
        int node_id;
        int distance;      /* NUMA 距离 (越小越近) */
        double pressure;   /* 内存压力 (0~1) */
        size_t free_mem;   /* 空闲内存 */
        double bw_usage;   /* 带宽利用率 (0~1) */
        double score;      /* 综合评分 (越小越好) */
    } node_candidate_t;
    
    node_candidate_t candidates[MAX_NUMA_NODES];
    int candidate_count = 0;
    
    /* 收集所有候选节点信息 */
    for (int i = 0; i < num_nodes; i++) {
        if (i == current_node) continue; /* 跳过当前节点 */
        
        double pressure = numaGetNodePressure(i);
        size_t free_mem = numaGetNodeFreeMemory(i);
        
        /* 检查压力是否超限 */
        double threshold = server.numa_demote_pressure_threshold / 100.0;
        if (pressure >= threshold) {
            serverLog(LL_DEBUG,
                "[NUMA Demote] Node %d skipped: pressure %.2f >= threshold %.2f",
                i, pressure, threshold);
            continue;
        }

        /* 获取带宽利用率 */
        double bw_usage = numa_bw_get_usage(i);
        if (bw_usage >= g_demote_config.bw_saturation_threshold) {
            serverLog(LL_DEBUG,
                "[NUMA Demote] Node %d skipped: bw_usage %.2f >= threshold %.2f",
                i, bw_usage, g_demote_config.bw_saturation_threshold);
            continue;
        }
        
        /* 检查是否有足够空间 */
        if (free_mem < object_size * 2) {
            serverLog(LL_DEBUG,
                "[NUMA Demote] Node %d skipped: free_mem %zu < required %zu",
                i, free_mem, object_size * 2);
            continue;
        }
        
        /* 获取 NUMA 距离 */
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
    
    /* === 评分计算 === */
    /*
     * 综合评分 = 距离归一化 * distance_weight + 压力归一化 * pressure_weight + 带宽归一化 * bandwidth_weight
     * 评分越低越优先选择
     */
    
    /* 找最大距离、最大压力和最大带宽用于归一化 */
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
    
    /* 避免除零 */
    if (max_distance == 0) max_distance = 1;
    if (max_pressure < 0.01) max_pressure = 1.0;
    if (max_bw_usage < 0.01) max_bw_usage = 1.0;
    
    /* 计算每个候选节点的综合评分 */
    for (int i = 0; i < candidate_count; i++) {
        double dist_norm = (double)candidates[i].distance / (double)max_distance;
        double pres_norm = candidates[i].pressure / max_pressure;
        double bw_norm = candidates[i].bw_usage / max_bw_usage;
            
        /* 从 g_demote_config 读取权重配置 */
        int dist_weight = g_demote_config.distance_weight;
        int pres_weight = g_demote_config.pressure_weight;
        int bw_weight = g_demote_config.bandwidth_weight;
            
        if (g_demote_config.prefer_closer_node) {
            /*
             * 策略 A: 加权模式 - 使用配置的三因子权重
             * 适合延迟敏感场景
             */
            candidates[i].score =
                dist_norm * dist_weight / 100.0 +
                pres_norm * pres_weight / 100.0 +
                bw_norm   * bw_weight   / 100.0;
        } else {
            /*
             * 策略 B: 平衡模式 - 距离、压力、带宽同等重要
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
    
    /* 选择评分最低的节点 */
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

/* ========== 降级执行接口 ========== */

/*
 * evictionTryNumaDemote - 尝试将对象降级到其他 NUMA 节点
 *
 * @db: 数据库指针 (redisDb*)
 * @key: 键名 (sds)
 * @val: 值对象 (robj*)
 * @target_node: 输出参数，返回目标节点ID
 *
 * 返回值: numa_demote_result_t
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
    
    /* 获取对象大小 */
    size_t obj_size = objectComputeSize(val_obj, 0);
    if (obj_size < server.numa_demote_min_size) {
        return NUMA_DEMOTE_SKIP; /* 太小不值得迁移 */
    }
    
    /* 获取当前 NUMA 节点 */
    int current_node = -1;
    if (val_obj->ptr) {
        current_node = numa_get_node_id(val_obj->ptr);
    }
    if (current_node < 0) {
        current_node = numa_pool_get_node();
    }
    
    /* 找最佳目标节点 */
    int best_node = numaFindBestDemoteNode(obj_size, current_node);
    if (best_node < 0) {
        *target_node = -1;
        return NUMA_DEMOTE_NO_NODE;
    }
    
    /* 执行迁移 */
    robj keyobj;
    initStaticStringObject(keyobj, key);
    
    int result = numa_migrate_single_key(rdb, &keyobj, best_node);
    
    if (result == NUMA_KEY_MIGRATE_OK) {
        *target_node = best_node;
        server.stat_numa_demotions++;
        server.stat_numa_demote_bytes += obj_size;
        
        /* 统计距离分布 */
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
 * numaGetNodeBandwidthUsage - 获取节点带宽利用率
 *
 * 返回值: 0.0 ~ 1.0, -1.0 表示无效节点
 */
double numaGetNodeBandwidthUsage(int node_id) {
    return numa_bw_get_usage(node_id);
}

#else /* !HAVE_NUMA */

/* 非 NUMA 环境的空实现 */

void evictionDemoteConfigDefaults(numa_demote_config_t *config) {
    if (config) memset(config, 0, sizeof(*config));
}

void evictionSetDemoteConfig(const numa_demote_config_t *config) {
    (void)config;
}

void evictionGetDemoteConfig(numa_demote_config_t *config) {
    if (config) memset(config, 0, sizeof(*config));
}

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
