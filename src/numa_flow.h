/* =============================================================================
 * numa_flow.h - Redis adapter that runs NUMAflow DAG workflows in-process.
 *
 * This is the thin bridge between the pure-C11 NUMAflow engine (numaflow/) and
 * the Redis NUMA runtime: it implements the two bridge callbacks (enumerate the
 * keyspace into nf_item_t, apply a migration via numa_key_migrate) and exposes
 * the `NUMA FLOW` sub-command so a workflow exported by the GUI can be loaded
 * and executed on a schedule, with optional self-adaptation of parameters and
 * structure.
 * ========================================================================== */
#ifndef NUMA_FLOW_H
#define NUMA_FLOW_H

#include "server.h"

#ifdef HAVE_NUMA

/* Initialise the bridge (env/topology/tracker + empty workflow list). */
int  numa_flow_init(void);
/* Release all loaded workflows and the bridge state. */
void numa_flow_cleanup(void);

/* Auto-load the default migration strategy (one of the NUMAflow presets in
 * numaflow/src/nf_strategy.c: caat|composite_lru|tinylfu|noop) as the
 * "default" workflow entry, running every interval_sec seconds via
 * numa_flow_cron(). Called once from main() after numa_flow_init(), only
 * when numa-enabled is on. */
int  numa_flow_load_default(const char *strategy_name, int interval_sec);

/* Rebuild the "default" entry's graph from a named preset at runtime
 * (backs `NUMA FLOW DEFAULT <name>`). Creates the entry if it doesn't exist
 * yet (e.g. numa-enabled was "no" at startup). */
int  numa_flow_set_default(const char *strategy_name);

/* Run the "default" entry once on demand (backs `NUMA MIGRATE SCAN`, kept
 * for interface continuity with the retired composite_lru scan command). */
int  numa_flow_run_default(uint64_t *scanned, uint64_t *migrated);

/* Feed a real key access into the shared NUMAflow tracker (CMS + Doorkeeper
 * frequency estimator + EWMA feedback), the same way numaflow's own fair
 * benchmark harness (nf_bench.c) does on every trace event. This is the
 * *only* place anything calls nf_tracker_observe() in the Redis integration
 * - without it, cms_estimate always reads freq_est=0 for every key (the
 * tracker is otherwise never written to), which silently breaks the
 * frequency-gated stages of the tinylfu and caat presets (op_filter_freq,
 * op_demote_cold). Call this unconditionally from the same access path as
 * numa_key_migrate_touch(); a no-op before numa_flow_init() runs. */
void numa_flow_observe_access(const char *key);

/* Handle the `NUMA FLOW <sub> ...` command. */
void numa_flow_command(client *c);

/* serverCron hook: run due workflows and fold feedback into the adapters. */
void numa_flow_cron(void);

#endif /* HAVE_NUMA */
#endif /* NUMA_FLOW_H */
