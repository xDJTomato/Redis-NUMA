# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Redis 6.2.21 extended with NUMA-aware memory allocation and CXL (Compute Express Link) memory tiering. The project adds transparent NUMA node-granular allocation, per-key heat tracking, and cross-node cold/hot data migration while preserving full Redis API compatibility.

A separate **pure-C11** subsystem **NUMAflow** (`numaflow/`) decomposes every NUMA scheduling strategy into **36 composable atomic operations** executable as N8N-style DAG workflows, and adds: a new default strategy (CAAT), a QEMU-free fair evaluation harness, a TUI and a web GUI, and a lightweight cache-behavior tracking feedback loop. The Redis 6.2 → 8 migration is documented in `docs/redis8-migration.md` with a compat header `src/redis8_compat.h` (the Redis core itself still targets 6.2.21 until it is recompiled on a Linux + libnuma host).

## Build Commands

```bash
cd src
make clean && make -j$(nproc)
```

The build **forces `MALLOC=libc`** and links `-lnuma` on Linux (lines 103-110 of `src/Makefile`). jemalloc is incompatible with the NUMA allocator. `libnuma-dev` (Debian/Ubuntu) or `numactl-devel` (CentOS/RHEL) is required.

Build all targets: `make` in `src/` produces redis-server, redis-cli, redis-benchmark, redis-sentinel, redis-check-rdb, redis-check-aof.

## Running Tests

```bash
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
```

Test structure:
- `tests/unit/*.tcl` — standard Redis unit tests
- `tests/ycsb/` — YCSB performance benchmarks (primary test framework)
- `tests/ycsb/workloads/` — workload definitions (baseline, stress, bw_saturate, numa_migration)
- `tests/legacy/numa/` — archived NUMA functional tests (C/bash)
- `tests/ycsb/scripts/` — helper scripts (install, eval, report generation)

## Architecture

### NUMA Module Layer (added on top of Redis core)

Ten modules in `src/`, all guarded by `#ifdef HAVE_NUMA`:

1. **numa_pool** — Custom memory allocator. 33 size classes (8B–64KB), bump-pointer O(1) allocation, 64KB slab allocator for ≤4KB objects, chunk compaction for <30% utilization chunks.
2. **numa_migrate** — Low-level block migration between NUMA nodes via `numa_alloc_onnode` + memcpy.
3. **numa_key_migrate** — Per-key migration (robj as unit). LRU-integrated heat tracking with lazy step decay. Full type adapters for all 5 Redis types: STRING (RAW/EMBSTR), HASH (ziplist/hashtable), LIST (quicklist with LZF/raw sub-paths), SET (intset/hashtable), ZSET (ziplist/skiplist).
4. **numa_strategy_slots** — 16-slot pluggable strategy framework with vtable-based polymorphism. Slot 0 = no-op, Slot 1 = Composite LRU, Slot 2 = TinyLFU (disabled by default). Runs via `serverCron` every second.
5. **numa_composite_lru** — Default migration strategy (Slot 1). Dual-channel: hot candidate ring buffer (fast path) + progressive dictionary scan (slow path). JSON-configurable.
6. **numa_tinylfu** — Frequency-driven migration strategy (Slot 2, disabled by default). Count-Min Sketch (4×16384, 4-bit) + Doorkeeper Bloom Filter. Fixed ~40KB memory, O(1) hot data discovery. Must be manually enabled to avoid conflict with Composite LRU.
7. **numa_configurable_strategy** — 9 allocation strategies (LOCAL_FIRST, INTERLEAVE, ROUND_ROBIN, WEIGHTED, PRESSURE_AWARE, CXL_OPTIMIZED, WEIGHTED_INTERLEAVE, ADAPTIVE, LATENCY_AWARE) at the zmalloc layer.
8. **numa_command** — Unified `NUMA` Redis command: `NUMA MIGRATE`, `NUMA CONFIG`, `NUMA STRATEGY`.
9. **numa_bw_monitor** — Real-time per-node bandwidth monitoring (resctrl/numastat/manual backends).
10. **evict_numa** — NUMA-aware eviction: demotes keys to less-pressured nodes before eviction. Weighted scoring: distance(40%) + pressure(30%) + bandwidth(30%).

### Key Integration Points in Redis Core

- **zmalloc.c/h** — All `zmalloc/zfree/zrealloc` routed through NUMA allocator when available. 16-byte `numa_alloc_prefix_t` prefix on every allocation tracks size, node, hotness, access metadata. `NO_MALLOC_USABLE_SIZE` is forced.
- **server.h** — NUMA stats counters and config fields in `redisServer` struct. NUMA headers included under `#ifdef HAVE_NUMA`.
- **server.c** — `numa_init()` in `main()` before `initServer()`. Strategy/key-migration/bw-monitor init after `initServer()`. Periodic compaction and strategy execution in `serverCron`.
- **evict.h** — Extended `evictionPoolEntry` with `current_node`, `object_size`, `numa_migrated` fields.

### Module Dependency Order (bottom to top)

libnuma → numa_pool → numa_migrate → numa_key_migrate → numa_composite_lru / numa_tinylfu / numa_strategy_slots → numa_command → evict_numa → server.c

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

- `redis.conf` lines 1051–1071: `numa-demote-*` settings (enable, min-size, max-migrate, pressure-threshold, weights)
- `redis.conf` lines 2092–2104: `numa-enabled` and `numa-migrate-config` path to `composite_lru.json`
- `composite_lru.json`: per-node bandwidth baselines and migration tuning parameters

## Documentation

- `docs/new/` — Current module design docs (00-design-proposal through 19-ae-strategy-scheduler-technical-design)
- `docs/numaflow/` — NUMAflow subsystem design and usage
- `docs/redis8-migration.md` — Redis 6.2 → 8 migration guide + verification checklist
- `docs/test/` — Test organization, benchmark results, and usage guides (benchmark_results.txt, EXECUTIVE_SUMMARY.txt, DIAGNOSIS_USAGE.txt)
- `docs/devlog/` — Development log and design notes (e.g. zmalloc-goals.txt)

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
- **All 5 data type migration adapters are fully implemented** — STRING, HASH, LIST, SET, ZSET with proper encoding handling (ziplist/hashtable/quicklist/skiplist)
