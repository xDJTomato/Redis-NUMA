/* =============================================================================
 * numa_shim.h - portable NUMA emulation environment (pure C11).
 *
 * On a real Linux machine the Redis-NUMA project links against libnuma. This
 * project targets machines WITHOUT heterogeneous memory and WITHOUT libnuma
 * (e.g. a 16 GB Windows laptop). To keep every strategy buildable, runnable
 * and fairly evaluable on such a machine we provide a drop-in emulation of
 * the libnuma allocation/query API on top of plain malloc:
 *
 *   - "NUMA nodes" are modeled as a configurable topology (capacity, latency,
 *     bandwidth, distance, pressure) instead of physical sockets.
 *   - Allocation on a node is a normal heap allocation recorded in a small
 *     registry so that numa_node_of_addr() still works.
 *   - Access cost is computed analytically from the topology, which is what
 *     the evaluation harness uses to rank strategies fairly: every strategy
 *     sees the exact same topology, workload and seed.
 *
 * The exported nf_numa_* names mirror libnuma's numa_* subset used by
 * Redis-NUMA (numa_available, numa_max_node, numa_alloc_onnode, ...). A Linux
 * build with real libnuma simply keeps using <numa.h>; this file is the
 * portable stand-in used by NUMAflow and the evaluation harness.
 * ========================================================================== */
#ifndef NUMA_SHIM_H
#define NUMA_SHIM_H

#include "nf_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nf_alloc {
    void   *ptr;
    size_t  size;
    int     node;
    struct nf_alloc *next;
} nf_alloc_t;

typedef struct {
    int         node_count;
    nf_node_t   nodes[NF_MAX_NODES];
    uint64_t    local_cpu_node;
    nf_alloc_t *allocs;
    size_t      alloc_count;
    double      bandwidth_penalty;
} numaflow_env_t;

void nf_numa_env_init(numaflow_env_t *env);
void nf_numa_env_destroy(numaflow_env_t *env);
int  nf_numa_configure_default(numaflow_env_t *env, int node_count);
int  nf_numa_add_node(numaflow_env_t *env, const nf_node_t *node);

int    nf_numa_available(void);
int    nf_numa_max_node(const numaflow_env_t *env);
int    nf_numa_num_nodes(const numaflow_env_t *env);
int    nf_numa_distance(const numaflow_env_t *env, int a, int b);
int    nf_numa_node_of_cpu(const numaflow_env_t *env, int cpu);

void  *nf_numa_alloc_onnode(numaflow_env_t *env, size_t size, int node);
void  *nf_numa_alloc_interleaved(numaflow_env_t *env, size_t size);
void   nf_numa_free(numaflow_env_t *env, void *ptr);
int    nf_numa_node_of_addr(const numaflow_env_t *env, const void *ptr);
size_t nf_numa_size_of_addr(const numaflow_env_t *env, const void *ptr);

double nf_numa_access_cost(const numaflow_env_t *env, int from_node, int node, size_t bytes);
double nf_numa_migrate_cost(const numaflow_env_t *env, int from, int to, size_t bytes);
void   nf_numa_account_alloc(numaflow_env_t *env, int node, size_t bytes);
void   nf_numa_account_free(numaflow_env_t *env, int node, size_t bytes);
double nf_numa_pressure(const numaflow_env_t *env, int node);
void   nf_numa_describe(const numaflow_env_t *env, char *buf, size_t buflen);

#ifdef __cplusplus
}
#endif

#endif /* NUMA_SHIM_H */
