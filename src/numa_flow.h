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

/* Handle the `NUMA FLOW <sub> ...` command. */
void numa_flow_command(client *c);

/* serverCron hook: run due workflows and fold feedback into the adapters. */
void numa_flow_cron(void);

#endif /* HAVE_NUMA */
#endif /* NUMA_FLOW_H */
