# CLAUDE.md

[中文版（人工阅读参考，本文件仍为唯一权威版本）](CLAUDE.zh-CN.md)

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Redis 7.2.6 extended with NUMA-aware memory allocation and CXL (Compute Express Link) memory tiering. The project adds transparent NUMA node-granular allocation, per-key heat tracking, and cross-node cold/hot data migration while preserving full Redis API compatibility.

A separate **pure-C11** subsystem **NUMAflow** (`numaflow/`) decomposes every NUMA scheduling strategy into **36 composable atomic operations** executable as N8N-style DAG workflows, and adds: a new default strategy (CAAT), a QEMU-free fair evaluation harness, a TUI and a web GUI, and a lightweight cache-behavior tracking feedback loop. The Redis 6.2.21 → 7.2.6 core migration was performed as a real three-way merge (not a paper plan) and is documented in `docs/redis7-migration.md`, including every bug the merge silently introduced and how it was found. The core builds and passes the full `make test` suite on Linux + libnuma.

## Build Commands

```bash
cd src
make clean && make -j$(nproc)
```

The build **forces `MALLOC=libc`** and links `-lnuma` on Linux (lines 133-140 of `src/Makefile`). jemalloc is incompatible with the NUMA allocator. `libnuma-dev` (Debian/Ubuntu) or `numactl-devel` (CentOS/RHEL) is required.

Build all targets: `make` in `src/` produces redis-server, redis-cli, redis-benchmark, redis-sentinel, redis-check-rdb, redis-check-aof.

## Running Tests

```bash
# Single entry point: build + make test + NUMAflow benchmark (+ optional YCSB/QEMU/CXLMemSim)
./run_full_validation.sh --quick   # fast: skips YCSB/QEMU/CXLMemSim
./run_full_validation.sh           # full: aggregated HTML report in results/full_report_<ts>/

# Standard Redis test suite (Tcl-based)
cd src && make test

# NUMA-specific functional tests
cd tests/ycsb && ./run_bw_benchmark.sh    # main benchmark (3-phase: Fill→Hotspot→Sustain)
cd tests/ycsb && ./run_ycsb.sh            # YCSB baseline/stress modes

# Quick NUMA environment check
./utils/numa/check_numa_config.sh
./utils/numa/diagnose_numa.sh

# NUMAflow subsystem tests (pure C11, runs on this Windows host too)
cd numaflow && make test

# Optional: QEMU multi-NUMA-node smoke test and CXLMemSim device-link check
./tests/vm/boot_numa_vm.sh
./tests/cxl/run_cxlmemsim.sh
```

Test structure:
- `tests/unit/*.tcl` — standard Redis unit tests
- `tests/ycsb/` — YCSB performance benchmarks (primary test framework)
- `tests/ycsb/workloads/` — workload definitions (baseline, stress, bw_saturate, numa_migration)
- `tests/legacy/numa/` — archived NUMA functional tests (C/bash)
- `tests/ycsb/scripts/` — helper scripts (install, eval, report generation)
- `tests/vm/` — QEMU multi-NUMA-node smoke test (TCG, gracefully skips without `/dev/kvm`)
- `tests/cxl/` — CXLMemSim device-emulation link validation (`external/CXLMemSim`)
- `tests/report/` — HTML report generator used by `run_full_validation.sh`

See `TESTING.md` for the full breakdown of every tier.

## Architecture

### NUMA Module Layer (added on top of Redis core)

Eight modules in `src/`, all guarded by `#ifdef HAVE_NUMA`, plus the NUMAflow atomic-op engine (`numaflow/`) which now owns all migration-strategy logic (see ADR-08 in `docs/new/09-architecture-decisions.md`):

