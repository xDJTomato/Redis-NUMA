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
#
# Honest scope note: this validates the *device-emulation link*
# (QEMU <-> cxlmemsim_server), which is CXLMemSim's own stated
# scope -- a software timing model, not cycle-accurate hardware
# (see CXLMemSim's README). It does NOT run redis-server inside a
# fully-booted CXLMemSim guest -- wiring a guest OS through this
# same patched QEMU on top of an already-slow TCG host (see
# tests/vm/boot_numa_vm.sh) was judged out of scope for a single
# validation pass. For an actual Redis-level DRAM-vs-far-memory
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

# ---- 2. QEMU <-> cxlmemsim_server device link ----
if [[ ! -x "$QEMU_BIN" ]]; then
    log_warn "patched QEMU not built at $QEMU_BIN -- run external/CXLMemSim/script/build_qemu.sh"
    DEVICE_LINK_STATUS="skipped"
    write_result "partial" "ctest=$CTEST_STATUS, device link skipped (patched qemu not built)"
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
    write_result "partial" "ctest=$CTEST_STATUS, device link failed (server did not start)"
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

if [[ "$CTEST_STATUS" == "passed" && "$DEVICE_LINK_STATUS" == "passed" ]]; then
    write_result "passed" "ctest=$CTEST_STATUS, device link=$DEVICE_LINK_STATUS"
else
    write_result "partial" "ctest=$CTEST_STATUS, device link=$DEVICE_LINK_STATUS"
fi
exit 0
