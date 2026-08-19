#!/usr/bin/env bash
# ============================================================================
# NUMA 迁移策略全量对比测试
#
# 用途: 依次跑通 5 组单一变量对比 —— Vanilla Redis 7.2.6 / Redis-NUMA(noop 基线,
#       即禁用迁移) / Redis-NUMA(Composite LRU, 旧默认策略) / Redis-NUMA(TinyLFU)
#       / Redis-NUMA(CAAT, NUMAflow 新默认策略) —— 复用 run_bw_benchmark.sh /
#       run_bw_benchmark_vanilla.sh 的三阶段负载 (Fill → Hotspot → Sustain)，
#       再用旧的 visualize_bw_benchmark.py 画图方案一次性画出 5 组对比图。
#
# 输出: 默认写到仓库最外层的 Results/algorithms_<timestamp>/，不再写入
#       tests/ycsb/results/。
#
# 用法: ./run_algorithm_comparison.sh [选项]
# 选项:
#   --maxmem MEM         每个实例的最大内存 (默认: 8gb，五组一致)
#   --output-dir DIR     结果根目录 (默认: <repo>/Results/algorithms_<timestamp>)
#   --process-nodes NODES Redis-NUMA 进程可用 NUMA 节点 (默认: all)
#   --only <name,...>     只跑指定的组：vanilla,noop,composite_lru,tinylfu,caat
#   --help                显示帮助
# ============================================================================

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
VISUALIZE_SCRIPT="$SCRIPT_DIR/scripts/visualize_bw_benchmark.py"
VENV_DIR="$SCRIPT_DIR/scripts/.venv"

MAX_MEMORY="8gb"
OUTPUT_ROOT=""
PROCESS_NODES="all"
RUN_SET="vanilla,noop,composite_lru,tinylfu,caat"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*"; }
log_step() { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --maxmem MEM          每个实例的最大内存 (默认: 8gb，五组一致)
  --output-dir DIR      结果根目录 (默认: <repo>/Results/algorithms_<timestamp>)
  --process-nodes NODES Redis-NUMA 进程可用 NUMA 节点 (默认: all)
  --only NAMES          只跑指定的组，逗号分隔 (默认: vanilla,noop,composite_lru,tinylfu,caat)
  --help                显示此帮助

五组对比:
  vanilla        原版 Redis 7.2.6（jemalloc，无 NUMA 模块）
  noop           Redis-NUMA，禁用迁移 (Slot 1/2 均关闭，且不加载 NUMAflow) —— NUMA 分配层单独的基线
  composite_lru  Redis-NUMA，旧默认迁移策略 (Slot 1: Composite LRU)
  tinylfu        Redis-NUMA，TinyLFU 迁移策略 (Slot 2)
  caat           Redis-NUMA，NUMAflow 的 CAAT 策略（成本感知分层，本仓库新默认）
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --maxmem) MAX_MEMORY="$2"; shift 2 ;;
        --output-dir) OUTPUT_ROOT="$2"; shift 2 ;;
        --process-nodes) PROCESS_NODES="$2"; shift 2 ;;
        --only) RUN_SET="$2"; shift 2 ;;
        --help|-h) usage ;;
        *) log_err "未知参数: $1"; usage ;;
    esac
done

[[ -z "$OUTPUT_ROOT" ]] && OUTPUT_ROOT="$PROJECT_ROOT/Results/algorithms_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUTPUT_ROOT"

should_run() { [[ ",$RUN_SET," == *",$1,"* ]]; }

echo -e "${BOLD}${CYAN}"
echo "╔════════════════════════════════════════════════════════════╗"
echo "║   NUMA 迁移策略全量对比 (vanilla / noop / composite_lru /   ║"
echo "║   tinylfu / caat) —— tests/ycsb 三阶段负载                   ║"
echo "╚════════════════════════════════════════════════════════════╝"
echo -e "${NC}"
log "结果根目录: $OUTPUT_ROOT"
log "最大内存 (五组一致): $MAX_MEMORY"
log "运行组: $RUN_SET"

declare -A DIRS=(
    [vanilla]="$OUTPUT_ROOT/vanilla"
    [noop]="$OUTPUT_ROOT/noop"
    [composite_lru]="$OUTPUT_ROOT/composite_lru"
    [tinylfu]="$OUTPUT_ROOT/tinylfu"
    [caat]="$OUTPUT_ROOT/caat"
)
declare -A OK=()

run_one() {
    local name="$1"; shift
    log_step "运行: $name"
    if "$@"; then
        OK[$name]=1
        log_ok "$name 完成"
    else
        OK[$name]=0
        log_err "$name 失败，跳过（比较图中将省略该组）"
    fi
}

if should_run vanilla; then
    run_one vanilla "$SCRIPT_DIR/run_bw_benchmark_vanilla.sh" \
        --maxmem "$MAX_MEMORY" --output-dir "${DIRS[vanilla]}"
fi

if should_run composite_lru; then
    run_one composite_lru "$SCRIPT_DIR/run_bw_benchmark.sh" \
        --maxmem "$MAX_MEMORY" --output-dir "${DIRS[composite_lru]}" --process-nodes "$PROCESS_NODES"
fi

if should_run noop; then
    run_one noop "$SCRIPT_DIR/run_bw_benchmark.sh" \
        --maxmem "$MAX_MEMORY" --output-dir "${DIRS[noop]}" --process-nodes "$PROCESS_NODES" --no-migrate
fi

