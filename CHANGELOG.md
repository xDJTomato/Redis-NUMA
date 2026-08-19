# Changelog

[中文版](CHANGELOG.zh-CN.md)

All notable changes to this fork are documented here, in the style of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased] — NUMA migration-strategy consolidation (ADR-08)

### Changed (breaking)

- **All three migration strategies (`caat`/`composite_lru`/`tinylfu`) now
  live exclusively in the NUMAflow atomic-op engine**
  (`numaflow/src/nf_strategy.c`) — the native kernel implementations
  (`src/numa_strategy_slots.{c,h}`, `src/numa_composite_lru.{c,h}`,
  `src/numa_tinylfu.{c,h}`) have been deleted. See ADR-08 in
  [`docs/new/09-architecture-decisions.md`](docs/new/09-architecture-decisions.md).
- `NUMA STRATEGY` command removed entirely (slot insert/enable/disable/
  schedule/status/list). Use `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT`
  and the new `NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>`.
- `numa-migrate-config` / `NUMA CONFIG LOAD` (composite-lru JSON hot-reload)
  removed. Replaced by `numa-flow-default-strategy` (default `caat`) and
  `numa-flow-interval-sec`, auto-loaded at startup as the NUMAflow `default`
  workflow entry — migration now runs out of the box without an explicit
  `NUMA FLOW LOAD`. `composite_lru.json` stays in the repo as a field-name
  reference only; the kernel no longer reads it.
- `NUMA CONFIG GET`/`NUMA MIGRATE STATS` responses no longer include the
  `access_tracking_enabled`/`locality_stats_enabled`/`debug_logging_enabled`/
  `composite_*`/`accesses_local`/`accesses_remote`/`tinylfu_*` fields (their
  source no longer exists). `NUMA MIGRATE SCAN` now runs the NUMAflow
  `default` entry once instead of a composite-lru scan pass; `COUNT` is
  accepted for CLI compatibility but ignored.
- Default migration behavior changed from Composite LRU to CAAT
  (Cost-Aware Adaptive Tiering) — ADR-04's own benchmark showed CAAT's net
  cost is ~37% lower.

### Fixed

- **TinyLFU and CAAT never actually migrated anything through the Redis
  integration** (`src/numa_flow.c`), even before this consolidation —
  `cms_estimate` (the Count-Min Sketch frequency read) always returned 0
  because nothing in the bridge ever called `nf_tracker_observe()` (the CMS
  write side); only numaflow's own standalone benchmark harness
  (`numaflow/src/nf_bench.c`) did that, which is why `ADR-04`'s benchmark
  numbers were real but the live bridge path was silently inert. Fixed by
  feeding the tracker from the same real access path as
  `numa_key_migrate_touch()`: `numa_flow_observe_access()`
  (`src/numa_flow.c`/`.h`), called from `src/db.c` on every real key access.
- **CAAT additionally lost every demoted item that didn't also qualify for
  promotion** (`numaflow/src/nf_strategy.c`'s `build_caat`, and the
  equivalent `NF_ADAPT_AGGRESSIVE` template in `numaflow/src/nf_adapt.c`).
  `nf_exec_run()`'s result is the union of graph *sink* node outputs only;
  the old single linear chain fed demote's `emit_migrate` straight into the
  promote phase's `filter_freq`/`filter_benefit`, so any item that got
  demoted but didn't survive those filters was dropped from the graph
  before reaching any sink — its demotion had already executed
  (`current_node` mutated, `ctx.stats.migrations_done` incremented) but the
  bridge's enumerate-vs-result diff never saw it, so the host's `apply()`
  callback (what performs a *real* migration) was never called. Fixed by
  forking the graph *before* either phase mutates anything, splitting on
  each item's original residency: DRAM residents only ever go through
  demote (decide, then a terminal `emit_migrate`), off-DRAM residents only
  ever go through promote (filters narrow first, mutate last) — every item
  is mutated at most once and always reaches exactly one sink. Verified
  with a standalone harness exercising the real bridge/engine code
  end-to-end: CAAT now correctly demotes cold DRAM residents *and* promotes
  hot CXL residents in the same run.
- Per-key hotness tracking (`numa_get_hotness`/`numa_get_access_count` on
  the zmalloc allocation prefix) was previously only updated when the
  now-removed Composite LRU/TinyLFU slots were enabled. It's now updated
  unconditionally via `numa_key_migrate_touch()`
  (`src/numa_key_migrate.c`/`src/db.c`), so NUMAflow's `enumerate()` keeps
  a real signal regardless of which (if any) migration strategy is active.
- `numa_configurable_strategy`'s `PRESSURE_AWARE` allocation strategy read
  `numa_config_get_node_utilization()` — an unbounded GB-of-bytes counter,
  not a 0.0-1.0 ratio — against a min-seed of `1.0`, silently breaking node
  ranking once any node passed 1GB allocated. It now reads the same
  canonical `numa_bw_get_node_pressure()` signal `evict_numa` uses.
- `evict_numa.c` and `numa_configurable_strategy.c` each maintained an
  independent node-pressure computation with different formulas and cache
  TTLs. Consolidated into a single getter,
  `numa_bw_get_node_pressure()` (`src/numa_bw_monitor.c`).
- `WEIGHTED_INTERLEAVE` was a byte-for-byte duplicate of `WEIGHTED`'s
  weighted-random node-selection loop, differing only in which weight array
  it read. Both cases now call one shared `select_weighted_node()` helper.

