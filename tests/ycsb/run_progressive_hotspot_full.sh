#!/usr/bin/env bash
# ============================================================================
# Redis-NUMA 一键远程渐进并发热点访问压力测试
#
# 在远程 NUMA 服务器上执行：
#   1. sync  — 同步 NUMA 源码、渐进测试脚本、workload 和绘图脚本并编译
#   2. bench — 运行 1M*4KB key + 4→128（步长4）线程热点访问测试
#   3. fetch — 下载结果到本地
#
# 用法:
#   ./run_progressive_hotspot_full.sh
#   ./run_progressive_hotspot_full.sh --step sync,bench,fetch
#   ./run_progressive_hotspot_full.sh --bench-args "--threads 4,16,32,64,128 --ops 20000"
# ============================================================================

set -euo pipefail

REMOTE_HOST="192.168.12.204"
REMOTE_USER="dell"
REMOTE_PASS="Dell@123"
SSH_PORT=22
REMOTE_NUMA_ROOT="~/lx/Redis-NUMA-main"
REMOTE_VANILLA_ROOT="~/lx/redis-6.2.21"
REMOTE_YCSB="${REMOTE_NUMA_ROOT}/tests/ycsb"
TEST_PORT=6409

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOCAL_RESULTS="${SCRIPT_DIR}/results"

STEPS="sync,bench,fetch"
VANILLA_MAX_MEMORY="8gb"
RUN_VANILLA=true
DEFAULT_BENCH_ARGS="--records 10000 --fieldlength 4096 --ops 500000 --threads 4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,100,104,108,112,116,120,124,128"
BENCH_ARGS="$DEFAULT_BENCH_ARGS"
DRY_RUN=false
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')

BLUE='\033[0;34m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
RED='\033[0;31m'; BOLD='\033[1m'; CYAN='\033[0;36m'; DIM='\033[2m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*" >&2; }
log_step() { echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; echo -e "${BOLD}${CYAN}  $*${NC}"; echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; }
log_sub()  { echo -e "  ${DIM}──${NC} $*"; }

usage() {
    cat <<'EOF'
用法: ./run_progressive_hotspot_full.sh [选项]

步骤选择:
  --step STEPS       逗号分隔步骤列表 (默认: sync,bench,fetch)
                     可选: sync, bench, fetch

测试选项:
  --skip-vanilla     只运行 Redis-NUMA，不运行原版 Redis 对比
  --bench-args ARGS  透传给 run_progressive_hotspot.sh
                     默认: "--records 1000000 --fieldlength 4096 --ops 100000 --threads 4,8,12,...,128"
                     例如 "--threads 4,16,32,64,128 --ops 20000"
  --port PORT        测试端口 (默认: 6409)

远程服务器:
  --host HOST        远程主机 (默认: 192.168.12.204)
  --user USER        SSH 用户 (默认: dell)
  --pass PASS        SSH 密码
  --remote-dir DIR   远程项目根 (默认: ~/lx/Redis-NUMA-main)

其他:
  --dry-run          只打印计划，不执行
  --help             显示帮助
EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --step) STEPS="$2"; shift 2 ;;
            --skip-vanilla) RUN_VANILLA=false; shift ;;
            --bench-args) BENCH_ARGS="$2"; shift 2 ;;
            --port) TEST_PORT="$2"; shift 2 ;;
            --host) REMOTE_HOST="$2"; shift 2 ;;
            --user) REMOTE_USER="$2"; shift 2 ;;
            --pass) REMOTE_PASS="$2"; shift 2 ;;
            --remote-dir) REMOTE_NUMA_ROOT="$2"; REMOTE_YCSB="${REMOTE_NUMA_ROOT}/tests/ycsb"; shift 2 ;;
            --dry-run) DRY_RUN=true; shift ;;
            --help|-h) usage ;;
            *) log_err "未知参数: $1"; usage ;;
        esac
    done
}

_ssh() {
    sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p "$SSH_PORT" "${REMOTE_USER}@${REMOTE_HOST}" "$@"
}

_scp() {
    sshpass -p "$REMOTE_PASS" scp -P "$SSH_PORT" -o StrictHostKeyChecking=no "$@"
}

step_enabled() {
    [[ ",$STEPS," == *",$1,"* ]]
}

