# Changelog

All notable changes to this fork are documented here, in the style of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

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