if should_run tinylfu; then
    run_one tinylfu "$SCRIPT_DIR/run_bw_benchmark.sh" \
        --maxmem "$MAX_MEMORY" --output-dir "${DIRS[tinylfu]}" --process-nodes "$PROCESS_NODES" --tinylfu
fi

if should_run caat; then
    run_one caat "$SCRIPT_DIR/run_bw_benchmark.sh" \
        --maxmem "$MAX_MEMORY" --output-dir "${DIRS[caat]}" --process-nodes "$PROCESS_NODES" --caat
fi

# ── 汇总画图（复用旧的 visualize_bw_benchmark.py 方案） ────────────────────
log_step "生成对比报告"

PYTHON="$VENV_DIR/bin/python"
if [[ ! -x "$PYTHON" ]]; then
    log "创建 Python 虚拟环境..."
    python3 -m venv "$VENV_DIR" 2>/dev/null || log_warn "创建 venv 失败"
fi
if [[ -x "$PYTHON" ]] && ! "$PYTHON" -c "import matplotlib, pandas" 2>/dev/null; then
    "$VENV_DIR/bin/pip" install --quiet matplotlib pandas || log_warn "依赖安装失败"
fi

declare -a VIZ_ARGS=()
PRIMARY_SET=0
add_dataset() {
    local key="$1" label="$2"
    [[ "${OK[$key]:-0}" != "1" ]] && return
    local dir="${DIRS[$key]}"
    [[ -f "$dir/metrics.csv" ]] || return
    if [[ $PRIMARY_SET -eq 0 ]]; then
        VIZ_ARGS+=(--input "$dir/metrics.csv" --label "$label" --phase-dir "$dir")
        PRIMARY_SET=1
    elif [[ ${#VIZ_ARGS[@]} -le 6 ]]; then
        VIZ_ARGS+=(--compare-input "$dir/metrics.csv" --compare-label "$label" --compare-phase-dir "$dir")
    elif [[ ${#VIZ_ARGS[@]} -le 12 ]]; then
        VIZ_ARGS+=(--compare-input2 "$dir/metrics.csv" --compare-label2 "$label" --compare-phase-dir2 "$dir")
    elif [[ ${#VIZ_ARGS[@]} -le 18 ]]; then
        VIZ_ARGS+=(--compare-input3 "$dir/metrics.csv" --compare-label3 "$label" --compare-phase-dir3 "$dir")
    else
        VIZ_ARGS+=(--compare-input4 "$dir/metrics.csv" --compare-label4 "$label" --compare-phase-dir4 "$dir")
    fi
}

# 顺序即图例/柱状图顺序：先默认策略，再 vanilla 基线，再另外三个策略变体。
add_dataset composite_lru "Redis-NUMA (Composite LRU)"
add_dataset vanilla "Vanilla Redis"
add_dataset noop "Redis-NUMA (Noop)"
add_dataset tinylfu "Redis-NUMA (TinyLFU)"
add_dataset caat "Redis-NUMA (CAAT)"

if [[ $PRIMARY_SET -eq 0 ]]; then
    log_err "没有任何一组产出 metrics.csv，无法生成对比报告"
else
    if [[ -x "$PYTHON" ]]; then
        "$PYTHON" "$VISUALIZE_SCRIPT" "${VIZ_ARGS[@]}" \
            --title "NUMA Migration Strategy Comparison — 3-Phase YCSB Bandwidth Benchmark" \
            --output "$OUTPUT_ROOT/comparison_report.png" \
            2>&1 || log_warn "可视化失败，请查看各组 metrics.csv"
        [[ -f "$OUTPUT_ROOT/comparison_report.png" ]] && log_ok "对比报告: $OUTPUT_ROOT/comparison_report.png"
        [[ -f "$OUTPUT_ROOT/comparison_report_latency.png" ]] && log_ok "延迟对比: $OUTPUT_ROOT/comparison_report_latency.png"
    else
        log_warn "python3/venv 不可用，跳过画图，原始数据在各组目录的 metrics.csv"
    fi
fi

# ── 文本摘要 ────────────────────────────────────────────────────────────────
SUMMARY="$OUTPUT_ROOT/summary.txt"
{
    echo "NUMA 迁移策略全量对比 - 测试摘要"
    echo "生成时间: $(date)"
    echo "最大内存: $MAX_MEMORY (五组一致)"
    echo ""
    for key in composite_lru vanilla noop tinylfu caat; do
        [[ "${OK[$key]:-0}" != "1" ]] && continue
        dir="${DIRS[$key]}"
        echo "=== $key ($dir) ==="
        for f in phase1_load.txt phase2_hotspot.txt phase3_sustain.txt; do
            [[ -f "$dir/$f" ]] || continue
            echo "-- $f --"
            grep -E 'OVERALL.*Throughput|^\[READ\].*AverageLatency|^\[UPDATE\].*AverageLatency' "$dir/$f" 2>/dev/null || true
        done
        echo ""
    done
} > "$SUMMARY"
log_ok "文本摘要: $SUMMARY"

log_step "完成"
for key in vanilla noop composite_lru tinylfu caat; do
    if [[ "${OK[$key]:-}" == "1" ]]; then
        log_ok "$key: 成功 (${DIRS[$key]})"
    elif [[ -n "${OK[$key]:-}" ]]; then
        log_err "$key: 失败"
    else
        log_warn "$key: 未运行 (--only 未包含)"
    fi
done
log "结果根目录: $OUTPUT_ROOT"
