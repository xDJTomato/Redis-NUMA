#!/bin/bash
# ============================================================
# CXLMemSim device-emulation link validation for Redis-NUMA.
#
# CXLMemSim (https://github.com/SlugLab/CXLMemSim) is checked out
# under external/CXLMemSim with its own patched QEMU (lib/qemu)
# and cxlmemsim_server already built. This script:
#
#   1. Runs CXLMemSim's own CTest suite (protocol/coherence unit
#      tests -- no QEMU involved).
#   2. Boots the patched QEMU with a CXL Type2/Type3 endpoint that
#      forwards memory-timing requests to cxlmemsim_server over
#      TCP, and confirms the device actually links up (no OS boot
#      needed -- QEMU is paused with -S immediately after device
#      realization, matching the project's own qemu_integration/
#      smoke_type2_endpoint.sh pattern).
#   3. Replays the same four workload shapes NUMAflow's fair
#      evaluation harness uses (zipf/uniform/hotspot/temporal)
#      directly through CXLMemSim's own C++ CXLMemExpander model
#      (cxlmemsim_workload_bench.cpp), so there's a real-model
#      comparison point alongside NUMAflow's simplified single
#      latency/bandwidth cost model.
#
# Honest scope note: this validates the *device-emulation link*
# (QEMU <-> cxlmemsim_server), which is CXLMemSim's own stated
# scope -- a software timing model, not cycle-accurate hardware
# (see CXLMemSim's README). It does NOT run redis-server inside a
# fully-booted CXLMemSim guest with redis actually touching the
# emulated CXL memory. A real attempt was made (booting the same
# Debian 12 cloud image tests/vm/boot_numa_vm.sh uses, but under
# CXLMemSim's patched QEMU with a cxl-type2 endpoint attached
# instead of a second -numa node): the guest's stock kernel sees
# the device on the PCI bus fine (lspci shows the CXL [0502]
# 8086:0d92 endpoint) and already ships cxl_pci/cxl_acpi/cxl_mem
# modules, but cxl_pci's bind fails (I/O error, no dmesg) because
# CXLMemSim's own qemu_integration/launch_qemu_vcs_dcd_gfam.sh
# expects a *custom-patched* Linux kernel
# (/root/linux-cxl-type2/arch/x86/boot/bzImage) to actually expose
# this device's memory as guest RAM/NUMA capacity -- a stock distro
# kernel's driver isn't enough. Building that patched kernel was
# judged out of scope for this pass, so redis-server was never able
# to touch CXL-emulated memory through this device. See
# ARCHITECTURE.md's "External validation layers" section for the
# full account. For an actual Redis-level DRAM-vs-far-memory
# comparison, see tests/ycsb/scripts/eval_cxl_memory.sh, which
# uses numactl --membind across 2 NUMA nodes (works inside the
# 2-node VM booted by tests/vm/boot_numa_vm.sh; this bare host
# has only 1 physical NUMA node).
#
# Usage: ./run_cxlmemsim.sh [--timeout SECONDS] [--skip]
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CXL_ROOT="$PROJECT_ROOT/external/CXLMemSim"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()      { echo -e "[$(date +%H:%M:%S)] $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*"; }

DEVICE_TIMEOUT=8
SKIP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) DEVICE_TIMEOUT="$2"; shift 2 ;;
        --skip) SKIP=1; shift ;;
        *) log_warn "unknown arg: $1"; shift ;;
    esac
done

RESULT_JSON="$RESULTS_DIR/cxlmemsim_$(date +%Y%m%d_%H%M%S).json"
write_result() {
    cat > "$RESULT_JSON" <<EOF
{
  "phase": "cxlmemsim_integration",
  "status": "$1",
  "reason": "$2",
  "ctest_status": "${CTEST_STATUS:-not_run}",
  "device_link_status": "${DEVICE_LINK_STATUS:-not_run}",
  "native_bench_status": "${NATIVE_BENCH_STATUS:-not_run}",
  "native_bench_result": "${NATIVE_BENCH_JSON_PATH:-}",
  "note": "device link validates QEMU<->cxlmemsim_server timing-forwarding path only; redis-level DRAM-vs-far-memory numbers come from tests/ycsb/scripts/eval_cxl_memory.sh run inside a real 2-NUMA-node environment (see tests/vm/boot_numa_vm.sh), not from a guest booted through this device"
}
EOF
    log "result written to $RESULT_JSON"
}

