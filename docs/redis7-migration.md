# Redis 6.2.21 &rarr; 7.2.6 migration

[中文版](redis7-migration.zh-CN.md)

This document records what actually happened when the NUMA/CXL fork's Redis
core was migrated from 6.2.21 to 7.2.6, on branch `feat/redis7-port`. It
replaces the earlier `docs/redis8-migration.md`, which was a *paper design*
written before the merge was attempted — several of its assumptions turned
out to be wrong once the merge was actually run, and its accompanying
`src/redis8_compat.h` shim was never wired into the build (nothing included
it) and has been deleted. Everything below is the record of the real merge:
what tools were used, what broke, and how each break was found and fixed.

## Why 7.2.6, not 8

7.2.6 is the last stable release before Redis's licensing change (RSALv2/SSPL
dual license from 7.4 onward) and before the larger 8.x refactors, while still
picking up all the API changes (opaque `dictEntry`, listpack, quicklist
container types) that actually affect this fork's NUMA modules. It keeps the
diff reviewable and stays on the BSD-3-Clause license this fork was already
under.

## Why a manual graft was required

This repository was imported as a fresh history (`git log` shows a single
root commit, "first commit") — it does **not** share commit history with
`redis/redis`. A plain `git merge upstream/7.2.6` against that would be
diffed as two entirely unrelated trees and produce a wall of whole-file
conflicts instead of a real three-way merge.

The fix: fetch the upstream `6.2.21` and `7.2.6` tags, then
`git replace --graft <our-root-commit> <upstream-6.2.21-commit>` to give our
root commit a synthetic parent equal to upstream's own 6.2.21 tree. This lets
Git compute the correct merge base (upstream 6.2.21) for a real three-way
merge between "our 6.2.21 + NUMA modules" and "upstream 7.2.6", instead of a
from-scratch reconciliation.

## The core lesson: a clean merge is not a correct merge

`git merge`'s recursive strategy auto-resolves any hunk where it can compute
*a* result without leaving `<<<<<<<` markers — but "no markers" only means
"no *unresolved* conflict", not "resolved correctly". Six real bugs survived
the merge with zero conflict markers, entirely invisible to a `grep` for
diff3 markers:

| # | File | What survived | Symptom |
|---|------|----------------|---------|
| 1 | `src/zmalloc.c` | Missing `#endif` closing an `#ifdef HAVE_NUMA` block around `PREFIX_SIZE` | Build error, every symbol after it |
| 2 | `src/dict.c` | Stray duplicate overflow-check referencing a variable (`realsize`) that no longer existed after the `dictht`&rarr;`ht_table[]` restructuring | Build error: undeclared identifier |
| 3 | `src/server.c` | Both the 6.2.21 and 7.2.6 bodies of `afterCommand()` kept, back to back | Build error: redefinition |
| 4 | `src/networking.c` | Both bodies of `addReplyBigNum()` and `deferredAfterErrorReply()` kept | Build error: redefinition |
| 5 | `src/server.h` | Stale 2-arg `objectComputeSize()` prototype after the `.c` definition moved to 4 args | Silent implicit-declaration in `evict.c`/`evict_numa.c` &rarr; SIGSEGV under `integration/replication-buffer.tcl` |
| 6 | `src/server.c` | Both the unconditional 6.2.21 call and the flag-guarded 7.2.6 call to `replicationFeedMonitors()` kept inside `call()` | MONITOR clients saw every command fed twice (`unit/introspection.tcl` failure, not a crash) |

None of these showed up as conflict markers. All six were only found by
actually compiling the tree and running the real test suite — grepping for
markers, or eyeballing the diff for "does this look plausible", was not
enough. **Rule going forward: after any non-trivial cross-version merge in
this repo, run the real `make -j$(nproc)` build to completion and treat every
compiler error as a possible silent-corruption site, not just a normal
porting gap.** Distinguish "this needs porting to a new API" from "the merge
mangled working code" by diffing the region against both the pre-merge commit
and the real upstream tag commit — don't guess from the diff alone.

## Porting the NUMA modules to Redis 7's APIs

This part was expected, real work — not merge corruption:

- **`dictCreate(dictType*)`** lost its `privdata` argument; every
  `dictCreate(&sometype, NULL)` call in `numa_key_migrate.c` and
  `numa_composite_lru.c` became `dictCreate(&sometype)`.
- **`dictEntry` became fully opaque.** Code that used to walk
  `d->ht[t].table[i]` by hand (in `numa_object_sample_alloc_ptr`/
  `numa_object_sample_alloc_size`) was rewritten to use
  `dictGetIterator`/`dictNext`/`dictGetKey`/`dictGetVal`/`dictReleaseIterator`,
  and to size entries with `dictEntryMemUsage()` and `dictSlots(d)` instead of
  `sizeof(dictEntry)` and manual slot-count math.
- **dictType callbacks** (`keyCompare`/`keyDup`/`keyDestructor`/
  `valDestructor`) take a `dict *d` first parameter instead of
  `void *privdata` now; updated in both modules' callback tables.
- **`dictGenHashFunction`** takes `size_t len`, not `int len` — dropped a
  redundant, now-incorrect `extern` redeclaration in `numa_tinylfu.c` that
  shadowed the correct prototype from `dict.h` with the old signature.
- **`quicklistNode->zl` was renamed to `->entry`** throughout the LIST
  migration adapter in `numa_key_migrate.c`.
