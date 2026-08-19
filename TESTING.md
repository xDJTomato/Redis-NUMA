# Testing

## Quick reference

```bash
./run_full_validation.sh --quick     # build + make test + NUMAflow benchmark
./run_full_validation.sh             # ...plus YCSB (if java is present),
                                      #    QEMU VM smoke test, CXLMemSim check
```

Every run writes `results/full_report_<timestamp>/index.html` plus the raw
logs/JSON for each step. A step that cannot run in the current environment
is recorded as `skipped` with the reason — never fabricated.

Flags: `--skip-build`, `--skip-test`, `--skip-ycsb`, `--skip-vm`,
`--skip-cxlmemsim`, `--vm-timeout SECONDS`. `--quick` is shorthand for
`--skip-ycsb --skip-vm --skip-cxlmemsim`.

## Test tiers

### 1. Standard Redis Tcl suite

```bash
cd src && make clean && make -j$(nproc)
cd .. && make test
```

Runs the full `tests/unit/*.tcl` suite. This is where all six of the silent
merge-corruption bugs and the one pre-existing allocator-guard bug described
in [`docs/redis7-migration.md`](docs/redis7-migration.md) were actually
caught — treat any regression here as a serious signal, not noise.

### 2. NUMA environment scripts

```bash
./utils/numa/check_numa_config.sh
./utils/numa/diagnose_numa.sh
```

Quick sanity checks against the host's real NUMA topology (works fine when
the host has only 1 physical node — the checks degrade gracefully).

### 3. NUMAflow subsystem (pure C11, no Redis dependency)

```bash
cd numaflow && make && make test && make report
```

`make test` runs unit, smoke, bridge/adapt, and allocator tests. `make
report` runs the fair-evaluation harness across the `zipf`/`uniform`/
`hotspot`/`temporal` workloads and regenerates `results/bench_*.json` +
`results/report.html` (bar charts comparing noop/composite_lru/tinylfu/CAAT
net cost and local-hit-ratio, pure stdlib SVG, no matplotlib dependency).

### 4. YCSB bandwidth benchmark

```bash
cd tests/ycsb && ./run_bw_benchmark.sh    # 3-phase: Fill -> Hotspot -> Sustain
cd tests/ycsb && ./run_ycsb.sh            # baseline/stress modes
```

Requires a JDK and the YCSB distribution (`run_full_validation.sh` skips
this step automatically if `java` isn't on `PATH` — it does not attempt to
install a JDK for you).

### 5. QEMU multi-NUMA-node smoke test

```bash
./tests/vm/boot_numa_vm.sh [--timeout SECONDS] [--keep] [--skip]
```

Boots a Debian 12 generic-cloud image (downloaded once, cached under
`tests/vm/.cache/`, gitignored) with 2 emulated NUMA nodes via
`-object memory-backend-ram` + `-numa node,memdev=...`, waits for SSH with
an honest timeout (default 480s — this host has no `/dev/kvm`, so boot is
pure TCG software emulation and slow), copies the locally built
`redis-server`/`redis-cli`/`redis-benchmark` in, and runs `PING`/`SET`/`GET`,
`NUMA CONFIG GET`, `NUMA STRATEGY LIST`, and a `redis-benchmark` pass inside
the guest. NUMA-node visibility is checked by reading
`/sys/devices/system/node/` directly rather than shelling out to
`numactl` — that package isn't preinstalled on the cloud image, and
installing it means a network-bound `apt-get` that can take many minutes
under TCG's slirp NAT for no benefit over reading sysfs directly.

If the VM doesn't become SSH-reachable within the timeout, the script logs
the last serial-console output, writes a `"timeout"` status to
`tests/vm/results/qemu_smoke_<ts>.json`, and exits 0 (a slow/unavailable
QEMU environment is not treated as a NUMA-code bug).

### 6. CXLMemSim device-link check

```bash
./tests/cxl/run_cxlmemsim.sh [--timeout SECONDS] [--skip]
```

Requires `external/CXLMemSim` (clone from `SlugLab/CXLMemSim`) with its own
patched QEMU and `cxlmemsim_server` already built (`script/build_qemu.sh` +
`cmake --build build`). Runs CXLMemSim's own CTest suite, then boots the
patched QEMU with a CXL Type2 endpoint pointed at `cxlmemsim_server` over
TCP and confirms the device actually links up (checked via `"Device
realized"` and `"Connected to CXLMemSim"` in the QEMU log) — no guest OS
boot is needed, QEMU is paused with `-S` immediately after device
realization. See [`ARCHITECTURE.md`](ARCHITECTURE.md#external-validation-layers)
for why this validates the device-emulation link rather than a full
Redis-inside-CXLMemSim-guest run, and where to find the Redis-level
DRAM-vs-far-memory comparison instead
(`tests/ycsb/scripts/eval_cxl_memory.sh`, run inside a real &ge;2-NUMA-node
environment such as the VM from step 5).

## Manual functional smoke tests

Useful when iterating on a specific module without running the whole suite:

```bash
./src/redis-server ./redis.conf --daemonize yes --logfile /tmp/r.log
./src/redis-cli numa config get
./src/redis-cli numa strategy list
./src/redis-cli numa migrate stats
./src/redis-benchmark -q -n 20000 -c 20 -t set,get
./src/redis-cli --cluster create 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003 \
    --cluster-replicas 0   # exercises dict/iterator code under redis-cli,
                            # not just redis-server
```
