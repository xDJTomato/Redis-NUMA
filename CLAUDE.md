# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Redis 6.2.21 extended with NUMA-aware memory allocation and CXL (Compute Express Link) memory tiering. The project adds transparent NUMA node-granular allocation, per-key heat tracking, and cross-node cold/hot data migration while preserving full Redis API compatibility.

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
./check_numa_config.sh
./diagnose_numa.sh
```

Test structure:
- `tests/unit/*.tcl` — standard Redis unit tests
- `tests/ycsb/` — YCSB performance benchmarks (primary test framework)
- `tests/ycsb/workloads/` — workload definitions (baseline, stress, bw_saturate, numa_migration)
- `tests/legacy/numa/` — archived NUMA functional tests (C/bash)
- `tests/ycsb/scripts/` — helper scripts (install, eval, report generation)

## Architecture

### NUMA Module Layer (added on top of Redis core)

Nine modules in `src/`, all guarded by `#ifdef HAVE_NUMA`:

1. **numa_pool** — Custom memory allocator. 16 size classes (16B–4KB), bump-pointer O(1) allocation, slab allocator for ≤128B objects, chunk compaction for <30% utilization chunks.
2. **numa_migrate** — Low-level block migration between NUMA nodes via `numa_alloc_onnode` + memcpy.
3. **numa_key_migrate** — Per-key migration (robj as unit). LRU-integrated heat tracking with lazy step decay. Type adapters for STRING (implemented), HASH/LIST/SET/ZSET (stubs).
4. **numa_strategy_slots** — 16-slot pluggable strategy framework with vtable-based polymorphism. Slot 0 = no-op, Slot 1 = Composite LRU. Runs via `serverCron` every second.
5. **numa_composite_lru** — Default migration strategy (Slot 1). Dual-channel: hot candidate ring buffer (fast path) + progressive dictionary scan (slow path). JSON-configurable.
6. **numa_configurable_strategy** — 6 allocation strategies (LOCAL_FIRST, INTERLEAVE, ROUND_ROBIN, WEIGHTED, PRESSURE_AWARE, CXL_OPTIMIZED) at the zmalloc layer.
7. **numa_command** — Unified `NUMA` Redis command: `NUMA MIGRATE`, `NUMA CONFIG`, `NUMA STRATEGY`.
8. **numa_bw_monitor** — Real-time per-node bandwidth monitoring (resctrl/numastat/manual backends).
9. **evict_numa** — NUMA-aware eviction: demotes keys to less-pressured nodes before eviction. Weighted scoring: distance(40%) + pressure(30%) + bandwidth(30%).

### Key Integration Points in Redis Core

- **zmalloc.c/h** — All `zmalloc/zfree/zrealloc` routed through NUMA allocator when available. 16-byte `numa_alloc_prefix_t` prefix on every allocation tracks size, node, hotness, access metadata. `NO_MALLOC_USABLE_SIZE` is forced.
- **server.h** — NUMA stats counters and config fields in `redisServer` struct. NUMA headers included under `#ifdef HAVE_NUMA`.
- **server.c** — `numa_init()` in `main()` before `initServer()`. Strategy/key-migration/bw-monitor init after `initServer()`. Periodic compaction and strategy execution in `serverCron`.
- **evict.h** — Extended `evictionPoolEntry` with `current_node`, `object_size`, `numa_migrated` fields.

### Module Dependency Order (bottom to top)

libnuma → numa_pool → numa_migrate → numa_key_migrate → numa_composite_lru / numa_strategy_slots → numa_command → evict_numa → server.c

## Configuration

- `redis.conf` lines 1044–1071: `numa-demote-*` settings (enable, min-size, max-migrate, pressure-threshold, weights)
- `redis.conf` lines 2085–2097: `numa-migrate-config` path to `composite_lru.json`
- `composite_lru.json`: per-node bandwidth baselines and migration tuning parameters

## Documentation

- `docs/new/` — Current module design docs (01-overview through 10-call-chain). **Prefer these over `docs/modules/`** which contains older versions.
- `TEST_README.md` — Test organization and recommended workflows
- `WORKFLOW.md` — Development workflow conventions (coding order, integration checklist, common pitfalls)

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
- **Only string key migration is fully implemented** — hash/list/set/zset adapters are stubs
