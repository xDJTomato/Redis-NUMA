# Redis-NUMA

[中文版](README.zh-CN.md)

Redis 7.2.6 extended with NUMA-aware memory allocation and CXL (Compute
Express Link) memory tiering: transparent NUMA node-granular allocation,
per-key heat tracking, and cross-node cold/hot data migration, while
preserving full Redis API compatibility.

A separate pure-C11 subsystem, **NUMAflow** (`numaflow/`), decomposes every
NUMA scheduling strategy into 36 composable atomic operations executable as
N8N-style DAG workflows, and adds a new default strategy (CAAT), a QEMU-free
fair evaluation harness, a TUI, a web GUI, and a lightweight cache-behavior
feedback loop.

## Status

The Redis core was migrated from 6.2.21 to 7.2.6 via a real three-way merge
(not a paper plan) — see [`docs/redis7-migration.md`](docs/redis7-migration.md)
for exactly what that involved, what it broke, and how each break was found
and fixed. `make -j$(nproc)` builds clean and `make test` passes the full
Tcl suite. See [`ARCHITECTURE.md`](ARCHITECTURE.md) for the module layout and
[`TESTING.md`](TESTING.md) for how to run everything, including the optional
QEMU multi-NUMA-node and CXLMemSim validation steps.

## Quick start

```bash
# Build (Linux + libnuma required; forces MALLOC=libc, see Building below)
cd src && make clean && make -j$(nproc)

# Run the standard Redis config with NUMA enabled (see redis.conf:2342-2354)
./redis-server ../redis.conf

# Talk to it
./redis-cli set foo bar
./redis-cli numa config get
./redis-cli numa strategy list
```

### Full validation in one command

```bash
./run_full_validation.sh --quick     # build + unit tests + NUMAflow benchmark
./run_full_validation.sh             # ...plus QEMU VM smoke test + CXLMemSim
```

This produces `results/full_report_<timestamp>/index.html` — a single,
dependency-free HTML file with inline SVG charts summarizing every step. Any
step that cannot run in the current environment (no JDK for YCSB, no
`/dev/kvm` for QEMU, CXLMemSim not built) is marked **skipped** with the
reason; nothing is fabricated.

## Building

```bash
cd src
make clean && make -j$(nproc)
```

The build **forces `MALLOC=libc`** and links `-lnuma` on Linux
(`src/Makefile` lines 133-140) — jemalloc is incompatible with the NUMA
allocator. You need `libnuma-dev` (Debian/Ubuntu) or `numactl-devel`
(CentOS/RHEL).

`make` in `src/` produces `redis-server`, `redis-cli`, `redis-benchmark`,
`redis-sentinel`, `redis-check-rdb`, `redis-check-aof`.

## What's in this fork

Ten modules layered on top of Redis core, all guarded by `#ifdef HAVE_NUMA`
(see [`ARCHITECTURE.md`](ARCHITECTURE.md) for the full breakdown):

- **numa_pool** — custom allocator: 33 size classes, bitmap-managed
  two-tier slab allocation (small/large) with a thread-local cache
  (tcache) for a lock-free fast path.
- **numa_migrate** / **numa_key_migrate** — block- and key-granular
  cross-node migration, with full type adapters for STRING/HASH/LIST/SET/ZSET.
- **numa_strategy_slots** + **numa_composite_lru** + **numa_tinylfu** — a
  16-slot pluggable migration-strategy framework; Composite LRU is the
  default (slot 1), TinyLFU is available but disabled by default (slot 2).
- **numa_configurable_strategy** — 9 allocation strategies at the `zmalloc`
  layer (LOCAL_FIRST, INTERLEAVE, ROUND_ROBIN, WEIGHTED, PRESSURE_AWARE,
  CXL_OPTIMIZED, WEIGHTED_INTERLEAVE, ADAPTIVE, LATENCY_AWARE).
- **numa_command** — the unified `NUMA` command (`MIGRATE`/`CONFIG`/`STRATEGY`).
- **numa_bw_monitor** — real-time per-node bandwidth monitoring.
- **evict_numa** — NUMA-aware eviction that demotes keys before evicting them.
- **numa_flow.c** — bridges Redis to the NUMAflow subsystem's strategy
  catalog via `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT`.

## Configuration

- `redis.conf` lines 1184-1208: `numa-demote-*` settings.
- `redis.conf` lines 2342-2354: `numa-enabled` and `numa-migrate-config`
  (points at `composite_lru.json`).

## Documentation map

| Doc | Covers |
|---|---|
| [`docs/GUIDE.zh-CN.md`](docs/GUIDE.zh-CN.md) | Chinese-language student study guide — NUMA/CXL background, module-by-module walkthrough, the 6.2.21→7.2.6 migration as a case study, full test tiers, suggested reading order |
| [`ARCHITECTURE.md`](ARCHITECTURE.md) | Module layout, dependency order, integration points in Redis core |
| [`docs/redis7-migration.md`](docs/redis7-migration.md) | What the 6.2.21 → 7.2.6 merge actually did, bug-for-bug |
| [`TESTING.md`](TESTING.md) | How to run every test tier, including QEMU/CXLMemSim |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Conventions for adding a new NUMA module |
| [`CHANGELOG.md`](CHANGELOG.md) | Version history |
| `docs/new/` | arc42-pattern architecture documentation: 12 top-level chapters + a `modules/` detail sheet per component + an `appendix/` |
| `docs/numaflow/` | NUMAflow subsystem design and usage |
| `docs/README.md` | Full documentation index with a fact-checked status table |