if [[ "$SKIP" == "1" ]]; then
    write_result "skipped" "explicitly skipped via --skip"
    exit 0
fi

if [[ ! -d "$CXL_ROOT" ]]; then
    log_warn "external/CXLMemSim not present -- clone with: git clone https://github.com/SlugLab/CXLMemSim $CXL_ROOT"
    write_result "skipped" "external/CXLMemSim not cloned"
    exit 0
fi

QEMU_BIN="$CXL_ROOT/lib/qemu/build/qemu-system-x86_64"
SERVER_BIN="$CXL_ROOT/build/cxlmemsim_server"
BUILD_DIR="$CXL_ROOT/build"

if [[ ! -x "$SERVER_BIN" ]] || [[ ! -d "$BUILD_DIR" ]]; then
    log_warn "cxlmemsim_server not built -- run: cd external/CXLMemSim && cmake -B build && cmake --build build -j\$(nproc)"
    write_result "skipped" "cxlmemsim_server not built"
    exit 0
fi

# ---- 1. CXLMemSim's own unit/coherence test suite ----
log "running CXLMemSim CTest suite"
CTEST_LOG="$RESULTS_DIR/cxlmemsim_ctest.log"
if (cd "$BUILD_DIR" && timeout 180 ctest --output-on-failure) > "$CTEST_LOG" 2>&1; then
    CTEST_STATUS="passed"
    log_ok "CXLMemSim CTest suite passed ($(grep -oE '[0-9]+% tests passed' "$CTEST_LOG" | tail -1))"
else
    CTEST_STATUS="failed"
    log_warn "CXLMemSim CTest suite reported failures, see $CTEST_LOG"
fi

# ---- 2. Native workload bench through CXLMemSim's own C++ model ----
# Independent of QEMU/the patched-QEMU build below -- only needs
# libcxlmemsim.a + headers, so it still runs when the heavier patched
# QEMU device-link step (step 3) is unavailable.
NATIVE_BENCH_SRC="$SCRIPT_DIR/cxlmemsim_workload_bench.cpp"
NATIVE_BENCH_BIN="$RESULTS_DIR/.cxlmemsim_workload_bench"
NATIVE_BENCH_BUILD_LOG="$RESULTS_DIR/cxlmemsim_workload_bench_build.log"
if g++ -std=c++20 -O2 -pthread \
        -I"$CXL_ROOT/include" -I"$CXL_ROOT/src" \
        "$NATIVE_BENCH_SRC" "$BUILD_DIR/libcxlmemsim.a" \
        -lfmt -lrt -latomic \
        -o "$NATIVE_BENCH_BIN" > "$NATIVE_BENCH_BUILD_LOG" 2>&1; then
    NATIVE_BENCH_JSON_PATH="$RESULTS_DIR/cxlmemsim_native_bench_$(date +%Y%m%d_%H%M%S).json"
    if "$NATIVE_BENCH_BIN" --keys 20000 --accesses 200000 \
            --read-bw-gbps 25 --write-bw-gbps 25 --read-lat-ns 100 --write-lat-ns 150 \
            --capacity-mb 256000 --dram-latency-ns 60 \
            --out "$NATIVE_BENCH_JSON_PATH" > /dev/null 2>&1; then
        NATIVE_BENCH_STATUS="passed"
        log_ok "native workload bench complete: $NATIVE_BENCH_JSON_PATH"
    else
        NATIVE_BENCH_STATUS="failed"
        NATIVE_BENCH_JSON_PATH=""
        log_warn "native workload bench binary exited non-zero"
    fi