1. **numa_pool** — Custom memory allocator. 33 size classes (8B–64KB), bump-pointer O(1) allocation, 64KB slab allocator for ≤4KB objects, chunk compaction for <30% utilization chunks.
2. **numa_migrate** — Low-level block migration between NUMA nodes via `numa_alloc_onnode` + memcpy.
3. **numa_key_migrate** — Per-key migration (robj as unit). `numa_key_migrate_touch()` unconditionally updates the neutral zmalloc-prefix hotness signal on every real access; this is the single ground truth NUMAflow's `enumerate()` reads. Full type adapters for all 5 Redis types: STRING (RAW/EMBSTR), HASH (listpack/ziplist/hashtable), LIST (quicklist with LZF/raw and PLAIN/PACKED container sub-paths), SET (intset/hashtable), ZSET (listpack/ziplist/skiplist).
4. **numa_configurable_strategy** — 7 independent allocation-node-selection behaviors at the zmalloc layer (LOCAL_FIRST, INTERLEAVE, ROUND_ROBIN, WEIGHTED/WEIGHTED_INTERLEAVE share one weighted-random implementation with different weight sources, PRESSURE_AWARE, CXL_OPTIMIZED). ADAPTIVE/LATENCY_AWARE are kernel-side placeholders (behave as LOCAL_FIRST, self-report via a startup log and `NUMA CONFIG GET`'s `strategy_note` field) — their real implementation is the matching `alloc_adaptive`/`alloc_latency_aware` atomic op in NUMAflow.
5. **numa_command** — Unified `NUMA` Redis command: `NUMA MIGRATE`, `NUMA CONFIG`, `NUMA FLOW`.
6. **numa_bw_monitor** — Real-time per-node bandwidth monitoring (resctrl/numastat/manual backends) and the single canonical node-pressure getter (`numa_bw_get_node_pressure()`), shared by `evict_numa` and `numa_configurable_strategy`.
7. **evict_numa** — NUMA-aware eviction: demotes keys to less-pressured nodes before eviction. Weighted scoring: distance(40%) + pressure(30%) + bandwidth(30%); pressure comes from `numa_bw_monitor`.
8. **numa_flow** (`src/numa_flow.c`) — Redis-side bridge to the NUMAflow atomic-op engine. Migration strategy (`caat`/`composite_lru`/`tinylfu`/`noop`) is implemented *only* here, as NUMAflow DAG presets (`numaflow/src/nf_strategy.c`) — there is no native kernel implementation of any of them. Auto-loads `numa-flow-default-strategy` (default `caat`) as the `default` workflow entry at startup unless `numa-enabled no`; switch it at runtime with `NUMA FLOW DEFAULT <name>` or load custom workflows with `NUMA FLOW LOAD`.

The 16-slot vtable strategy framework (`numa_strategy_slots`) and its native Composite LRU / TinyLFU implementations (`numa_composite_lru`, `numa_tinylfu`) have been retired — see `docs/new/09-architecture-decisions.md` ADR-08 and the corresponding (now-archived) module docs under `docs/new/modules/`.

### Key Integration Points in Redis Core

- **zmalloc.c/h** — All `zmalloc/zfree/zrealloc` routed through NUMA allocator when available. 16-byte `numa_alloc_prefix_t` prefix on every allocation tracks size, node, hotness, access metadata. `NO_MALLOC_USABLE_SIZE` is forced.
- **server.h** — NUMA stats counters and config fields in `redisServer` struct. NUMA headers included under `#ifdef HAVE_NUMA`.
- **server.c** — `numa_init()` in `main()` before `initServer()`. Key-migration/bw-monitor/NUMAflow-bridge init after `initServer()`; NUMAflow's default strategy auto-loads there too. Periodic compaction and `numa_flow_cron()` run in `serverCron`.
- **evict.h** — Extended `evictionPoolEntry` with `current_node`, `object_size`, `numa_migrated` fields.

### Module Dependency Order (bottom to top)

libnuma → numa_pool → numa_migrate → numa_key_migrate → numa_bw_monitor → numa_configurable_strategy → numa_flow (NUMAflow bridge) → numa_command → evict_numa → server.c

### NUMAflow subsystem (`numaflow/`, pure C11, no Redis/libnuma dependency)

Builds and tests on any platform with a C11 compiler (`make` or `mingw32-make`):

```bash
cd numaflow && make && make test && make report
./build/numaflow ops          # list 36 atomic operations
./build/numaflow strategies   # list 13 built-in strategies (CAAT is default)
python gui/server.py          # N8N-style DAG editor at http://127.0.0.1:8090
```

Components: `include/` + `src/` (engine), `tui/nf_tui.c` (interactive TUI), `gui/` (web editor + Python bridge), `eval/report.py` (SVG/HTML visualization), `tests/` (unit + bridge/adapt + smoke). Key files: `nf_ops.c` (36 atomic ops), `nf_strategy.c` (strategy catalog incl. CAAT), `nf_bench.c` (fair evaluator), `nf_track.c` (CMS + Doorkeeper + EWMA feedback), `nf_bridge.c` (store-agnostic bridge contract + migration application), `nf_adapt.c` (self-adapting DAG: parameter hill-climb + structure selection), `numa_shim.c` (portable libnuma emulation).

The Redis adapter `src/numa_flow.c` (compiled only under `HAVE_NUMA`) implements the two bridge callbacks and exposes `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT`; `serverCron` runs loaded workflows on their interval. `nf_adapt_tune()` folds each run's DRAM-residency feedback and can switch the DAG between conservative/balanced/aggressive templates and hill-climb its parameters.

## Configuration

- `redis.conf` lines 1184–1208: `numa-demote-*` settings (enable, min-size, max-migrate, pressure-threshold, weights)
- `redis.conf` numa migration section: `numa-enabled`, `numa-flow-default-strategy` (default `caat`; also accepts `composite_lru`/`tinylfu`/`noop`), `numa-flow-interval-sec`
- `composite_lru.json`: retired as a kernel-consumed config file (see ADR-08); kept as a field-name reference for writing custom NUMAflow workflow JSON

## Documentation

- `README.md` — project overview, quick start, `run_full_validation.sh` usage
- `ARCHITECTURE.md` — module layout, dependency order, Redis-core integration points
- `docs/new/` — arc42-pattern architecture documentation: 12 top-level chapters (`01-introduction-and-goals.md` through `12-glossary.md`), a `modules/` detail sheet per component (including `numa_bw_monitor` and `evict_numa`, which previously had no dedicated doc), and an `appendix/` for supporting material (call-chain reference, related-work comparison)
- `docs/numaflow/` — NUMAflow subsystem design and usage
- `docs/redis7-migration.md` — Redis 6.2.21 → 7.2.6 real merge record: tooling, every bug found and fixed, verification performed
- `docs/test/` — Test organization, benchmark results, and usage guides (benchmark_results.txt, EXECUTIVE_SUMMARY.txt, DIAGNOSIS_USAGE.txt)
- `docs/devlog/` — Development log and design notes (e.g. zmalloc-goals.txt)
- `TESTING.md` — every test tier, including the optional QEMU/CXLMemSim steps
- `CONTRIBUTING.md` — conventions for adding a new NUMA module
- `CHANGELOG.md` — version history (Keep a Changelog format)

## Development Conventions

When adding a new NUMA module:
1. Create `.h` (interfaces/structs) then `.c` (implementation)
2. Add `numa_xxx.o` to `REDIS_SERVER_OBJ` in `src/Makefile`
3. Include header in `server.h` under `#ifdef HAVE_NUMA`
4. Call init function in `server.c` after `initServer()`
5. Use `extern void _serverLog(...)` — not `serverLog()` directly (Redis internal convention)
6. NUMA .o files must appear after `server.o` in the Makefile link order

## Key Gotchas

- **Never use jemalloc** — the build forces libc, but if you change Makefile flags, NUMA will break
- **Init order matters** — `initServer()` must complete before any `numa_*_init()` call
- **serverLog is not directly available** — use `extern void _serverLog(int level, const char *fmt, ...)` in NUMA modules
- **All 5 data type migration adapters are fully implemented** — STRING, HASH, LIST, SET, ZSET with proper encoding handling (listpack/ziplist/hashtable/quicklist/skiplist)