## [Unreleased] — Redis 7 port on `feat/redis7-port`

### Changed

- **Redis core migrated from 6.2.21 to 7.2.6** via a real three-way merge
  (using a synthetic graft to give the fork's fresh-history root commit a
  common ancestor with upstream). See
  [`docs/redis7-migration.md`](docs/redis7-migration.md) for the full
  record: tooling, every bug found and fixed, and how verification was
  done.
- All 10 NUMA modules ported to Redis 7's dict API (opaque `dictEntry`,
  `dictCreate()` without `privdata`, `dict *d`-first callback signatures),
  listpack (alongside legacy ziplist), and quicklist's `->entry` field
  rename.
- `src/commands/numa.json` added to register `NUMA` with Redis 7's
  declarative command-introspection system.
- Documentation rewritten to match the real post-merge state: `README.md`,
  new `ARCHITECTURE.md`, new `TESTING.md`, `CONTRIBUTING.md`, this
  changelog, and `docs/redis7-migration.md` (replacing
  `docs/redis8-migration.md`).

### Added

- `run_full_validation.sh` — single entry point running build, unit tests,
  the NUMAflow benchmark, and (optionally) the YCSB, QEMU, and CXLMemSim
  steps below, producing one aggregated HTML report.
- `tests/vm/boot_numa_vm.sh` — QEMU (TCG) multi-NUMA-node smoke test:
  boots a 2-node emulated guest and runs `redis-server` + the `NUMA`
  command family + `redis-benchmark` inside it.
- `tests/cxl/run_cxlmemsim.sh` — validates the CXLMemSim
  (`SlugLab/CXLMemSim`, `external/CXLMemSim`) device-emulation link: its own
  CTest suite, plus a QEMU CXL Type2 endpoint confirmed connected to
  `cxlmemsim_server` over TCP.
- `tests/cxl/cxlmemsim_workload_bench.cpp` — replays the same
  zipf/uniform/hotspot/temporal workload shapes NUMAflow's harness uses
  directly through CXLMemSim's own `CXLMemExpander` C++ model, giving a
  real-device-model comparison point alongside NUMAflow's simplified cost
  model. See `ARCHITECTURE.md` for two real bugs found in CXLMemSim's own
  cache-invalidation ordering while wiring this up (worked around, not
  patched, since `external/CXLMemSim` isn't vendored into this repo).
- `numaflow eval --cxl-latency-ns`/`--cxl-bandwidth-mbps` — calibrate
  NUMAflow's own cost model against real numbers captured from a
  CXLMemSim device-link run instead of `numa_shim.c`'s synthetic tier-1
  defaults; `run_full_validation.sh` now runs every workload both ways
  (`results/bench_<workload>.json` and
  `results/bench_<workload>_cxlcal.json`).
- `tests/report/generate_full_report.py` — aggregates step results and
  NUMAflow `bench_*.json` data into a self-contained HTML report with
  inline SVG charts (extends the approach in `numaflow/eval/report.py`);
  now also charts the CXLMemSim-calibrated NUMAflow runs and the native
  CXLMemSim model results alongside the synthetic-default comparison.

### Fixed

Six bugs the `git merge` recursive strategy introduced silently (zero
conflict markers) during the core merge, one pre-existing allocator-guard
bug the new test suite happened to expose, and one upstream CVE that
postdated the `7.2.6` tag. Full details, file-by-file, in
[`docs/redis7-migration.md`](docs/redis7-migration.md):

- `src/zmalloc.c` — missing `#endif` after an `HAVE_NUMA` block.
- `src/dict.c` — stray duplicate overflow-check referencing a
  since-removed variable.
- `src/server.c` — duplicate `afterCommand()` definition; separately, a
  duplicate `replicationFeedMonitors()` call in `call()` that fed MONITOR
  clients every command twice.
- `src/networking.c` — duplicate `addReplyBigNum()` and
  `deferredAfterErrorReply()` definitions.
- `src/server.h`/`src/evict.c`/`src/evict_numa.c` — stale 2-arg
  `objectComputeSize()` prototype vs. the real 4-arg signature, causing a
  SIGSEGV in the eviction-demote path.
- `src/zmalloc.c` — `numa_alloc_dram()` (backing
  `zmalloc_local`/`zcalloc_local`/`ztrycalloc_local`) never checked
  `numa_ctx.numa_available`, corrupting the heap in any binary other than
  `redis-server` that builds a dict (surfaced as a `redis-cli` SIGSEGV
  under `tests/unit/cluster/cli.tcl`).
- `src/hyperloglog.c` — cherry-picked the CVE-2025-32023 fix
  (`f35b72dd1`), which lands after the `7.2.6` tag.
- `src/config.c` — `numa-demote-min-size` registered with
  `createIntConfig()` against a `size_t` field; switched to
  `createSizeTConfig()`.

### Removed

- `src/redis8_compat.h` — dead code; nothing in the tree included it, and
  the actual port went directly to Redis 7's native APIs.
- `docs/redis8-migration.md` — superseded by `docs/redis7-migration.md`.

## Earlier history

Everything before the Redis 7 port (the NUMA module implementations
themselves, NUMAflow's introduction, the 6.2.21 Linux/QEMU-verified
baseline) predates this changelog. See `git log` for the detailed commit
history — `9ff536438` (`checkpoint(redis-6.2): Linux/QEMU verified NUMA 6.2
baseline`) is a convenient marker for "last known-good state before the
Redis 7 port began".