else
    NATIVE_BENCH_STATUS="skipped"
    NATIVE_BENCH_JSON_PATH=""
    log_warn "could not build cxlmemsim_workload_bench (g++/libfmt/libcxlmemsim.a missing?), see $NATIVE_BENCH_BUILD_LOG"
fi

# ---- 3. QEMU <-> cxlmemsim_server device link ----
if [[ ! -x "$QEMU_BIN" ]]; then
    log_warn "patched QEMU not built at $QEMU_BIN -- run external/CXLMemSim/script/build_qemu.sh"
    DEVICE_LINK_STATUS="skipped"
    write_result "partial" "ctest=$CTEST_STATUS, native bench=$NATIVE_BENCH_STATUS, device link skipped (patched qemu not built)"
    exit 0
fi

PORT=10199
SERVER_LOG="$RESULTS_DIR/cxlmemsim_server.log"
QEMU_LOG="$RESULTS_DIR/cxlmemsim_qemu_device.log"

"$SERVER_BIN" --comm-mode=tcp --port="$PORT" --capacity=256 --default_latency=100 > "$SERVER_LOG" 2>&1 &
SERVER_PID=$!
trap '[[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" >/dev/null 2>&1' EXIT

log "waiting for cxlmemsim_server on port $PORT"
up=0
for _ in $(seq 1 50); do
    timeout 1 bash -c "</dev/tcp/127.0.0.1/$PORT" >/dev/null 2>&1 && { up=1; break; }
    kill -0 "$SERVER_PID" 2>/dev/null || break
    sleep 0.2
done

if [[ "$up" != "1" ]]; then
    log_err "cxlmemsim_server did not open its TCP port"
    DEVICE_LINK_STATUS="failed"
    write_result "partial" "ctest=$CTEST_STATUS, native bench=$NATIVE_BENCH_STATUS, device link failed (server did not start)"
    exit 0
fi

log "launching patched QEMU with a CXL Type2 endpoint pointed at the server (paused after device realization, timeout ${DEVICE_TIMEOUT}s)"
timeout "$DEVICE_TIMEOUT" "$QEMU_BIN" \
    -M q35,cxl=on \
    -m 512M -smp 1 -nodefaults -display none -serial none -monitor none -S \
    -device pxb-cxl,bus_nr=12,bus=pcie.0,id=cxl.0 \
    -device cxl-rp,port=0,bus=cxl.0,id=type2_rp,chassis=0,slot=2 \
    -device cxl-type2,bus=type2_rp,id=cxl-type2-smoke,sn=200,gpu-mode=0,cache-size=16M,mem-size=64M,cxlmemsim-addr=127.0.0.1,cxlmemsim-port="$PORT",coherency-enabled=true \
    > "$QEMU_LOG" 2>&1
QEMU_RC=$?

if grep -q "Connected to CXLMemSim" "$QEMU_LOG" && grep -q "Device realized" "$QEMU_LOG"; then
    DEVICE_LINK_STATUS="passed"
    log_ok "device link established: $(grep 'Device realized' "$QEMU_LOG")"
    log "server-side simulated topology:"
    grep -A2 "Expander endpoint" "$SERVER_LOG" | sed 's/^/    /'
else
    DEVICE_LINK_STATUS="failed"
    log_warn "device did not report a confirmed link within ${DEVICE_TIMEOUT}s (qemu rc=$QEMU_RC); see $QEMU_LOG"
fi

kill "$SERVER_PID" >/dev/null 2>&1
trap - EXIT

if [[ "$CTEST_STATUS" == "passed" && "$DEVICE_LINK_STATUS" == "passed" && "$NATIVE_BENCH_STATUS" == "passed" ]]; then
    write_result "passed" "ctest=$CTEST_STATUS, device link=$DEVICE_LINK_STATUS, native bench=$NATIVE_BENCH_STATUS"
else
    write_result "partial" "ctest=$CTEST_STATUS, device link=$DEVICE_LINK_STATUS, native bench=$NATIVE_BENCH_STATUS"
fi
exit 0
