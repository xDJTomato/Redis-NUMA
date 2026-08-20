# Architecture

[中文版](ARCHITECTURE.zh-CN.md)

## NUMA module layer (added on top of Redis core)

Eight modules in `src/`, all guarded by `#ifdef HAVE_NUMA` (the 16-slot vtable
strategy framework and its native Composite LRU / TinyLFU implementations
have been retired — see ADR-08 in `docs/new/09-architecture-decisions.md`):

1. **numa_pool** (`numa_pool.c/h`) — custom memory allocator. 33 size
   classes (8B-64KB), bitmap-managed two-tier slab allocation (small/large)
   with a thread-local cache (tcache) for a lock-free fast path.
2. **numa_migrate** (`numa_migrate.c/h`) — low-level block migration between
   NUMA nodes via `numa_zmalloc_onnode` + `memcpy` (note: actual per-key
   migration is implemented independently in numa_key_migrate below, which
   does not call this module's migration function).
3. **numa_key_migrate** (`numa_key_migrate.c/h`) — per-key migration (a
   `robj` as the unit). `numa_key_migrate_touch()` unconditionally updates
   the neutral zmalloc-prefix hotness signal on every real access - the
   single ground truth NUMAflow's bridge (`numa_flow.c`) reads via
   `enumerate()`. Full type adapters for all 5 Redis types: STRING
   (RAW/EMBSTR), HASH (listpack/ziplist/hashtable), LIST (quicklist,
   LZF/raw and `QUICKLIST_NODE_CONTAINER_PLAIN`/`PACKED` sub-paths), SET
   (intset/hashtable), ZSET (listpack/ziplist/skiplist).
4. **numa_configurable_strategy** (`numa_configurable_strategy.c/h`) — 7
   independent allocation-node-selection behaviors at the `zmalloc` layer
   (LOCAL_FIRST, INTERLEAVE, ROUND_ROBIN, one shared weighted-random
   implementation for WEIGHTED/WEIGHTED_INTERLEAVE differing only in weight
   source, PRESSURE_AWARE, CXL_OPTIMIZED). ADAPTIVE/LATENCY_AWARE are
   kernel-side placeholders (behave as LOCAL_FIRST; self-report via a
   startup log and `NUMA CONFIG GET`'s `strategy_note` field) - their real
   implementation is the matching `alloc_adaptive`/`alloc_latency_aware`
   atomic op in NUMAflow.
5. **numa_command** (`numa_command.c/h`) — the unified `NUMA` command:
   `NUMA MIGRATE`, `NUMA CONFIG`, `NUMA FLOW`, registered via
   `src/commands/numa.json` (Redis 7's declarative command-introspection
   system).
6. **numa_bw_monitor** (`numa_bw_monitor.c/h`) — real-time per-node
   bandwidth monitoring (resctrl/numastat/manual backends) and the single
   canonical node-pressure getter (`numa_bw_get_node_pressure()`), shared by
   `evict_numa` and `numa_configurable_strategy` so the two never disagree
   about how loaded a node is.
7. **evict_numa** (`evict_numa.c`, interface declared in `evict.h` — there
   is no separate `evict_numa.h`) — NUMA-aware eviction: demotes keys
   to less-pressured nodes before evicting them. Weighted scoring:
   distance (40%) + pressure (30%) + bandwidth (30%); pressure comes from
   `numa_bw_monitor`.
8. **numa_flow.c** (`HAVE_NUMA` only) — the Redis-side bridge to the
   NUMAflow atomic-op engine. Migration strategy (`caat`/`composite_lru`/
   `tinylfu`/`noop`) is implemented *only* here, as NUMAflow DAG presets
   (`numaflow/src/nf_strategy.c`) — there is no native kernel
   implementation of any of them. Implements the two bridge callbacks
   `nf_bridge.c` expects, auto-loads `numa-flow-default-strategy` (default
   `caat`) as the `default` workflow entry at startup unless
   `numa-enabled no`, and exposes `NUMA FLOW
   LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT/DEFAULT`. `serverCron` runs loaded
   workflows on their configured interval.

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
  Periodic strategy execution happens in `serverCron`.
- **`evict.h`/`evict.c`** — a stateless demotion attempt
  (`evictionTryNumaDemote`) is inserted into the eviction loop before a key
  is actually evicted; `evictionPoolEntry` itself is unmodified.

## Module dependency order (bottom to top)

```
libnuma
  -> numa_pool
    -> numa_migrate
      -> numa_key_migrate
        -> numa_bw_monitor
          -> numa_configurable_strategy
            -> numa_flow (NUMAflow bridge)
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
./build/numaflow eval --workload zipf --cxl-latency-ns 125 --cxl-bandwidth-mbps 25000
./build/numaflow replay --trace caat=trace_caat.json --trace noop=trace_noop.json
python gui/server.py          # N8N-style DAG editor at http://127.0.0.1:8090
```

Layout:

- `include/` + `src/` — the engine (`nf_ops.c` for the 36 atomic ops,
  `nf_strategy.c` for the strategy catalog including CAAT, `nf_bench.c` for
  the fair evaluator over a synthetic access trace, `nf_cli.c` for CLI
  dispatch (`ops/strategies/templates/template/workflow/run/dump-ops/
  dump-templates/eval/replay`), `nf_track.c` for the CMS + Doorkeeper + EWMA
  feedback loop, `nf_bridge.c` for the store-agnostic bridge contract and
  migration application, `nf_adapt.c` for the self-adapting DAG (parameter
  hill-climb + structure selection), `numa_shim.c` for portable libnuma
  emulation — also the home of the pure-function cost model
  `nf_numa_access_cost`/`nf_numa_migrate_cost` that both `eval` and `replay`
  feed into). `replay` feeds a *real* placement trace (not a synthetic one)
  through that same calibratable cost model, producing output
  shape-compatible with `eval`'s `bench_<workload>.json`.
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
- **`tests/vm/placement_quality.sh`** / **`tests/vm/relative_perf_bench.sh`**
  (need `boot_numa_vm.sh --keep` to leave a guest running) — the smoke test
  above doesn't run any migration strategy long enough to say anything
  about placement; these do, against the same real (if latency-flat)
  ≥2-node topology. `placement_quality.sh` measures hot-key-stays-local /
  cold-key-moves-off ratios per strategy inside the guest (found and fixed
  two previously zero-coverage bugs in the real migration execution path
  the first time it ran — SDS key lookup, tick/recency truncation; see
  ADR-11). `relative_perf_bench.sh` orchestrates all four strategies,
  collects a real per-key placement trace from each, and feeds it through
  NUMAflow's calibrated cost model (`numaflow replay`) for a *modeled*
  relative ns-level projection — not a measured latency, since QEMU's two
  `-numa node`s share the same host DRAM (see ADR-12).
- **`external/CXLMemSim`** (`SlugLab/CXLMemSim`, not vendored into this
  repo's history) — a device-level CXL memory-timing simulator with its own
  patched QEMU. `tests/cxl/run_cxlmemsim.sh` validates the QEMU&harr;
  `cxlmemsim_server` link (a CXL Type2 endpoint connecting to the server
  over TCP and exchanging a simulated topology). A guest-boot attempt was
  made (booting the stock Debian 12 cloud image, which already ships
  `cxl_pci`/`cxl_acpi`/`cxl_mem` kernel modules, under CXLMemSim's patched
  QEMU with a `cxl-type2` endpoint attached): the guest correctly enumerates
  the device on the PCI bus (`0d:00.0 CXL [0502]: Intel Corporation Device
  [8086:0d92]`), but the stock kernel's `cxl_pci` driver fails to bind to it
  (`echo ... > bind` returns `I/O error`, no dmesg output) — CXLMemSim's own
  `qemu_integration/launch_qemu_vcs_dcd_gfam.sh` confirms this is expected:
  its default `KERNEL_IMAGE` is a custom-patched
  `/root/linux-cxl-type2/arch/x86/boot/bzImage`, i.e. exposing this Type2
  device's memory as guest-visible RAM/NUMA capacity needs a kernel built
  from CXLMemSim's own patched Linux tree (not present in this environment),
  not just a stock distro kernel. So `redis-server` was never able to
  actually touch CXL-emulated memory here; building that kernel was judged
  out of scope for this pass. For a real Redis-level DRAM-vs-far-memory
  comparison, see `tests/ycsb/scripts/eval_cxl_memory.sh`, which uses
  `numactl --membind` across 2 real NUMA nodes (works inside the VM above;
  this fork's own development host has only 1 physical NUMA node).

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
