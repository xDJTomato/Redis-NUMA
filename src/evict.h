/* evict.h - Memory eviction policy and NUMA interaction interface.
 *
 * This header defines the eviction pool entry and the NUMA demotion
 * interface, allowing cold data to be migrated to another NUMA node
 * instead of being evicted when memory is over the limit.
 *
 * Copyright (c) 2009-2016, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2024, Redis-CXL Project
 * All rights reserved.
 */

#ifndef __EVICT_H__
#define __EVICT_H__

#include <stdint.h>
#include <stddef.h>

/* Eviction pool size. */
#define EVPOOL_SIZE 16
#define EVPOOL_CACHED_SDS_SIZE 255

/* Eviction pool entry (kept in sync with the evict.c usage). */
struct evictionPoolEntry {
    unsigned long long idle;    /* Object idle time (inverse frequency for LFU) */
    char *key;                  /* Key name (SDS string). */
    char *cached;               /* Cached SDS object for key name. */
    int dbid;                   /* Key DB number. */
};

/* NUMA demotion result codes. */
typedef enum {
    NUMA_DEMOTE_OK = 0,         /* Demotion succeeded. */
    NUMA_DEMOTE_NO_NODE,        /* No usable target node. */
    NUMA_DEMOTE_FAILED,         /* Migration failed. */
    NUMA_DEMOTE_SKIP            /* Skipped (object too small / unsupported / disabled). */
} numa_demote_result_t;

/* Maximum number of NUMA nodes. */
#define MAX_NUMA_NODES 16

/* ========== Public interface ========== */

/*
 * evictionTryNumaDemote - Try to demote an object to another NUMA node.
 *
 * @db: database pointer
 * @key: key name (SDS string)
 * @val: value object
 * @target_node: output parameter, receives the target node ID
 *
 * Returns: numa_demote_result_t
 */
numa_demote_result_t evictionTryNumaDemote(void *db, char *key, void *val, int *target_node);

/*
 * numaFindBestDemoteNode - Find the best demotion target node.
 *
 * Selection policy: distance-first with pressure awareness.
 * Uses a weighted score combining distance and pressure factors.
 *
 * @object_size: object size
 * @current_node: current node
 *
 * Returns: best node ID, -1 if no usable node exists
 */
int numaFindBestDemoteNode(size_t object_size, int current_node);

/*
 * numaGetNodePressure - Get the memory pressure of a node.
 *
 * Returns: 0.0 ~ 1.0, higher means more pressure
 */
double numaGetNodePressure(int node_id);

/*
 * numaGetNodeFreeMemory - Get the free memory of a node.
 *
 * Returns: free memory in bytes
 */
size_t numaGetNodeFreeMemory(int node_id);

/*
 * numaGetNodeBandwidthUsage - Get the bandwidth utilization of a node.
 *
 * Returns: 0.0 ~ 1.0, -1.0 for an invalid node
 */
double numaGetNodeBandwidthUsage(int node_id);

#endif /* __EVICT_H__ */
