# Changelog

[中文版](CHANGELOG.zh-CN.md)

All notable changes to this fork are documented here, in the style of
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased] — 相对性能基准：真实放置轨迹 x 标定代价模型（ADR-12）

### Added

- `numaflow replay --trace <name>=<file.json> ...`（`numaflow/src/nf_cli.c`）：
  新 CLI 子命令，把一份真实放置轨迹（`{key,size,access_count,origin_node,
  final_node}` 的 JSON 数组）喂进 NUMAflow 已有的纯函数代价模型
  （`nf_numa_access_cost`/`nf_numa_migrate_cost`），支持和 `eval` 相同的
  `--cxl-latency-ns`/`--cxl-bandwidth-mbps` 标定，输出和
  `bench_<workload>.json` 的 `migration` 数组同构的 JSON——`numaflow/eval/
  report.py`、`tests/report/generate_full_report.py` 不需要改一行代码即可
  多画一张对比面板。
- `tests/vm/collect_relative_trace.sh`（guest 内运行）+
  `tests/vm/relative_perf_bench.sh`（开发机上运行）：在真实双节点 QEMU
  guest 里对 noop/composite_lru/tinylfu/caat 四个策略采集真实放置轨迹
  （fill 后即时快照 + 手动触发 `NUMA FLOW RUN default` 若干次 + 最终快照），
  取回后跑 `numaflow replay` 产出 `results/bench_relative_perf.json` /
  `bench_relative_perf_cxlcal.json`，并打印"这是建模投影不是实测延迟"的
  双语免责声明。
- 详见 ADR-12（`docs/new/09-architecture-decisions.md`），包括实现过程中
  发现并修正的两个方法论错误：(1) 假设所有 key 起始节点是 0（实际上
  `local_first` 分配策略下起始节点取决于分配调用发生时线程被调度到哪个
  vCPU），(2) `local_hit_ratio` 一开始按 `final_node==origin_node` 算，导致
  任何从不迁移的策略都结构性地恒为 100%，与实际放置质量无关。

## [Unreleased] — 迁移路径在真实双 NUMA 节点上的首次验证（ADR-11）

在 QEMU 双 NUMA 节点 guest（`tests/vm/boot_numa_vm.sh`）里第一次真正执行迁移路径，
立刻暴露两个此前完全无法被发现的 bug——开发主机只有 1 个 NUMA 节点，
`numa_pool_num_nodes()==1` 导致 `migrations` 恒为 0，整条执行路径零覆盖。

### Fixed

- **NUMAflow 驱动的迁移 100% 静默失败**（`applied=0`，尽管策略正确决策了几十次
  迁移）。`numa_migrate_key_by_name()` 内部做 `dictFind(db->dict, keyname)`，而
  `db->dict` 用 SDS 键——`dictSdsHash()` 和 `dictSdsKeyCompare()` 都会对**查找键**
  调用 `sdslen()`。NUMAflow 桥接（`src/numa_flow.c` 的 `numa_flow_apply`）传的是
  `nf_item_t.key`，一个普通 `char[]`，于是 `sdslen()` 把指针**前面**的字节当 SDS
  头读出垃圾长度，每次查找必然 miss（而且是越界读）。原契约"必须传 SDS"只写在
  头文件注释里，而签名是 `const char *`，把这个陷阱完全隐藏了。修复：新增
  `numa_key_migrate_dict_find()` 在函数内部归一化，两种形式都安全。
  实测：修复前 `successful_migrations=0`，修复后 `=50`。
- **`composite_lru` 预设永远不迁移任何数据**（迁移次数恒为 0）。
  `src/numa_flow.c` 把 `br.ctx.tick` 设成完整的 24 位 `server.lruclock`（当前约
  860 万），而 `nf_item_t.recency` 来自 zmalloc 前缀的 **uint16_t** `last_access`
  （只有低 16 位，0–65535）。于是 DAG 里所有 `idle = ctx->tick - it.recency`
  的计算（`op_score_hotness` / `op_decay_hotness`）都得到数百万秒的空闲时间，
  `nf_staircase_decay()` 恒定返回最大衰减值，hotness 被永久压到 3，被
  `filter_hot threshold=5` 全部滤除。`caat`/`tinylfu` 不受影响，因为它们用 CMS
  频率而非 hotness 做门控——这也解释了为什么只有 composite_lru 表现异常。
  修复：把 `ctx.tick` 截断到 16 位以匹配前缀精度。

### Added

- `tests/vm/placement_quality.sh` — 在真实双节点 guest 里测量**放置质量**的对比
  脚本。测的不是吞吐/延迟（QEMU 的两个 `-numa node` 背后是同一块宿主机 DRAM，
  没有真实延迟差，测不出迁移收益），而是策略把数据放在哪：`hot_local_ratio`
  （热 key 驻留本地节点比例）、`cold_off_ratio`（冷 key 被挪离本地节点比例）、
  实际迁移次数。这个指标不依赖任何延迟建模，跨策略可比。

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