do_sync() {
    log_step "Step: sync — 同步代码、脚本并编译"

    log_sub "同步渐进测试脚本和 workload..."
    _ssh "mkdir -p ${REMOTE_YCSB}/scripts ${REMOTE_YCSB}/workloads ${REMOTE_YCSB}/results"
    _scp "${SCRIPT_DIR}/run_progressive_hotspot.sh" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/run_progressive_hotspot.sh"
    _scp "${SCRIPT_DIR}/workloads/workload_progressive_hotspot" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/workloads/workload_progressive_hotspot"
    _scp "${SCRIPT_DIR}/scripts/visualize_progressive_hotspot.py" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/scripts/visualize_progressive_hotspot.py"
    _ssh "chmod +x ${REMOTE_YCSB}/run_progressive_hotspot.sh ${REMOTE_YCSB}/scripts/visualize_progressive_hotspot.py"
    log_ok "渐进测试文件已同步"

    log_sub "同步 NUMA 源码并编译..."
    local src_dir="${PROJECT_ROOT}/src"
    for f in "$src_dir"/*.c "$src_dir"/*.h "$src_dir"/Makefile; do
        _scp "$f" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_NUMA_ROOT}/src/" 2>/dev/null
    done
    log_ok "NUMA 源码已同步并编译"

    if [[ "$RUN_VANILLA" == "true" ]]; then
        log_sub "检查远程 vanilla Redis..."
        local remote_ver
        remote_ver=$(_ssh "${REMOTE_VANILLA_ROOT}/src/redis-server --version 2>/dev/null | head -1" 2>/dev/null || true)
        if [[ "$remote_ver" == *"v=6.2.21"* ]]; then
            log_ok "远程已有 vanilla Redis: $remote_ver"
        else
            log_err "远程 vanilla Redis 不可用: ${REMOTE_VANILLA_ROOT}/src/redis-server"
            return 1
        fi
    fi
}

do_bench() {
    log_step "Step: bench — 渐进并发热点访问测试"
    log "NUMA 参数: --port $TEST_PORT --numa-strategy interleaved $BENCH_ARGS"
    _ssh "cd ${REMOTE_YCSB} && bash run_progressive_hotspot.sh --variant numa --port $TEST_PORT --numa-strategy interleaved $BENCH_ARGS" || {
        log_warn "NUMA 渐进测试返回非零，继续"
    }
    log_ok "NUMA 渐进测试完成"

    if [[ "$RUN_VANILLA" == "true" ]]; then
        log "vanilla (local) 参数: --port $((TEST_PORT+1)) --maxmem $VANILLA_MAX_MEMORY --vanilla-cpu-node 0 --vanilla-mem-node 0 $BENCH_ARGS"
        _ssh "cd ${REMOTE_YCSB} && VANILLA_REDIS_ROOT=${REMOTE_VANILLA_ROOT} bash run_progressive_hotspot.sh --variant vanilla --port $((TEST_PORT+1)) --maxmem $VANILLA_MAX_MEMORY --vanilla-cpu-node 0 --vanilla-mem-node 0 $BENCH_ARGS" || {
            log_warn "vanilla (local) 渐进测试返回非零，继续"
        }
        log_ok "vanilla (local) 渐进测试完成"

        log "vanilla (interleaved) 参数: --port $((TEST_PORT+2)) --maxmem $VANILLA_MAX_MEMORY --vanilla-cpu-node 0 --vanilla-mem-node 0,2 --vanilla-mem-policy interleave $BENCH_ARGS"
        _ssh "cd ${REMOTE_YCSB} && VANILLA_REDIS_ROOT=${REMOTE_VANILLA_ROOT} bash run_progressive_hotspot.sh --variant vanilla --port $((TEST_PORT+2)) --maxmem $VANILLA_MAX_MEMORY --vanilla-cpu-node 0 --vanilla-mem-node 0,2 --vanilla-mem-policy interleave $BENCH_ARGS" || {
            log_warn "vanilla (interleaved) 渐进测试返回非零，继续"
        }
        log_ok "vanilla (interleaved) 渐进测试完成"
    fi

    log "NUMA TinyLFU 参数: --variant numa --tinylfu --port $((TEST_PORT+3)) --numa-strategy interleaved $BENCH_ARGS"
    _ssh "cd ${REMOTE_YCSB} && bash run_progressive_hotspot.sh --variant numa --tinylfu --port $((TEST_PORT+3)) --numa-strategy interleaved --output-dir ${REMOTE_YCSB}/results/progressive_hotspot_tinylfu_${TIMESTAMP} $BENCH_ARGS" || {
        log_warn "NUMA TinyLFU 渐进测试返回非零，继续"
    }
    log_ok "NUMA TinyLFU 渐进测试完成"
}

do_fetch() {
    log_step "Step: fetch — 下载结果到本地"
    local fetch_dir="${LOCAL_RESULTS}/progressive_hotspot_full_${TIMESTAMP}"
    mkdir -p "$fetch_dir"

    local latest_numa
    latest_numa=$(_ssh "ls -1 ${REMOTE_YCSB}/results/ 2>/dev/null | grep -E '^progressive_hotspot_numa_[0-9]{8}_[0-9]{6}$' | sort | tail -1" || true)
    if [[ -n "$latest_numa" ]]; then
        mkdir -p "$fetch_dir/numa"
        _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/${latest_numa}/." "$fetch_dir/numa/"
        log_ok "NUMA 结果已下载: $fetch_dir/numa"
    else
        log_warn "远程无 NUMA progressive_hotspot 结果"
    fi

    if [[ "$RUN_VANILLA" == "true" ]]; then
        local vanilla_dirs
        vanilla_dirs=$(_ssh "ls -1d ${REMOTE_YCSB}/results/progressive_hotspot_vanilla_* 2>/dev/null | sort" || true)
        local dir_array
        readarray -t dir_array <<< "$vanilla_dirs"
        local n=${#dir_array[@]}

        if (( n >= 2 )); then
            mkdir -p "$fetch_dir/vanilla_local" "$fetch_dir/vanilla_interleaved"
            _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${dir_array[$((n-2))]}/." "$fetch_dir/vanilla_local/"
            log_ok "vanilla (local) 结果已下载"
            _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${dir_array[$((n-1))]}/." "$fetch_dir/vanilla_interleaved/"
            log_ok "vanilla (interleaved) 结果已下载"
        elif (( n >= 1 )); then
            mkdir -p "$fetch_dir/vanilla"
            _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${dir_array[$((n-1))]}/." "$fetch_dir/vanilla/"
            log_ok "vanilla 结果已下载 (仅 $n 组)"
        else
            log_warn "远程无 vanilla progressive_hotspot 结果"
        fi
    fi

    local tinylfu_dir="progressive_hotspot_tinylfu_${TIMESTAMP}"
    local tinylfu_exists
    tinylfu_exists=$(_ssh "test -d ${REMOTE_YCSB}/results/${tinylfu_dir} && echo yes || echo no" 2>/dev/null || echo "no")
    if [[ "$tinylfu_exists" == "yes" ]]; then
        mkdir -p "$fetch_dir/numa_tinylfu"
        _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/${tinylfu_dir}/." "$fetch_dir/numa_tinylfu/"
        log_ok "NUMA TinyLFU 结果已下载: $fetch_dir/numa_tinylfu"
    else
        local latest_tinylfu
        latest_tinylfu=$(_ssh "ls -1 ${REMOTE_YCSB}/results/ 2>/dev/null | grep -E '^progressive_hotspot_tinylfu_' | sort | tail -1" || true)
        if [[ -n "$latest_tinylfu" ]]; then
            mkdir -p "$fetch_dir/numa_tinylfu"
            _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/${latest_tinylfu}/." "$fetch_dir/numa_tinylfu/"
            log_ok "NUMA TinyLFU 结果已下载 (fallback): $fetch_dir/numa_tinylfu"
        else
            log_warn "远程无 TinyLFU progressive_hotspot 结果"
        fi
    fi

    generate_summary "$fetch_dir"
    generate_compare_report "$fetch_dir"
}

generate_summary() {
    local dir="$1"
    local summary="$dir/SUMMARY.txt"
    {
        echo "Redis-NUMA / vanilla 渐进并发热点访问测试摘要"
        echo "=============================================="
        echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "远程: ${REMOTE_USER}@${REMOTE_HOST}"
        echo "NUMA 端口: ${TEST_PORT}"
        [[ "$RUN_VANILLA" == "true" ]] && echo "vanilla 端口: $((TEST_PORT+1)) (local), $((TEST_PORT+2)) (interleaved)"
        echo ""
        for variant in numa vanilla_local vanilla_interleaved vanilla; do
            local csv="$dir/$variant/progressive_summary.csv"
            if [[ -f "$csv" ]]; then
                echo "── ${variant} 线程扩展结果 ──"
                column -s, -t "$csv" 2>/dev/null || cat "$csv"
                echo ""
            fi
        done
        echo "── 文件列表 ──"
        find "$dir" -type f \( -name '*.txt' -o -name '*.csv' -o -name '*.png' \) | sort | sed "s|^${dir}/|  |"
    } > "$summary"
    log_ok "摘要已生成: $summary"
}

generate_compare_report() {
    local dir="$1"
    local numa_csv="$dir/numa/progressive_summary.csv"
    local local_csv="$dir/vanilla_local/progressive_summary.csv"
    local interleaved_csv="$dir/vanilla_interleaved/progressive_summary.csv"
    local vanilla_csv="$dir/vanilla/progressive_summary.csv"
    local tinylfu_csv="$dir/numa_tinylfu/progressive_summary.csv"
    local output="$dir/progressive_hotspot_compare.png"

    [[ -f "$numa_csv" ]] || return 0
    if ! command -v python3 &>/dev/null; then
        log_warn "python3 不可用，跳过本地叠加绘图"
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
            python3 -m venv "$venv_dir" || { log_warn "创建 venv 失败，跳过叠加绘图"; return 0; }
        fi
        "$venv_dir/bin/pip" install --quiet matplotlib pandas || { log_warn "安装绘图依赖失败，跳过叠加绘图"; return 0; }
        python="$venv_dir/bin/python"
    fi

    local plot_args=(
        "$SCRIPT_DIR/scripts/visualize_progressive_hotspot.py"
        --input "$numa_csv"
        --label "Redis-NUMA (Composite LRU)"
    )

    if [[ -f "$local_csv" && -f "$interleaved_csv" ]]; then
        plot_args+=(
            --compare-input "$local_csv"
            --compare-label "Vanilla Redis (local)"
            --compare-input2 "$interleaved_csv"
            --compare-label2 "Vanilla Redis (interleaved)"
        )
    elif [[ -f "$vanilla_csv" ]]; then
        plot_args+=(
            --compare-input "$vanilla_csv"
            --compare-label "Vanilla Redis"
        )
    fi

    if [[ -f "$tinylfu_csv" ]]; then
        plot_args+=(
            --compare-input3 "$tinylfu_csv"
            --compare-label3 "Redis-NUMA (TinyLFU)"
        )
    fi

    plot_args+=(
        --output "$output"
        --title "Concurrency Scaling under YCSB Hotspot Workload (10K Keys × 4 KiB)"
    )

    "$python" "${plot_args[@]}" 2>&1 || log_warn "叠加绘图失败，请查看 CSV 文件"
    [[ -f "$output" ]] && log_ok "叠加图表已生成: $output"
}

main() {
    parse_args "$@"

    echo -e "${BOLD}${CYAN}"
    echo "╔══════════════════════════════════════════════════════════════╗"
    echo "║          Redis-NUMA 一键渐进并发热点访问测试                  ║"
    echo "║   远程: ${REMOTE_USER}@${REMOTE_HOST}                        ║"
    echo "║   端口: ${TEST_PORT}  时间: ${TIMESTAMP}                     ║"
    echo "╚══════════════════════════════════════════════════════════════╝"
    echo -e "${NC}"

    echo -e "${BOLD}执行计划:${NC}"
    for s in sync bench fetch; do
        if step_enabled "$s"; then
            case "$s" in
                sync) echo -e "  ${GREEN}✓${NC} sync  — 同步代码、脚本并编译" ;;
                bench) echo -e "  ${GREEN}✓${NC} bench — Redis-NUMA + vanilla 1M*4KB + 4→128(步长4) 线程热点访问" ;;
                fetch) echo -e "  ${GREEN}✓${NC} fetch — 下载结果和图表" ;;
            esac
        else
            echo -e "  ${DIM}○ $s — 跳过${NC}"
        fi
    done
    echo ""

    if [[ "$DRY_RUN" == "true" ]]; then
        log_warn "dry-run 模式，不执行任何操作"
        exit 0
    fi
    command -v sshpass &>/dev/null || { log_err "sshpass 未安装，请执行: sudo apt install -y sshpass"; exit 1; }
    log "测试 SSH 连接..."
    _ssh "echo ok" &>/dev/null || { log_err "SSH 连接失败: ${REMOTE_USER}@${REMOTE_HOST}"; exit 1; }
    log_ok "SSH 连接正常"

    step_enabled sync && do_sync
    step_enabled bench && do_bench
    step_enabled fetch && do_fetch

    log_ok "全部步骤完成"
}

main "$@"
