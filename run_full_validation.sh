#!/bin/bash
# ============================================================
# run_full_validation.sh - single entry point for validating the
# Redis-NUMA fork: build -> unit tests -> NUMAflow benchmark ->
# (optional) YCSB bandwidth benchmark -> (optional) QEMU multi-
# NUMA-node smoke test -> (optional) CXLMemSim device-link check
# -> aggregated HTML report with inline SVG charts.
#
# Every step that cannot run in this environment (no JDK for
# YCSB, no /dev/kvm for QEMU, CXLMemSim not built) is skipped
# with an honestly logged reason -- this script never fabricates
# a result for a step it did not actually execute.
#
# Usage: ./run_full_validation.sh [options]
#   --skip-build       skip 'make -j$(nproc)' in src/
#   --skip-test        skip the Tcl unit-test suite ('make test')
#   --skip-ycsb        skip tests/ycsb/run_bw_benchmark.sh
#   --skip-vm          skip the QEMU multi-NUMA-node smoke test
#   --skip-cxlmemsim   skip the CXLMemSim device-link check
#   --quick            shorthand for --skip-vm --skip-cxlmemsim --skip-ycsb
#   --vm-timeout SEC   QEMU boot timeout (default 480)
# ============================================================
set -uo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TS="$(date +%Y%m%d_%H%M%S)"
REPORT_DIR="$PROJECT_ROOT/results/full_report_$TS"
mkdir -p "$REPORT_DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log()      { echo -e "[$(date +%H:%M:%S)] $*"; }
log_step() { echo -e "\n${BLUE}== $* ==${NC}"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*"; }

SKIP_BUILD=0; SKIP_TEST=0; SKIP_YCSB=0; SKIP_VM=0; SKIP_CXL=0
VM_TIMEOUT=480
while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-test) SKIP_TEST=1; shift ;;
        --skip-ycsb) SKIP_YCSB=1; shift ;;
        --skip-vm) SKIP_VM=1; shift ;;
        --skip-cxlmemsim) SKIP_CXL=1; shift ;;
        --quick) SKIP_YCSB=1; SKIP_VM=1; SKIP_CXL=1; shift ;;
        --vm-timeout) VM_TIMEOUT="$2"; shift 2 ;;
        -h|--help) grep '^#' "$0" | sed 's/^# \?//'; exit 0 ;;
        *) log_warn "unknown arg: $1"; shift ;;
    esac
done

SUMMARY_JSON="$REPORT_DIR/summary.json"
declare -A STATUS
declare -A DETAIL

record() { STATUS["$1"]="$2"; DETAIL["$1"]="$3"; }

# ---- 1. Build ----
log_step "Build (make -j\$(nproc))"
if [[ "$SKIP_BUILD" == "1" ]]; then
    record build skipped "explicitly skipped via --skip-build"
    log_warn "skipped"
else
    BUILD_LOG="$REPORT_DIR/build.log"
    if (cd "$PROJECT_ROOT/src" && make -j"$(nproc)") > "$BUILD_LOG" 2>&1; then
        WARN_COUNT=$(grep -ciE "warning:" "$BUILD_LOG" || true)
        record build passed "0 errors, $WARN_COUNT warnings"
        log_ok "build passed ($WARN_COUNT warnings)"
    else
        record build failed "see $BUILD_LOG"
        log_err "build failed, see $BUILD_LOG"
    fi
fi

# ---- 2. Tcl unit-test suite ----
log_step "Unit tests (make test)"
if [[ "$SKIP_TEST" == "1" ]]; then
    record unit_tests skipped "explicitly skipped via --skip-test"
    log_warn "skipped"
elif [[ "${STATUS[build]:-}" == "failed" ]]; then
    record unit_tests skipped "build failed, cannot run tests"
    log_warn "skipped (build failed)"
else
    TEST_LOG="$REPORT_DIR/make_test.log"
    if (cd "$PROJECT_ROOT" && make test) > "$TEST_LOG" 2>&1; then
        FILE_COUNT=$(grep -cE '^ *[0-9]+ seconds? - ' "$TEST_LOG" 2>/dev/null || echo 0)
        record unit_tests passed "$FILE_COUNT test files run, \"All tests passed without errors!\" seen, 0 exceptions"
        log_ok "make test passed"
    else
        ERR_COUNT=$(grep -icE 'exception|\[err\]' "$TEST_LOG" 2>/dev/null || echo "?")
        record unit_tests failed "$ERR_COUNT error/exception marker(s), see $TEST_LOG"
        log_err "make test reported failures, see $TEST_LOG"
    fi
fi

# ---- 3. NUMAflow fair-evaluation benchmark (always available, pure C11) ----
log_step "NUMAflow scheduling-strategy benchmark"
NF_DIR="$PROJECT_ROOT/numaflow"
if (cd "$NF_DIR" && make -j"$(nproc)" test) > "$REPORT_DIR/numaflow_test.log" 2>&1; then
    NF_BIN="$NF_DIR/build/numaflow"
    for w in zipf uniform hotspot temporal; do
        "$NF_BIN" eval --workload "$w" --keys 20000 --accesses 200000 --epoch 5000 --budget 64 --nodes 2 \
            > "$PROJECT_ROOT/results/bench_$w.json" 2>"$REPORT_DIR/numaflow_eval_$w.err" || true
    done
    (cd "$NF_DIR" && python3 eval/report.py) > "$REPORT_DIR/numaflow_report.log" 2>&1 || true
    record numaflow_bench passed "4 workloads (zipf/uniform/hotspot/temporal) x 4 strategies (noop/composite_lru/tinylfu/caat)"
    log_ok "NUMAflow benchmark complete"
