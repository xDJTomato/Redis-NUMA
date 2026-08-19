# Contributing

This fork follows standard GitHub PR workflow. This document covers the
project-specific conventions for working on the NUMA modules and NUMAflow
subsystem; for generic Redis contribution etiquette (issue triage, mailing
list, security reports) see the upstream conventions linked from
[`SECURITY.md`](SECURITY.md).

## Adding a new NUMA module

1. Create `.h` (interfaces/structs) then `.c` (implementation) under `src/`.
2. Add `numa_xxx.o` to `REDIS_SERVER_OBJ` in `src/Makefile`. NUMA `.o` files
   must appear **after** `server.o` in the link order.
3. Include the header in `server.h` under `#ifdef HAVE_NUMA`.
4. Call the module's init function in `server.c`, **after**
   `initServer()` completes — every existing NUMA module depends on
   `initServer()`'s state, and `numa_init()` itself must run before
   `initServer()`. Getting this ordering wrong is the single most common
   way to introduce a hard-to-diagnose startup crash in this fork.
5. Use `extern void _serverLog(int level, const char *fmt, ...)`, not
   `serverLog()` directly — this is the established Redis-internal
   convention. See existing modules (`numa_composite_lru.c`,
   `numa_bw_monitor.c`, etc.) for the pattern.
6. Respect the module dependency order documented in
   [`ARCHITECTURE.md`](ARCHITECTURE.md#module-dependency-order-bottom-to-top):
   `libnuma -> numa_pool -> numa_migrate -> numa_key_migrate ->
   {numa_composite_lru, numa_tinylfu, numa_strategy_slots} -> numa_command
   -> evict_numa -> server.c`.

## Before opening a PR

- `cd src && make clean && make -j$(nproc)` must build with zero errors and
  no new warnings.
- `make test` (from the repo root) must pass the full Tcl suite.
- If you touched NUMAflow (`numaflow/`), `cd numaflow && make test` must
  also pass — it has no Redis/libnuma dependency and is a useful control:
  if it breaks, the bug is in NUMAflow itself, not in the Redis-side
  integration.
- Run `./run_full_validation.sh --quick` for a fast combined check, or the
  full `./run_full_validation.sh` if your change touches migration
  strategies, allocation, or eviction (it also exercises the QEMU
  multi-NUMA-node path). See [`TESTING.md`](TESTING.md).

## Lessons that shape these rules

These aren't arbitrary — each one traces back to a real bug this fork hit:

- **A clean merge is not a correct merge.** `git merge`'s recursive
  resolution can silently duplicate, drop, or corrupt code with zero
  conflict markers when both sides restructured the same region. Six such
  bugs survived the Redis 6.2.21&rarr;7.2.6 core merge undetected by a
  conflict-marker grep; only an actual `make -j$(nproc)` + `make test`
  caught them. See [`docs/redis7-migration.md`](docs/redis7-migration.md)
  for the full list. **Never stage a non-trivial merge on "no conflict
  markers" alone.**
- **Grep the whole build log, not a truncated head of it.** A `head -30` on
  a build log missed a `dictType` callback-signature mismatch and a
  `createIntConfig`/`createSizeTConfig` field-width mismatch, both of which
  only showed up as compiler *warnings* further down. Always grep for
  `implicit declaration|incompatible pointer|too few arguments|too many
  arguments` across the full log.
- **Audit every binary entry point for allocator-init ordering, not just
  `redis-server`'s `main()`.** `numa_init()` is only called there;
  `redis-cli`/`redis-benchmark`/`redis-check-rdb`/`redis-check-aof`/
  `redis-sentinel` all link `zmalloc.o` but never call it. Any new NUMA-path
  code that assumes `numa_ctx.numa_available` is set needs an explicit
  fallback for binaries that never initialize it.
- **A merge base tag is not a security baseline.** Cherry-pick known CVEs
  for the exact files a merge pulls in, separately from diffing for
  NUMA-relevant changes — picking `7.2.6` did not include the
  CVE-2025-32023 HyperLogLog fix, which landed after that tag.