- **Hash/zset encodings**: `OBJ_ENCODING_ZIPLIST` is kept for RDB
  compatibility, but new objects use `OBJ_ENCODING_LISTPACK`. Added
  `OBJ_ENCODING_LISTPACK` branches (using `lpBytes()`) alongside the existing
  ziplist branches in `migrate_hash_type()` and `migrate_zset_type()`. The
  migration itself is still a single `memcpy` of the packed blob either way,
  so the risk here was lower than initially expected — no per-entry walking
  needed.
- **`src/commands/numa.json`** was written to register `NUMA` with Redis 7's
  declarative command-introspection system (`COMMAND INFO`, `COMMAND DOCS`),
  regenerating `src/commands.def`.

## A pre-existing bug the merge exposed (not caused)

`numa_init()` — which sets up the slab/direct-cache allocator state in
`zmalloc.c` — is only ever called from `server.c`'s `main()`. Every other
binary that links `zmalloc.o` (`redis-cli`, `redis-benchmark`,
`redis-check-rdb`/`aof`, `redis-sentinel`) never calls it, so
`numa_ctx.numa_available` stays 0 for that process's whole lifetime.
`zmalloc()`/`zcalloc()`/`zrealloc()` already guarded on that flag with a
plain-`malloc()` fallback — but `zmalloc_local()`/`zcalloc_local()`/
`ztrycalloc_local()` (used by `dict.c`'s `dictCreate()`/`dictGetIterator()`)
called `numa_alloc_dram()` unconditionally, running the slab allocator
against never-initialized global state. This corrupts the heap in a way that
only crashes later, inside an unrelated `free()`.

This bug is identical in the pre-merge 6.2.21 `dict.c`/`zmalloc.c` — it always
existed, but nothing in the pre-7.x test suite happened to exercise
`redis-cli`'s dict/iterator code paths under NUMA. Redis 7's
`tests/unit/cluster/cli.tcl` (`redis-cli --cluster create`, which builds an
anti-affinity-score dict) was the first thing to trip it, surfacing as a
`redis-cli` SIGSEGV inside `zfree()`. Fixed by adding the same
`numa_ctx.numa_available` guard (with a matching plain-`malloc` fallback) to
`numa_alloc_dram()`.

**Lesson**: a "NUMA-aware" fork needs to audit every binary entry point, not
just `redis-server`'s `main()`, for allocator-init ordering. This class of bug
is invisible until a non-server binary happens to exercise the right code
path.

## A security fix that had to be cherry-picked separately

Picking `7.2.6` as the merge base does not mean picking a fully-patched
security baseline. `tests/unit/hyperloglog.tcl`'s "Corrupted sparse
HyperLogLogs ... XZERO opcode" test crashed `hllMerge()` in byte-identical,
zero-diff vanilla `src/hyperloglog.c` — this is
[CVE-2025-32023](https://github.com/redis/redis/security/advisories) (a
HyperLogLog out-of-bounds write), fixed upstream in commit `f35b72dd1`, which
lands chronologically *after* the `7.2.6` tag. Cherry-picked cleanly onto
`hyperloglog.c`; the only conflict was in the `.tcl` test file itself
(7.2.6's test suite already had a differently-worded but logically identical
regression test — resolved by keeping the existing wording).

**Lesson**: always check for known CVEs against the exact file(s) a merge
base pulls in, separately from diffing for NUMA-relevant changes.

## Two config-registration bugs caught by compiler warnings, not errors

- `src/config.c`: `numa-demote-min-size` was registered with
  `createIntConfig()` against a field that is actually `size_t` in
  `server.h` — silent truncation on `CONFIG SET` for values near/above
  `INT_MAX`. Fixed by switching to `createSizeTConfig()`.
- Both bugs above, plus the dictType-callback-signature issue, were found by
  re-grepping the *full* build log for
  `implicit declaration|incompatible pointer|too few arguments|too many arguments`
  after an earlier, truncated `head -30` grep had missed them. **Always grep
  the whole log, not a truncated head of it** — a build can emit far more
  than 30 lines of interesting warnings before the first error.

## Verification performed

- `cd src && make clean && make -j$(nproc)`: zero errors, only pre-existing/
  unrelated warnings.
- `make test` (full Tcl suite): 91/91 files passed with zero errors on the
  final run (four earlier runs each caught and fixed one of the bugs above).
- `numaflow`'s own test suite (`cd numaflow && make test`): unaffected by the
  core merge, since NUMAflow has no Redis/libnuma dependency; kept green
  throughout as a control.
- Manual functional smoke tests: `NUMA` command registration and all three
  subcommands (`CONFIG`, `STRATEGY`, `MIGRATE`) via `redis-cli`, an
  eviction-storm stress test across all 5 data types, and a live 3-node
  `redis-cli --cluster create`.
- `tests/vm/boot_numa_vm.sh`: boots a 2-NUMA-node QEMU guest (software TCG,
  this host has no `/dev/kvm`) and runs `redis-server` + the `NUMA` command
  set + `redis-benchmark` inside it. See [TESTING.md](../TESTING.md).

## What was deleted

- `src/redis8_compat.h` — dead code. Nothing in the tree ever included it;
  the actual port went directly to Redis 7's real APIs (see above) rather
  than through a compatibility shim. Keeping it around would misrepresent
  how the migration actually happened.
- `docs/redis8-migration.md` — superseded by this document.
