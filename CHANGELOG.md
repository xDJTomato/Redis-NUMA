# Changelog

[中文版](CHANGELOG.zh-CN.md)

All notable changes to this fork are documented here, in the style of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased] — tag a known GitHub-runner-hostile stock test as `slow`

CI's `build-and-test` and `sanitizer-address` jobs both reliably hung for
15+ minutes on the last test file, `tests/unit/maxmemory.tcl`, then failed
with an I/O error. Reproduced identically twice. Locally (and even
artificially pinned to 2 CPUs, matching the runner's core count) the same
test completes in single-digit seconds — so before touching anything,
verified whether this was actually caused by this fork's code: cloned and
built unmodified upstream `redis/redis` at the `7.2.6` tag (zero fork
changes) in a one-off diagnostic GitHub Actions workflow and ran just this
test file on the same `ubuntu-latest` runner class. It hung in the exact
same spot for the exact same reason (killed by a 15-minute `timeout`
wrapper). Confirms this is a pre-existing characteristic of
`test_slave_buffers`'s `cmd_count=1000000` case (1M pipelined `SETRANGE`s
against a `SIGSTOP`'d replica) on GitHub's shared runners, not a NUMA-fork
regression — no code in this fork was changed as a result.

### Changed

- `tests/unit/test_slave_buffers` (`tests/unit/maxmemory.tcl`): added a
  `slow` tag, so `./runtest --tags -slow` (already what CI passes) skips
  it. Both call sites (`slave buffer are counted correctly` and `replica
  buffer don't induce eviction`) share the same proc and are now skipped
  together.

## [Unreleased] — first green CI run: build-system and warning fixes

Running the rewritten `ci.yml` for the first time (see the entry below)
immediately failed, since nothing had ever actually built this fork with
CI's stricter settings before.

### Fixed

- **CI's `make ... REDIS_CFLAGS='-Werror'` silently broke the NUMAflow
  build.** `src/Makefile:124` does `REDIS_CFLAGS+=-I../numaflow/include` to
  make the NUMAflow headers visible to the Redis core build — but GNU Make
  does not let a makefile's `+=` modify a variable that was set on the
  `make` command line, so passing *any* `REDIS_CFLAGS=...` on the CLI (not
  just `-Werror`) silently dropped that include path and broke every file
  that includes an `nf_*.h` header. Not caught locally because the
  documented build command (`make -j$(nproc)`, no `REDIS_CFLAGS`) never
  triggers it. Fixed by dropping `REDIS_CFLAGS='-Werror'` from `ci.yml`'s
  `build-and-test` and `sanitizer-address` jobs entirely, matching the
  project's actual documented build command.
- `src/zmalloc.c`: `numa_tcache_free_miss` was declared as a counter
  alongside `numa_tcache_alloc_hit`/`numa_tcache_alloc_miss`/
  `numa_tcache_free_hit`, but unlike its three siblings, was never
  incremented anywhere — a real missing-instrumentation bug, not dead
  code. Now incremented at the tcache-free-miss site in
  `numa_free_with_size()` (the slab-return path taken when a pool-backed
  free doesn't fit the tcache).
- `src/numa_pool.c`: removed `bitmap_find_first_free()`, an entirely
  unused earlier version of the bitmap-scan logic — `bitmap_find_and_set()`
  (used at all 4 call sites) superseded it.
- `numa_slab_free()` (`src/numa_pool.c`/`.h`) took `total_size` and `node`
  parameters that the function never used (it derives everything it needs
  from the pointer's slab header). Trimmed the signature to `(void *ptr)`
  and updated both call sites in `src/zmalloc.c`.
- `src/object.c`: a duplicated comment-opener line before
  `trimStringObjectIfNeeded()` (two consecutive `/* Optimize the SDS
  string...` lines) — a merge/edit artifact, not a real doc change.
- `src/server.c`: fixed a doubled-letter typo in a client-facing error
  string (an extra "r" in the word "interact") that made one of the two
  `CLIENT_SLAVE`
  rejection call sites inconsistent with the other (and with
  `tests/integration/replication.tcl`'s log-pattern assertion, which
  already expected the correct spelling).
- `tests/unit/test_numa_command.sh`: renamed the `strat` loop variable
  (a codespell false positive — `strat` isn't a word) to `strategy_name`.

## [Unreleased] — repository CI/CD and OSS-scaffolding cleanup

### Changed

- `.github/workflows/ci.yml` rewritten from an unmodified copy of upstream
  `redis/redis`'s CI to jobs that actually exercise this fork: installs
  `libnuma-dev` and builds/tests the Redis core (`make test` +
  `runtest-moduleapi`), adds an ASan build (`SANITIZER=address`) covering
  the NUMA allocator's manual memory management, and adds a NUMAflow
  build+test job on both Linux and macOS. Dropped jobs that tested things
  irrelevant or actively wrong for this project: 32-bit, Debian-old,
  generic macOS, and a CentOS 7 jemalloc build (jemalloc is incompatible
  with the NUMA allocator, and the job never installed `libnuma-devel`
  either).
- `.github/workflows/daily.yml` and `.github/workflows/external.yml`
  removed — upstream's scheduled fleet-regression workflows, gated on
  `github.repository == 'redis/redis'`, so they never ran on this fork at
  all.
- `.github/workflows/codeql-analysis.yml` — dropped the same
  `redis/redis`-only gate so CodeQL actually runs here.

### Added

- `.github/PULL_REQUEST_TEMPLATE.md`.
- `LICENSE` at the repo root (same BSD-3-Clause text as `COPYING`, kept for
  tooling/humans that specifically look for `LICENSE`).
- `docs/legacy/` — houses `00-RELEASENOTES`, `MANIFESTO`, `BUGS`, `INSTALL`,
  moved out of the repo root; these are unmodified upstream Redis artifacts
  kept only for provenance.

## [Unreleased] — relative-performance benchmark: real placement trace x calibrated cost model (ADR-12)

### Added

- `numaflow replay --trace <name>=<file.json> ...` (`numaflow/src/nf_cli.c`):
  a new CLI subcommand that feeds a real placement trace (a JSON array of
  `{key,size,access_count,origin_node,final_node}`) through NUMAflow's
  existing pure-function cost model (`nf_numa_access_cost`/
  `nf_numa_migrate_cost`), supporting the same `--cxl-latency-ns`/
  `--cxl-bandwidth-mbps` calibration as `eval`, and producing JSON
  shape-compatible with `bench_<workload>.json`'s `migration` array —
  `numaflow/eval/report.py` and `tests/report/generate_full_report.py` need
  zero code changes to render an extra comparison panel.
- `tests/vm/collect_relative_trace.sh` (runs in the guest) +
  `tests/vm/relative_perf_bench.sh` (runs on the host): collects a real
  placement trace for the noop/composite_lru/tinylfu/caat strategies inside
  a real dual-node QEMU guest (an immediate post-fill snapshot + several
  manual `NUMA FLOW RUN default` triggers + a final snapshot), then feeds
  it through `numaflow replay` to produce `results/bench_relative_perf.json`
  / `bench_relative_perf_cxlcal.json`, printing a bilingual disclaimer that
  this is a modeled projection, not measured latency.
- See ADR-12 (`docs/new/09-architecture-decisions.md`) for the two
  methodology bugs found and fixed while building this: (1) assuming every
  key's origin node was 0 (in fact, under the `local_first` allocation
  strategy, the origin node depends on which vCPU the allocating thread
  happened to be scheduled on), and (2) `local_hit_ratio` initially
  computed as `final_node==origin_node`, which is structurally 100% for any
  strategy that never migrates anything, regardless of actual placement
  quality.

## [Unreleased] — first validation of the migration path on real dual-NUMA-node hardware (ADR-11)

Running the migration path for the first time inside a real dual-NUMA-node
QEMU guest (`tests/vm/boot_numa_vm.sh`) immediately exposed two bugs that
were previously impossible to trigger — the dev host has only 1 NUMA node,
so `numa_pool_num_nodes()==1` forced `migrations` to always be 0, leaving
the entire execution path at zero coverage.

### Fixed

- **NUMAflow-driven migration silently failed 100% of the time**
  (`applied=0`, despite the strategy correctly deciding on dozens of
  migrations). `numa_migrate_key_by_name()` calls
  `dictFind(db->dict, keyname)` internally, and `db->dict` uses SDS keys —
  both `dictSdsHash()` and `dictSdsKeyCompare()` call `sdslen()` on the
  **lookup key**. The NUMAflow bridge (`numa_flow_apply` in
  `src/numa_flow.c`) passes `nf_item_t.key`, a plain `char[]`, so
  `sdslen()` reads garbage length out of the bytes **before** the pointer,
  guaranteeing a miss (and an out-of-bounds read) on every lookup. The
  "must be SDS" contract was documented only in a header comment, while
  the signature read `const char *`, completely hiding the trap. Fixed by
  adding `numa_key_migrate_dict_find()`, which normalizes internally so
  both forms are safe. Measured: `successful_migrations=0` before the fix,
  `=50` after.
- **The `composite_lru` preset never migrated any data** (migration count
  stuck at 0). `src/numa_flow.c` set `br.ctx.tick` to the full 24-bit
  `server.lruclock` (currently ~8.6 million), while `nf_item_t.recency`
  comes from the zmalloc prefix's **uint16_t** `last_access` (only the low
  16 bits, 0–65535). Every `idle = ctx->tick - it.recency` computation in
  the DAG (`op_score_hotness` / `op_decay_hotness`) therefore got an idle
  time of several million seconds, so `nf_staircase_decay()` always
  returned the maximum decay, hotness was permanently pinned at 3, and
  everything was filtered out by `filter_hot threshold=5`. `caat`/
  `tinylfu` were unaffected because they gate on CMS frequency, not
  hotness — which is why only composite_lru misbehaved. Fixed by
  truncating `ctx.tick` to 16 bits to match the prefix's precision.

### Added

- `tests/vm/placement_quality.sh` — a comparison script that measures
  **placement quality** inside a real dual-node guest. It doesn't measure
  throughput/latency (QEMU's two `-numa node`s share the same host DRAM
  underneath, so there's no real latency gap to show a migration benefit
  for) — instead it measures where a strategy actually puts data:
  `hot_local_ratio` (fraction of hot keys resident on their local node),
  `cold_off_ratio` (fraction of cold keys moved off their local node), and
  the actual migration count. This metric doesn't depend on any latency
  model, so it's comparable across strategies.

## [Unreleased] — NUMAflow bridge scalability fixes (ADR-10)

### Fixed

- **`numa_flow_cron()` did a full, uncapped `dictGetSafeIterator()` walk of
  the entire keyspace every tick** (`numa-flow-interval-sec`, default 1s).
  With a multi-hundred-thousand-key dataset this turned every tick into an
  O(keyspace size) synchronous scan blocking Redis's single command-processing
  thread — the server became unresponsive to a plain `PING` for minutes.
  This was always latent in the bridge design but never triggered before
  ADR-08 made NUMAflow auto-load and run by default (previously opt-in only,
  so nobody ran it against a real, large keyspace). Fixed by switching to a
  bounded, resumable scan: `dictScan()` (the same primitive behind the
  `SCAN` command) accumulates up to `NUMA_FLOW_SCAN_BATCH` (4096) keys per
  tick and persists its cursor on the workflow entry, so a full pass over
  the keyspace is spread across many ticks instead of done all at once —
  mirroring the "progressive dictionary scan" the retired native
  composite_lru module used for exactly this reason.
- **A real, CPU-pegging infinite loop** in `numaflow/src/nf_bridge.c`'s
  internal `kn_map_t` (key -> original-node lookup table used to diff
  enumerate-time state against the DAG's final result). `kn_init()`
  hardcoded its capacity to ~2049 slots with no resize logic; `kn_put()`'s
  linear-probe insert loop (`while (m->keys[i]) i++`) never terminates once
  the table fills, since there is no empty slot left to find. The scan fix
  above (4096 keys/tick) was the first thing to ever push a real call
  through this path above that hidden 2049-slot ceiling — this bug predates
  this session entirely but was never triggered (numaflow's own benchmark
  harness, `nf_bench.c`, calls the DAG executor directly and never goes
  through this table at all). Fixed by making `kn_map_t` a proper
  auto-resizing hash table (doubles + rehashes at 70% load factor).
  Verified at ~700k-1M keys via `redis-benchmark`: healthy throughput
  (~700k req/s), `PING` responsive throughout and after, full Redis
  `make test`, numaflow's own test suite, and `test_numa_command.sh` all
  still pass. See ADR-10 in `docs/new/09-architecture-decisions.md` for the
  full diagnosis, including why the prior session's bug-fix verification
  (correct but small-scale) wasn't sufficient to catch this.

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