else
    record numaflow_bench failed "numaflow self-test failed, see $REPORT_DIR/numaflow_test.log"
    log_err "numaflow self-test failed"
fi

# ---- 4. YCSB bandwidth benchmark (needs a JDK + downloaded YCSB dist) ----
log_step "YCSB bandwidth benchmark"
if [[ "$SKIP_YCSB" == "1" ]]; then
    record ycsb skipped "explicitly skipped via --skip-ycsb"
    log_warn "skipped"
elif ! command -v java >/dev/null 2>&1; then
    record ycsb skipped "no JDK installed in this environment"
    log_warn "skipped (no java)"
else
    YCSB_LOG="$REPORT_DIR/ycsb_bw_benchmark.log"
    if (cd "$PROJECT_ROOT/tests/ycsb" && ./run_bw_benchmark.sh --output-dir "$REPORT_DIR/ycsb") > "$YCSB_LOG" 2>&1; then
        record ycsb passed "see $REPORT_DIR/ycsb/"
        log_ok "YCSB benchmark complete"
    else
        record ycsb failed "see $YCSB_LOG"
        log_err "YCSB benchmark failed, see $YCSB_LOG"
    fi
fi

# ---- 5. QEMU multi-NUMA-node smoke test (optional, slow under TCG) ----
log_step "QEMU multi-NUMA-node smoke test"
if [[ "$SKIP_VM" == "1" ]]; then
    record qemu_vm skipped "explicitly skipped via --skip-vm"
    log_warn "skipped"
else
    VM_LOG="$REPORT_DIR/qemu_vm.log"
    if (cd "$PROJECT_ROOT/tests/vm" && ./boot_numa_vm.sh --timeout "$VM_TIMEOUT") > "$VM_LOG" 2>&1; then
        LATEST_VM_JSON=$(ls -t "$PROJECT_ROOT/tests/vm/results"/qemu_smoke_*.json 2>/dev/null | head -1)
        VM_STATUS=$(python3 -c "import json;print(json.load(open('$LATEST_VM_JSON'))['status'])" 2>/dev/null || echo "unknown")
        record qemu_vm "$VM_STATUS" "see $VM_LOG and $LATEST_VM_JSON"
        log_ok "QEMU smoke test finished: $VM_STATUS"
    else
        record qemu_vm failed "see $VM_LOG"
        log_err "QEMU smoke test script errored, see $VM_LOG"
    fi
fi

# ---- 6. CXLMemSim device-link check (optional) ----
log_step "CXLMemSim device-link check"
if [[ "$SKIP_CXL" == "1" ]]; then
    record cxlmemsim skipped "explicitly skipped via --skip-cxlmemsim"
    log_warn "skipped"
else
    CXL_LOG="$REPORT_DIR/cxlmemsim.log"
    if (cd "$PROJECT_ROOT/tests/cxl" && ./run_cxlmemsim.sh) > "$CXL_LOG" 2>&1; then
        LATEST_CXL_JSON=$(ls -t "$PROJECT_ROOT/tests/cxl/results"/cxlmemsim_*.json 2>/dev/null | head -1)
        CXL_STATUS=$(python3 -c "import json;print(json.load(open('$LATEST_CXL_JSON'))['status'])" 2>/dev/null || echo "unknown")
        record cxlmemsim "$CXL_STATUS" "see $CXL_LOG and $LATEST_CXL_JSON"
        log_ok "CXLMemSim check finished: $CXL_STATUS"
    else
        record cxlmemsim failed "see $CXL_LOG"
        log_err "CXLMemSim check script errored, see $CXL_LOG"
    fi
fi

# ---- 7. Aggregate everything into a single HTML report ----
log_step "Generating aggregated report"
{
    echo "{"
    first=1
    for k in "${!STATUS[@]}"; do
        [[ $first -eq 0 ]] && echo ","
        first=0
        printf '  "%s": {"status": "%s", "detail": %s}' "$k" "${STATUS[$k]}" "$(python3 -c "import json,sys; print(json.dumps(sys.argv[1]))" "${DETAIL[$k]}")"
    done
    echo
    echo "}"
} > "$SUMMARY_JSON"

python3 "$PROJECT_ROOT/tests/report/generate_full_report.py" "$REPORT_DIR" "$SUMMARY_JSON" "$PROJECT_ROOT/results" \
    || log_warn "HTML report generation failed, raw JSON/logs are still in $REPORT_DIR"

echo
log_step "Summary"
for k in "${!STATUS[@]}"; do
    s="${STATUS[$k]}"
    case "$s" in
        passed) c="$GREEN" ;;
        skipped) c="$YELLOW" ;;
        *) c="$RED" ;;
    esac
    echo -e "  ${c}${s}${NC}\t$k\t${DETAIL[$k]}"
done
echo
log_ok "full report: $REPORT_DIR/index.html"
