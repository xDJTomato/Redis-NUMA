/* =============================================================================
 * nf_bench.h - fair, QEMU-free memory-scheduling strategy evaluator (pure C11).
 *
 * Compares migration strategies and allocation strategies on an emulated NUMA
 * topology using identical workloads, seeds, budgets and access traces so the
 * comparison is reproducible and fair.  Returns a JSON result tree for the
 * visualizer.
 * ========================================================================== */
#ifndef NF_BENCH_H
#define NF_BENCH_H

#include <stdint.h>
#include "nf_json.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *workload;   /* zipf | uniform | hotspot | temporal */
    size_t      keys;
    size_t      accesses;
    size_t      epoch;      /* accesses per migration-decision window */
    int         budget;     /* migration budget per decision */
    int         nodes;      /* emulated NUMA node count */
    uint64_t    seed;
} nf_bench_config_t;

/* Run the full comparison and return an owned JSON result object. */
nf_json_t *nf_bench_run(const nf_bench_config_t *cfg);

/* Fill defaults for any zeroed field in cfg. */
void nf_bench_config_defaults(nf_bench_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* NF_BENCH_H */
