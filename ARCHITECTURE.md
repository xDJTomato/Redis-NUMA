# Architecture

## NUMA module layer (added on top of Redis core)

Ten modules in `src/`, all guarded by `#ifdef HAVE_NUMA`:

1. **numa_pool** (`numa_pool.c/h`) — custom memory allocator. 33 size
   classes (8B-64KB), bump-pointer O(1) allocation, a 64KB slab allocator
   for objects &le;4KB, and chunk compaction for chunks under 30%
   utilization.
2. **numa_migrate** (`numa_migrate.c/h`) — low-level block migration between
   NUMA nodes via `numa_alloc_onnode` + `memcpy`.
3. **numa_key_migrate** (`numa_key_migrate.c/h`) — per-key migration (a
   `robj` as the unit). LRU-integrated heat tracking with lazy step decay.
   Full type adapters for all 5 Redis types: STRING (RAW/EMBSTR), HASH
   (listpack/ziplist/hashtable), LIST (quicklist, LZF/raw and
   `QUICKLIST_NODE_CONTAINER_PLAIN`/`PACKED` sub-paths), SET
   (intset/hashtable), ZSET (listpack/ziplist/skiplist).
4. **numa_strategy_slots** (`numa_strategy_slots.c/h`) — a 16-slot
   pluggable strategy framework with vtable-based polymorphism. Slot 0 =
   no-op, slot 1 = Composite LRU (default), slot 2 = TinyLFU (disabled by
   default). Runs via `serverCron` every second.
5. **numa_composite_lru** (`numa_composite_lru.c/h`) — the default
   migration strategy (slot 1). Dual-channel: a hot-candidate ring buffer
   (fast path) plus a progressive dictionary scan (slow path).
   JSON-configurable via `composite_lru.json`.
6. **numa_tinylfu** (`numa_tinylfu.c/h`) — frequency-driven migration
   strategy (slot 2, disabled by default). Count-Min Sketch (4x16384,
   4-bit) + Doorkeeper Bloom Filter. Fixed ~40KB memory, O(1) hot-data
   discovery. Enabled manually to avoid conflicting with Composite LRU.
7. **numa_configurable_strategy** (`numa_configurable_strategy.c/h`) — 9
   allocation strategies (LOCAL_FIRST, INTERLEAVE, ROUND_ROBIN, WEIGHTED,
   PRESSURE_AWARE, CXL_OPTIMIZED, WEIGHTED_INTERLEAVE, ADAPTIVE,
   LATENCY_AWARE) at the `zmalloc` layer.
8. **numa_command** (`numa_command.c/h`) — the unified `NUMA` command:
   `NUMA MIGRATE`, `NUMA CONFIG`, `NUMA STRATEGY`, registered via
   `src/commands/numa.json` (Redis 7's declarative command-introspection
   system).
9. **numa_bw_monitor** (`numa_bw_monitor.c/h`) — real-time per-node
   bandwidth monitoring (resctrl/numastat/manual backends).
10. **evict_numa** (`evict_numa.c/h`) — NUMA-aware eviction: demotes keys
    to less-pressured nodes before evicting them. Weighted scoring:
    distance (40%) + pressure (30%) + bandwidth (30%).

Plus the Redis&harr;NUMAflow bridge:

- **numa_flow.c** (`HAVE_NUMA` only) — implements the two bridge callbacks
  NUMAflow's `nf_bridge.c` expects, and exposes `NUMA FLOW
  LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT`. `serverCron` runs loaded workflows on
  their configured interval.

## Key integration points in Redis core

- **`zmalloc.c`/`zmalloc.h`** — every `zmalloc`/`zfree`/`zrealloc` routed
  through the NUMA allocator when available. Every allocation carries a
  16-byte `numa_alloc_prefix_t` prefix tracking size, node, hotness, and
  access metadata. `NO_MALLOC_USABLE_SIZE` is forced.
  `zmalloc_local()`/`zcalloc_local()`/`ztrycalloc_local()` (used by
  `dict.c`) guard on `numa_ctx.numa_available` the same way
  `zmalloc()`/`zcalloc()` already did, since not every Redis binary
  (`redis-cli`, `redis-benchmark`, `redis-check-rdb`/`aof`,
  `redis-sentinel`) calls `numa_init()`.
- **`server.h`** — NUMA stats counters and config fields on `redisServer`.
  NUMA headers included under `#ifdef HAVE_NUMA`.
- **`server.c`** — `numa_init()` runs in `main()` before `initServer()`.
  Strategy/key-migration/bandwidth-monitor init runs after `initServer()`.
  Periodic compaction and strategy execution happen in `serverCron`.
- **`evict.h`/`evict.c`** — `evictionPoolEntry` extended with
  `current_node`, `object_size`, `numa_migrated`.

## Module dependency order (bottom to top)

```
libnuma
  -> numa_pool
    -> numa_migrate
      -> numa_key_migrate
        -> numa_composite_lru / numa_tinylfu / numa_strategy_slots
          -> numa_command
            -> evict_numa
              -> server.c
```

