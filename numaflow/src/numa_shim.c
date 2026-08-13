/* numa_shim.c - portable NUMA emulation environment implementation (pure C11). */
#include "numa_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <inttypes.h>

void nf_numa_env_init(numaflow_env_t *env) {
    memset(env, 0, sizeof(*env));
    env->local_cpu_node = 0;
    env->bandwidth_penalty = 1.0;
}

void nf_numa_env_destroy(numaflow_env_t *env) {
    nf_alloc_t *a = env->allocs;
    while (a) {
        nf_alloc_t *next = a->next;
        free(a->ptr);
        free(a);
        a = next;
    }
    env->allocs = NULL;
    env->alloc_count = 0;
    env->node_count = 0;
}

int nf_numa_add_node(numaflow_env_t *env, const nf_node_t *node) {
    if (!env || !node) return NF_EINVAL;
    if (env->node_count >= NF_MAX_NODES) return NF_ENOMEM;
    env->nodes[env->node_count] = *node;
    env->nodes[env->node_count].node_id = env->node_count;
    env->nodes[env->node_count].used_bytes = 0;
    env->nodes[env->node_count].pressure = 0.0;
    env->node_count++;
    return NF_OK;
}

int nf_numa_configure_default(numaflow_env_t *env, int node_count) {
    if (!env) return NF_EINVAL;
    if (node_count < 1) node_count = 1;
    if (node_count > NF_MAX_NODES) node_count = NF_MAX_NODES;

    env->node_count = 0;

    /* Tier template: DRAM first, then progressively slower/larger CXL tiers. */
    static const struct { const char *name; uint64_t gb; double lat; double bw; double dist; } tiers[8] = {
        { "dram0",  8,  60.0, 20000.0, 10.0 },
        { "cxl1",  64, 300.0,  8000.0, 50.0 },
        { "cxl2", 128, 450.0,  5000.0, 80.0 },
        { "cxl3", 256, 600.0,  3000.0, 110.0 },
        { "cxl4", 512, 800.0,  2000.0, 140.0 },
        { "cxl5", 1024,1000.0, 1500.0, 170.0 },
        { "cxl6", 2048,1250.0, 1000.0, 200.0 },
        { "cxl7", 4096,1500.0,  700.0, 230.0 },
    };

    for (int i = 0; i < node_count; i++) {
        nf_node_t n;
        memset(&n, 0, sizeof(n));
        n.node_id = i;
        snprintf(n.name, sizeof(n.name), "%s", tiers[i].name);
        n.total_bytes = tiers[i].gb * 1024ULL * 1024ULL * 1024ULL;
        n.latency_ns = tiers[i].lat;
        n.bandwidth_mbps = tiers[i].bw;
        n.distance = tiers[i].dist;
        n.weight = (i == 0) ? 100.0 : 40.0;   /* DRAM preferred by default */
        nf_numa_add_node(env, &n);
    }
    return NF_OK;
}

int nf_numa_available(void) {
    return 1; /* the shim is always "available" */
}

int nf_numa_max_node(const numaflow_env_t *env) {
    return env ? env->node_count - 1 : -1;
}

int nf_numa_num_nodes(const numaflow_env_t *env) {
    return env ? env->node_count : 0;
}

int nf_numa_distance(const numaflow_env_t *env, int a, int b) {
    if (!env || a < 0 || b < 0 || a >= env->node_count || b >= env->node_count) return 10;
    if (a == b) return 10;
    double da = env->nodes[a].distance;
    double db = env->nodes[b].distance;
    return (int)(10.0 + fabs(da - db));
}

int nf_numa_node_of_cpu(const numaflow_env_t *env, int cpu) {
    (void)cpu;
    return env ? (int)env->local_cpu_node : 0;
}

void *nf_numa_alloc_onnode(numaflow_env_t *env, size_t size, int node) {
    if (!env || size == 0) return NULL;
    if (node < 0) node = 0;
    if (node >= env->node_count) node = env->node_count - 1;
    void *ptr = malloc(size);
    if (!ptr) return NULL;
    nf_alloc_t *rec = (nf_alloc_t *)malloc(sizeof(nf_alloc_t));
    if (!rec) { free(ptr); return NULL; }
    rec->ptr = ptr;
    rec->size = size;
    rec->node = node;
    rec->next = env->allocs;
    env->allocs = rec;
    env->alloc_count++;
    nf_numa_account_alloc(env, node, size);
    return ptr;
}

void *nf_numa_alloc_interleaved(numaflow_env_t *env, size_t size) {
    static int round_robin = 0;
    int n = env ? env->node_count : 1;
    if (n < 1) n = 1;
    int node = round_robin % n;
    round_robin = (round_robin + 1) % n;
    return nf_numa_alloc_onnode(env, size, node);
}

