This README covers the NUMA extensions added to Redis 6.2.21. For the
original Redis documentation, see [REDIS_ORIGINAL_README.md](docs/devlog/original/REDIS_ORIGINAL_README.md).

What is this?
-------------

This is a modified version of Redis 6.2.21 with transparent NUMA-aware
memory allocation and CXL (Compute Express Link) memory tiering support.
All standard Redis commands and APIs work unchanged. The extensions run
behind the scenes: every `zmalloc` call automatically picks the best NUMA
node, a 16-byte metadata prefix tracks per-object heat, and a background
migration engine moves hot data to local DRAM and cold data to CXL memory.

The project targets dual-socket servers and CXL-attached memory expanders
where Node 0 is low-latency DRAM and Node 1 is high-capacity CXL memory.

Building
--------

You need `libnuma-dev` (Debian/Ubuntu) or `numactl-devel` (CentOS/RHEL):

    % cd src
    % make clean && make -j$(nproc)

The build forces `MALLOC=libc` and links `-lnuma`. jemalloc is not
compatible with the NUMA allocator and must not be used.

Running
-------

Start the server as usual:

    % ./src/redis-server redis.conf

To enable NUMA migration, add these lines to `redis.conf`:

    numa-enabled yes
    numa-migrate-config /path/to/composite_lru.json

Verify that NUMA is active:

    % ./src/redis-cli NUMA CONFIG GET

An example `composite_lru.json`:

    {
        "migrate_hotness_threshold": 3,
        "hot_candidates_size": 1024,
        "scan_batch_size": 500,
        "decay_threshold_sec": 10,
        "auto_migrate_enabled": 1,
        "debug_logging_enabled": 0,
        "overload_threshold": 0.8,
        "bandwidth_threshold": 0.9,
        "pressure_threshold": 0.7,
        "stability_count": 3,
        "max_bandwidth_node0_mbps": 51000,
        "max_bandwidth_node1_mbps": 12000
    }

How it works
------------

Every allocation goes through a two-tier path:

* Objects up to 4 KB are served by a **Slab allocator**: 64 KB slabs,
  24 jemalloc-style size classes (16 B to 4096 B), 3072-bit bitmaps,
  atomic CAS — completely lock-free.

* Objects larger than 4 KB fall through to `numa_alloc_onnode()`, a
  direct system call.

Before returning the pointer, `zmalloc` prepends a 16-byte prefix
(`numa_alloc_prefix_t`) that records the allocation size, source slab
flag, NUMA node ID, hotness level (0–7), access count, and last-access
timestamp. The caller never sees this prefix; it sits behind the
returned pointer and is recovered by `zfree` via a simple offset.

The node to allocate on is chosen by a configurable strategy. The
default is **weighted interleave**: every second, `serverCron` reads
each node's memory pressure from `/sys/devices/system/node/nodeN/meminfo`,
converts it to a weight (`max(1, (1 − pressure) × 100)`), and publishes
it via `atomicSet`. The allocation path reads these weights with
`atomicGet` and does a weighted random selection — no locks on the hot
path.

Ten strategies are available:

    local_first         Always node 0
    interleaved         Random (per-thread seed)
    round_robin         Thread-local counter
    weighted            Static weights, short lock
    pressure_aware      Lowest utilization
    cxl_optimized       Small objects local, large objects remote
    weighted_interleave Pressure-aware weighted random (default, lock-free)
    adaptive            Not yet implemented (falls back to node 0)
    latency_aware       Not yet implemented (falls back to node 0)

Heat tracking and migration
---------------------------

Every time `lookupKey` finds a key, it calls
`composite_lru_record_access()`. This function:

1. Reads the hotness from the object's prefix.
2. Applies a stepped lazy decay based on idle time:
   less than 10 s → no decay; less than 60 s → −1; less than 5 min → −2;
   less than 30 min → −3; 30 min or more → reset to zero.
3. Increments hotness by one (capped at 7).
4. Writes the new hotness, access count, and timestamp back to the prefix.
5. Syncs the key into a dictionary (`key_heat_map`) so the scan channel
   can iterate over it.
6. If the hotness just crossed the migration threshold *and* the key
   lives on a remote node, inserts an SDS copy of the key name into a
   ring-buffer candidate pool.

Migration runs every second from `serverCron` through two channels:

* **Fast channel** — drains the candidate pool. For each entry it
  re-reads the current hotness from the prefix, checks the target node's
  memory pressure and bandwidth, and if conditions are met calls
  `numa_migrate_key_by_name()`.

* **Scan channel** — progressively iterates `key_heat_map` in batches.
  Hot keys on remote nodes are pulled back to local DRAM. Under memory
  pressure, cold keys on the local node are pushed out to the remote
  node (CXL).

The actual migration allocates new memory on the target node via
`numa_zmalloc_onnode`, copies the data with `memcpy`, atomically swaps
`val->ptr`, and frees the old allocation. Only the STRING type adapter
is fully implemented; HASH, LIST, SET, and ZSET adapters are stubs.

