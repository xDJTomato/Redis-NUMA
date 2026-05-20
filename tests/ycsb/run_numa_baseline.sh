#!/usr/bin/env bash
# ============================================================================
# NUMA 基准测试：本地节点 vs 远端节点 vs interleaved
#
# 在远程 NUMA 服务器上运行 Vanilla Redis，分别绑定到：
#   - 本地内存 (CPU=Node0, mem=Node0, NUMA distance=10)
#   - 远端内存 (CPU=Node0, mem=Node2/CXL, NUMA distance=17)
#   - interleaved (CPU=Node0, interleave=Node0,Node2)
# 使用与 progressive hotspot 相同的 YCSB 渐进线程负载。
#
# 用法:
#   ./run_numa_baseline.sh
#   ./run_numa_baseline.sh --step sync,bench,fetch
# ============================================================================

set -euo pipefail

REMOTE_HOST="192.168.12.204"
REMOTE_USER="dell"
REMOTE_PASS="Dell@123"
SSH_PORT=22
REMOTE_VANILLA_ROOT="~/lx/redis-6.2.21"
REMOTE_NUMA_ROOT="~/lx/Redis-NUMA-main"
REMOTE_YCSB="${REMOTE_NUMA_ROOT}/tests/ycsb"

PORT_LOCAL=6410
PORT_INTERLEAVE=6412
MAX_MEMORY="8gb"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOCAL_RESULTS="${SCRIPT_DIR}/results"

STEPS="sync,bench,fetch"
BENCH_ARGS="--records 1000000 --fieldlength 4096 --ops 100000 --threads 4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,100,104,108,112,116,120,124,128"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')

BLUE='\033[0;34m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
RED='\033[0;31m'; BOLD='\033[1m'; CYAN='\033[0;36m'; DIM='\033[2m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*" >&2; }
log_step() { echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; echo -e "${BOLD}${CYAN}  $*${NC}"; echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; }

_ssh() {
    sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p "$SSH_PORT" "${REMOTE_USER}@${REMOTE_HOST}" "$@"
}

_scp() {
    sshpass -p "$REMOTE_PASS" scp -P "$SSH_PORT" -o StrictHostKeyChecking=no "$@"
}

step_enabled() {
    [[ ",$STEPS," == *",$1,"* ]]
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --step) STEPS="$2"; shift 2 ;;
            --bench-args) BENCH_ARGS="$2"; shift 2 ;;
            --host) REMOTE_HOST="$2"; shift 2 ;;
            --help|-h)
                echo "用法: ./run_numa_baseline.sh [--step sync,bench,fetch] [--bench-args ARGS]"
                exit 0 ;;
            *) log_err "未知参数: $1"; exit 1 ;;
        esac
    done
}

do_sync() {
    log_step "Step: sync — 同步脚本到远程"

    _ssh "mkdir -p ${REMOTE_YCSB}/scripts ${REMOTE_YCSB}/workloads ${REMOTE_YCSB}/results"
    _scp "${SCRIPT_DIR}/run_progressive_hotspot.sh" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/run_progressive_hotspot.sh"
    _scp "${SCRIPT_DIR}/workloads/workload_progressive_hotspot" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/workloads/workload_progressive_hotspot"
    _scp "${SCRIPT_DIR}/scripts/visualize_progressive_hotspot.py" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/scripts/visualize_progressive_hotspot.py"
    _ssh "chmod +x ${REMOTE_YCSB}/run_progressive_hotspot.sh"
    log_ok "脚本已同步"

    local remote_ver
    remote_ver=$(_ssh "${REMOTE_VANILLA_ROOT}/src/redis-server --version 2>/dev/null | head -1" 2>/dev/null || true)
    if [[ "$remote_ver" == *"v=6.2"* ]]; then
        log_ok "远程 vanilla Redis 可用: $remote_ver"
    else
        log_err "远程 vanilla Redis 不可用: ${REMOTE_VANILLA_ROOT}/src/redis-server"
        return 1
    fi
}

do_bench() {
    log_step "Step: bench — NUMA 基准测试 (local vs interleaved)"

    log "1/2 Local 测试: CPU=Node0, membind=Node0 (port $PORT_LOCAL)"
    _ssh "cd ${REMOTE_YCSB} && VANILLA_REDIS_ROOT=${REMOTE_VANILLA_ROOT} bash run_progressive_hotspot.sh \
        --variant vanilla \
        --vanilla-cpu-node 0 --vanilla-mem-node 0 \
        --port $PORT_LOCAL --maxmem $MAX_MEMORY \
        $BENCH_ARGS" || {
        log_warn "local 测试返回非零，继续"
    }
    log_ok "Local 测试完成"

    log "2/2 Interleaved 测试: CPU=Node0, interleave=Node0,2 (port $PORT_INTERLEAVE)"
    _ssh "cd ${REMOTE_YCSB} && VANILLA_REDIS_ROOT=${REMOTE_VANILLA_ROOT} bash run_progressive_hotspot.sh \
        --variant vanilla \
        --vanilla-cpu-node 0 --vanilla-mem-node 0,2 --vanilla-mem-policy interleave \
        --port $PORT_INTERLEAVE --maxmem $MAX_MEMORY \
        $BENCH_ARGS" || {
        log_warn "interleaved 测试返回非零，继续"
    }
    log_ok "Interleaved 测试完成"
}