When adding a new module, follow this order and see
[`CONTRIBUTING.md`](CONTRIBUTING.md) for the step-by-step checklist.

## The NUMAflow subsystem (`numaflow/`)

Pure C11, no Redis/libnuma dependency — builds and tests on any platform
with a C11 compiler:

```bash
cd numaflow && make && make test && make report
./build/numaflow ops          # list the 36 atomic operations
./build/numaflow strategies   # list the 13 built-in strategies (CAAT is default)
python gui/server.py          # N8N-style DAG editor at http://127.0.0.1:8090
```

Layout:

- `include/` + `src/` — the engine (`nf_ops.c` for the 36 atomic ops,
  `nf_strategy.c` for the strategy catalog including CAAT, `nf_bench.c` for
  the fair evaluator, `nf_track.c` for the CMS + Doorkeeper + EWMA feedback
  loop, `nf_bridge.c` for the store-agnostic bridge contract and migration
  application, `nf_adapt.c` for the self-adapting DAG (parameter hill-climb
  + structure selection), `numa_shim.c` for portable libnuma emulation).
- `tui/nf_tui.c` — the interactive TUI.
- `gui/` — the web-based DAG editor and Python bridge.
- `eval/report.py` — pure-stdlib SVG/HTML report generator for the fair
  evaluation harness's `bench_*.json` output.
- `tests/` — unit, bridge/adapt, and smoke tests.

## External validation layers

Two optional, best-effort validation layers live outside the module tree
proper and degrade gracefully (log + skip, never fabricate) when the
environment doesn't support them:

- **`tests/vm/boot_numa_vm.sh`** — boots a small cloud image under QEMU with
  2 emulated NUMA nodes (pure TCG software emulation if `/dev/kvm` isn't
  present) and runs `redis-server` + the `NUMA` command family +
  `redis-benchmark` inside it, to exercise the NUMA code paths against a
  real (if emulated) multi-node topology.
- **`external/CXLMemSim`** (`SlugLab/CXLMemSim`, not vendored into this
  repo's history) — a device-level CXL memory-timing simulator with its own
  patched QEMU. `tests/cxl/run_cxlmemsim.sh` validates the QEMU&harr;
  `cxlmemsim_server` link (a CXL Type2 endpoint connecting to the server
  over TCP and exchanging a simulated topology); it does not run
  `redis-server` inside a CXLMemSim-attached guest, since stacking that on
  top of an already-slow TCG host was judged out of scope for a single
  validation pass. For a real Redis-level DRAM-vs-far-memory comparison,
  see `tests/ycsb/scripts/eval_cxl_memory.sh`, which uses `numactl
  --membind` across 2 real NUMA nodes (works inside the VM above; this
  fork's own development host has only 1 physical NUMA node).

The same script also runs `tests/cxl/cxlmemsim_workload_bench.cpp`, which
replays NUMAflow's four workload shapes (zipf/uniform/hotspot/temporal)
directly through CXLMemSim's own `CXLMemExpander::calculate_latency`/
`calculate_bandwidth` C++ model (built against `libcxlmemsim.a`), instead
of through NUMAflow's simplified single-latency/single-bandwidth cost
model. Two things worth knowing if you touch this file:

- Driving `calculate_latency`/`calculate_bandwidth` on a raw trace without
  first calling `CXLMemExpander::insert()` for each access makes the model
  workload-shape-*insensitive* (verified empirically -- all four workloads
  returned bit-identical output on the first draft): `insert()` is what
  classifies a first-ever touch of an address as a store and a repeat
  touch as a load, and that load/store ratio is what
  `calculate_bandwidth()`'s congestion model actually reads. The bench
  drives `insert()` over the whole trace before calling either
  calculation, which is what makes skewed workloads (zipf/hotspot: many
  repeat touches of a small hot set) produce meaningfully different
  numbers from uniform.
- `calculate_latency()` calls `update_address_cache()` first, which sets
  the same `cache_valid` flag `is_address_local()` checks without ever
  populating the `address_ranges` vector that method actually reads --
  calling `calculate_latency()` before `calculate_bandwidth()` on a fresh
  endpoint silently zeroes both. The bench calls `calculate_bandwidth()`
  first as a workaround. This is a real bug in CXLMemSim's own cache
  invalidation, not a misuse on our side; it's worked around rather than
  patched since `external/CXLMemSim` isn't vendored into this repo.
- NUMAflow's own model can also be calibrated with real captured
  CXLMemSim numbers instead of `numa_shim.c`'s synthetic tier-1 defaults,
  via `numaflow eval --cxl-latency-ns <n> --cxl-bandwidth-mbps <n>` (see
  `run_full_validation.sh`'s NUMAflow step, which runs every workload both
  ways and writes `results/bench_<workload>_cxlcal.json` alongside the
  synthetic-default `results/bench_<workload>.json`).

See [`TESTING.md`](TESTING.md) for how these fit into `run_full_validation.sh`.
