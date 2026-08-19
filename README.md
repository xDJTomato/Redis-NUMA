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

Eight modules layered on top of Redis core, all guarded by `#ifdef HAVE_NUMA`,
plus the NUMAflow atomic-op engine (`numaflow/`) which now owns all
migration-strategy logic (see [`ARCHITECTURE.md`](ARCHITECTURE.md) for the
full breakdown):

- **numa_pool** — custom allocator: 33 size classes, bitmap-managed
  two-tier slab allocation (small/large) with a thread-local cache
  (tcache) for a lock-free fast path.
- **numa_migrate** / **numa_key_migrate** — block- and key-granular
  cross-node migration, with full type adapters for STRING/HASH/LIST/SET/ZSET.
- **numa_configurable_strategy** — 7 independent allocation strategies at the
  `zmalloc` layer (LOCAL_FIRST, INTERLEAVE, ROUND_ROBIN, WEIGHTED/
  WEIGHTED_INTERLEAVE share one weighted-random implementation,
  PRESSURE_AWARE, CXL_OPTIMIZED). ADAPTIVE/LATENCY_AWARE are kernel-side
  placeholders whose real implementation lives in NUMAflow.
- **numa_command** — the unified `NUMA` command (`MIGRATE`/`CONFIG`/`FLOW`).
- **numa_bw_monitor** — real-time per-node bandwidth monitoring, plus the
  canonical node-pressure getter shared with `evict_numa`.
- **evict_numa** — NUMA-aware eviction that demotes keys before evicting them.
- **numa_flow.c** — the Redis-side bridge to the NUMAflow atomic-op engine.
  Migration strategy (`caat`/`composite_lru`/`tinylfu`/`noop`) is implemented
  *only* here, as NUMAflow DAG presets — there is no native kernel
  implementation. Auto-loads `numa-flow-default-strategy` (default `caat`)
  at startup; switch it at runtime with `NUMA FLOW DEFAULT <name>` or load
  custom workflows with `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT`.

The old 16-slot vtable strategy framework (`numa_strategy_slots`) and its
native Composite LRU / TinyLFU implementations have been retired — see ADR-08
in `docs/new/09-architecture-decisions.md`.

## Configuration

- `redis.conf` lines 1184-1208: `numa-demote-*` settings.
- `redis.conf` numa migration section: `numa-enabled`,
  `numa-flow-default-strategy` (default `caat`; also accepts
  `composite_lru`/`tinylfu`/`noop`), `numa-flow-interval-sec`.
  `composite_lru.json` is kept only as a field-name reference for custom
  NUMAflow workflow JSON — the kernel no longer reads it.

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