do_fetch() {
    log_step "Step: fetch — 下载结果到本地"
    local fetch_dir="${LOCAL_RESULTS}/numa_baseline_${TIMESTAMP}"
    mkdir -p "$fetch_dir/local" "$fetch_dir/interleaved"

    local dirs
    dirs=$(_ssh "ls -1d ${REMOTE_YCSB}/results/progressive_hotspot_vanilla_* 2>/dev/null | sort" || true)
    local dir_array
    readarray -t dir_array <<< "$dirs"
    local n=${#dir_array[@]}

    if (( n >= 2 )); then
        _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${dir_array[$((n-2))]}/." "$fetch_dir/local/"
        log_ok "Local 结果已下载"
        _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${dir_array[$((n-1))]}/." "$fetch_dir/interleaved/"
        log_ok "Interleaved 结果已下载"
    else
        log_err "远程结果不足 2 组 (found $n)"
        return 1
    fi

    generate_summary "$fetch_dir"
    generate_plot "$fetch_dir"
}

generate_summary() {
    local dir="$1"
    local summary="$dir/SUMMARY.txt"
    {
        echo "NUMA 基准测试：本地 vs interleaved"
        echo "============================================"
        echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "远程: ${REMOTE_USER}@${REMOTE_HOST}"
        echo "Local:       CPU=Node0, membind=Node0 (NUMA distance=10)"
        echo "Interleaved: CPU=Node0, interleave=Node0,2"
        echo "负载: 1M×4KiB, 100K ops, hotspot(5%/95%), 4→128 线程"
        echo ""
        for variant in local interleaved; do
            local csv="$dir/$variant/progressive_summary.csv"
            if [[ -f "$csv" ]]; then
                echo "── ${variant} ──"
                column -s, -t "$csv" 2>/dev/null || cat "$csv"
                echo ""
            fi
        done
    } > "$summary"
    log_ok "摘要已生成: $summary"
}

generate_plot() {
    local dir="$1"
    local local_csv="$dir/local/progressive_summary.csv"
    local interleaved_csv="$dir/interleaved/progressive_summary.csv"
    local output="$dir/numa_baseline_compare.png"

    [[ -f "$local_csv" && -f "$interleaved_csv" ]] || { log_warn "CSV 文件缺失，跳过绘图"; return 0; }

    if ! command -v python3 &>/dev/null; then
        log_warn "python3 不可用，跳过绘图"
        return 0
    fi

    local venv_dir="$SCRIPT_DIR/scripts/.venv"
    local python="$venv_dir/bin/python"
    if [[ -x "$python" ]] && "$python" -c "import matplotlib" 2>/dev/null; then
        :
    elif python3 -c "import matplotlib" 2>/dev/null; then
        python="python3"
    else
        if [[ ! -x "$python" ]]; then
            python3 -m venv "$venv_dir" || { log_warn "创建 venv 失败"; return 0; }
        fi
        "$venv_dir/bin/pip" install --quiet matplotlib pandas || { log_warn "安装绘图依赖失败"; return 0; }
        python="$venv_dir/bin/python"
    fi

    "$python" "$SCRIPT_DIR/scripts/visualize_progressive_hotspot.py" \
        --input "$local_csv" \
        --label "Vanilla Redis (local)" \
        --compare-input "$interleaved_csv" \
        --compare-label "Vanilla Redis (interleaved)" \
        --output "$output" \
        --title "NUMA Baseline: Local vs. Interleaved (1M x 4KiB)" \
        2>&1 || log_warn "绘图失败"
    [[ -f "${output%.*}_throughput.${output##*.}" ]] && log_ok "图表已生成: $dir/numa_baseline_compare_*.png"
}

main() {
    parse_args "$@"

    echo -e "${BOLD}${CYAN}"
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║   NUMA 基准测试：local vs interleaved                    ║"
    echo "║   远程: ${REMOTE_USER}@${REMOTE_HOST}                        ║"
    echo "║   时间: ${TIMESTAMP}                                         ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"

    command -v sshpass &>/dev/null || { log_err "sshpass 未安装"; exit 1; }
    log "测试 SSH 连接..."
    _ssh "echo ok" &>/dev/null || { log_err "SSH 连接失败"; exit 1; }
    log_ok "SSH 连接正常"

    step_enabled sync && do_sync
    step_enabled bench && do_bench
    step_enabled fetch && do_fetch

    log_ok "全部步骤完成"
}

main "$@"