NUMA commands
-------------

All operations are exposed through a single `NUMA` command with three
sub-domains:

    NUMA MIGRATE KEY <key> <node>     Migrate one key
    NUMA MIGRATE DB <node>            Migrate entire database
    NUMA MIGRATE SCAN [COUNT <n>]     Trigger incremental scan
    NUMA MIGRATE STATS                Show migration statistics
    NUMA MIGRATE RESET                Reset statistics
    NUMA MIGRATE INFO <key>           Show key's NUMA metadata

    NUMA CONFIG GET                   Show current configuration
    NUMA CONFIG SET <param> <value>   Set a parameter
    NUMA CONFIG LOAD [path]           Load JSON config file
    NUMA CONFIG STATS                 Show per-node allocation stats
    NUMA CONFIG REBALANCE             Trigger manual rebalance

    NUMA STRATEGY LIST                List all 16 strategy slots
    NUMA STRATEGY SLOT <id> <name>    Insert strategy into a slot

    NUMA HELP                         Print command reference

The strategy slots framework supports up to 16 pluggable strategies,
dispatched by priority (HIGH → NORMAL → LOW). Slot 0 is a no-op
placeholder (LOW priority), slot 1 is the Composite LRU migration engine
(HIGH priority), and slots 2–15 are available for custom strategies.

Source layout
-------------

The NUMA modules live in `src/`, all guarded by `#ifdef HAVE_NUMA`:

    numa_pool.c/h                  Slab allocator
    numa_migrate.c/h               Block-level migration
    numa_key_migrate.c/h           Key-level migration, type adapters
    numa_strategy_slots.c/h        Strategy slot framework
    numa_composite_lru.c/h         Composite LRU (default strategy)
    numa_configurable_strategy.c/h Allocation strategy selection
    numa_command.c                 Unified NUMA command
    numa_bw_monitor.c/h            Per-node bandwidth monitoring
    evict_numa.c/h                 NUMA-aware eviction

Integration points in the Redis core:

* `zmalloc.c/h` — all allocations route through the NUMA allocator when
  available; the 16-byte prefix is prepended here.
* `server.c` — `numa_init()` runs before `initServer()`; strategy and
  migration modules initialize after; `serverCron` drives periodic
  pressure-weight updates and strategy execution every second.
* `db.c` — `lookupKey()` calls `composite_lru_record_access()`.

Testing
-------

Standard Redis tests:

    % cd src && make test

NUMA benchmarks (YCSB-based, three phases: fill → hotspot → sustain):

    % cd tests/ycsb && ./run_bw_benchmark.sh

Environment check:

    % ./check_numa_config.sh
    % ./diagnose_numa.sh

Performance
-----------

Measured on a QEMU dual-node VM (Node 0 = 4 GB DRAM, Node 1 = 8 GB CXL):

* Fill phase throughput: ~53 K ops/s (weighted interleave)
* Sustained migration throughput: ~45 K ops/s
* Migration rate: ~1 524 keys/s, zero overload stalls
* Memory fragmentation ratio: 1.04–1.17

The allocation hot path is entirely lock-free: node selection uses
`atomicGet`, slab allocation uses atomic CAS on bitmaps, and statistics
counters use `atomicIncr`.

Documentation
-------------

Detailed design documents are in `docs/new/`:

    00-design-proposal.md          Project proposal
    01-overview.md                 Architecture overview
    02-numa-pool.md                Slab allocator internals
    03-zmalloc-numa.md             zmalloc integration, PREFIX layout
    04-numa-migrate.md             Block-level migration
    05-numa-strategy-slots.md      Strategy slot framework
    06-numa-composite-lru.md       Composite LRU dual-channel design
    07-numa-key-migrate.md         Key-level migration, type adapters
    08-numa-configurable.md        Allocation strategy framework
    09-numa-command.md             Command reference
    10-call-chain.md               Full call-chain trace
    11-alloc-path-instrumentation.md  RSS investigation
    12-perf-root-cause-analysis.md    Throughput root-cause analysis
    13-lockfree-alloc-design.md       Lock-free allocation design

Project status
--------------

Implemented:

* Slab allocator (24 size classes, atomic CAS, lock-free)
* Weighted-interleave default allocation strategy (lock-free)
* Composite LRU dual-channel migration
* 16-byte PREFIX inline metadata
* Strategy slot framework (16 slots, priority dispatch)
* STRING type migration adapter
* Unified NUMA command interface
* JSON hot-reload configuration
* Bandwidth monitoring
* Lock-free allocation path
* NUMA-aware eviction

Not yet implemented:

* HASH, LIST, SET, ZSET type migration adapters
* Adaptive allocation strategy
* Latency-aware allocation strategy
* ML-based migration prediction

License
-------

BSD 3-Clause. See [COPYING](COPYING). Built on Redis 6.2.21.