void nf_numa_free(numaflow_env_t *env, void *ptr) {
    if (!env || !ptr) return;
    nf_alloc_t **pp = &env->allocs;
    while (*pp) {
        nf_alloc_t *rec = *pp;
        if (rec->ptr == ptr) {
            *pp = rec->next;
            nf_numa_account_free(env, rec->node, rec->size);
            free(rec->ptr);
            free(rec);
            env->alloc_count--;
            return;
        }
        pp = &rec->next;
    }
    free(ptr); /* not in registry; still free to avoid leak */
}

int nf_numa_node_of_addr(const numaflow_env_t *env, const void *ptr) {
    if (!env || !ptr) return -1;
    for (const nf_alloc_t *rec = env->allocs; rec; rec = rec->next) {
        if (rec->ptr == ptr) return rec->node;
    }
    return -1;
}

size_t nf_numa_size_of_addr(const numaflow_env_t *env, const void *ptr) {
    if (!env || !ptr) return 0;
    for (const nf_alloc_t *rec = env->allocs; rec; rec = rec->next) {
        if (rec->ptr == ptr) return rec->size;
    }
    return 0;
}

double nf_numa_access_cost(const numaflow_env_t *env, int from_node, int node, size_t bytes) {
    if (!env || env->node_count == 0) return 0.0;
    if (node < 0) node = 0;
    if (node >= env->node_count) node = env->node_count - 1;
    if (from_node < 0) from_node = 0;
    const nf_node_t *n = &env->nodes[node];
    double latency = n->latency_ns;
    if (from_node != node) {
        /* remote access: scale latency by NUMA distance ratio */
        double d = (double)nf_numa_distance(env, from_node, node);
        latency = n->latency_ns * (d / 10.0);
    }
    double transfer = (double)bytes * 1000.0 / (n->bandwidth_mbps > 0 ? n->bandwidth_mbps : 1.0);
    return latency + transfer;
}

double nf_numa_migrate_cost(const numaflow_env_t *env, int from, int to, size_t bytes) {
    if (!env || env->node_count == 0) return 0.0;
    if (from < 0) from = 0;
    if (to < 0) to = 0;
    if (from >= env->node_count) from = env->node_count - 1;
    if (to >= env->node_count) to = env->node_count - 1;
    if (from == to) return 0.0;
    double read_t  = (double)bytes * 1000.0 / (env->nodes[from].bandwidth_mbps > 0 ? env->nodes[from].bandwidth_mbps : 1.0);
    double write_t = (double)bytes * 1000.0 / (env->nodes[to].bandwidth_mbps > 0 ? env->nodes[to].bandwidth_mbps : 1.0);
    double fixed = 1000.0; /* per-migration overhead (page setup, metadata) */
    return fixed + read_t + write_t;
}

void nf_numa_account_alloc(numaflow_env_t *env, int node, size_t bytes) {
    if (!env || node < 0 || node >= env->node_count) return;
    env->nodes[node].used_bytes += bytes;
    env->nodes[node].pressure = nf_numa_pressure(env, node);
}

void nf_numa_account_free(numaflow_env_t *env, int node, size_t bytes) {
    if (!env || node < 0 || node >= env->node_count) return;
    if (bytes > env->nodes[node].used_bytes) env->nodes[node].used_bytes = 0;
    else env->nodes[node].used_bytes -= bytes;
    env->nodes[node].pressure = nf_numa_pressure(env, node);
}

double nf_numa_pressure(const numaflow_env_t *env, int node) {
    if (!env || node < 0 || node >= env->node_count) return 0.0;
    const nf_node_t *n = &env->nodes[node];
    if (n->total_bytes == 0) return 0.0;
    double p = (double)n->used_bytes / (double)n->total_bytes;
    return p > 1.0 ? 1.0 : (p < 0.0 ? 0.0 : p);
}

void nf_numa_describe(const numaflow_env_t *env, char *buf, size_t buflen) {
    if (!env || !buf) return;
    size_t off = 0;
    for (int i = 0; i < env->node_count && off < buflen; i++) {
        const nf_node_t *n = &env->nodes[i];
        int w = snprintf(buf + off, buflen - off,
            "[%s] %" PRIu64 "GB lat=%.0fns bw=%.0fMB/s dist=%.0f press=%.2f\n",
            n->name,
            (uint64_t)(n->total_bytes / (1024ULL * 1024ULL * 1024ULL)),
            n->latency_ns, n->bandwidth_mbps, n->distance, n->pressure);
        if (w < 0) break;
        off += (size_t)w;
    }
}
