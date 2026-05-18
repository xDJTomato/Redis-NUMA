#!/usr/bin/env bash
# ============================================================================
# Redis-NUMA 渐进热点访问 NUMA 消融测试
#
# 负载：10000 * 1MiB key，线程：4..64，Redis-NUMA maxmemory=16GB
# 远程依次运行：baseline / no-locality-stats / no-access-tracking / no-auto-migrate
# ============================================================================

set -euo pipefail

REMOTE_HOST="192.168.12.204"
REMOTE_USER="dell"
REMOTE_PASS="Dell@123"
SSH_PORT=22
REMOTE_NUMA_ROOT="~/lx/Redis-NUMA-main"
REMOTE_YCSB="${REMOTE_NUMA_ROOT}/tests/ycsb"
TEST_PORT=6411

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOCAL_RESULTS="${SCRIPT_DIR}/results"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
THREADS="4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64"
COMMON_ARGS="--threads ${THREADS} --records 10000 --fieldlength 1048576 --ops 10000 --maxmem 16gb"

BLUE='\033[0;34m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; CYAN='\033[0;36m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*" >&2; }
log_step() { echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; echo -e "${BOLD}${CYAN}  $*${NC}"; echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; }

usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --port PORT        起始 Redis 端口 (默认: 6411)
  --host HOST        远程主机 (默认: 192.168.12.204)
  --user USER        SSH 用户 (默认: dell)
  --pass PASS        SSH 密码
  --remote-dir DIR   远程项目根 (默认: ~/lx/Redis-NUMA-main)
  --threads LIST     线程列表 (默认: ${THREADS})
  --help             显示帮助
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) TEST_PORT="$2"; shift 2 ;;
        --host) REMOTE_HOST="$2"; shift 2 ;;
        --user) REMOTE_USER="$2"; shift 2 ;;
        --pass) REMOTE_PASS="$2"; shift 2 ;;
        --remote-dir) REMOTE_NUMA_ROOT="$2"; REMOTE_YCSB="${REMOTE_NUMA_ROOT}/tests/ycsb"; shift 2 ;;
        --threads) THREADS="$2"; COMMON_ARGS="--threads ${THREADS} --records 10000 --fieldlength 1048576 --ops 10000 --maxmem 16gb"; shift 2 ;;
        --help|-h) usage ;;
        *) log_err "未知参数: $1"; usage ;;
    esac
done

_ssh() {
    sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p "$SSH_PORT" "${REMOTE_USER}@${REMOTE_HOST}" "$@"
}

_scp() {
    sshpass -p "$REMOTE_PASS" scp -P "$SSH_PORT" -o StrictHostKeyChecking=no "$@"
}

sync_remote() {
    log_step "同步测试脚本和 NUMA 源码"
    _ssh "mkdir -p ${REMOTE_YCSB}/scripts ${REMOTE_YCSB}/workloads ${REMOTE_YCSB}/results"
    _scp "${SCRIPT_DIR}/run_progressive_hotspot.sh" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/run_progressive_hotspot.sh"
    _scp "${SCRIPT_DIR}/workloads/workload_progressive_hotspot" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/workloads/workload_progressive_hotspot"
    _scp "${SCRIPT_DIR}/scripts/visualize_progressive_hotspot.py" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/scripts/visualize_progressive_hotspot.py"
    _ssh "chmod +x ${REMOTE_YCSB}/run_progressive_hotspot.sh ${REMOTE_YCSB}/scripts/visualize_progressive_hotspot.py"

    local src_dir="${PROJECT_ROOT}/src"
    for f in "$src_dir"/*.c "$src_dir"/*.h; do
        _scp "$f" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_NUMA_ROOT}/src/" 2>/dev/null
    done
    _ssh "cd ${REMOTE_NUMA_ROOT}/src && make -j\$(nproc) >/tmp/redis_numa_ablation_build.log 2>&1"
    log_ok "同步和编译完成"
}

run_case() {
    local name="$1"
    local extra_args="$2"
    local port="$3"
    log_step "运行消融组: ${name}"
    _ssh "cd ${REMOTE_YCSB} && bash run_progressive_hotspot.sh --variant numa --port ${port} --output-dir results/progressive_hotspot_ablation_${name}_${TIMESTAMP} ${COMMON_ARGS} ${extra_args}" || {
        log_warn "${name} 返回非零，继续后续消融组"
    }
}

fetch_results() {
    log_step "下载消融结果"
    local fetch_dir="${LOCAL_RESULTS}/progressive_hotspot_ablation_${TIMESTAMP}"
    mkdir -p "$fetch_dir"
    for name in baseline no_locality no_access_tracking no_auto_migrate; do
        mkdir -p "$fetch_dir/$name"
        _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/progressive_hotspot_ablation_${name}_${TIMESTAMP}/." "$fetch_dir/$name/" || log_warn "${name} 结果下载失败"
    done
    generate_summary "$fetch_dir"
    log_ok "结果目录: $fetch_dir"
}

generate_summary() {
    local dir="$1"
    local summary="$dir/SUMMARY.txt"
    {
        echo "Redis-NUMA 渐进热点访问消融测试摘要"
        echo "======================================"
        echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "远程: ${REMOTE_USER}@${REMOTE_HOST}"
        echo "负载: 10000 keys * 1048576 bytes, maxmemory=16gb"
        echo "线程: ${THREADS}"
        echo ""
        for name in baseline no_locality no_access_tracking no_auto_migrate; do
            local csv="$dir/$name/progressive_summary.csv"
            if [[ -f "$csv" ]]; then
                echo "── ${name} ──"
                column -s, -t "$csv" 2>/dev/null || cat "$csv"
                echo ""
            fi
        done
    } > "$summary"
    log_ok "摘要已生成: $summary"
}

main() {
    command -v sshpass &>/dev/null || { log_err "sshpass 未安装，请执行: sudo apt install -y sshpass"; exit 1; }
    log "测试 SSH 连接..."
    _ssh "echo ok" &>/dev/null || { log_err "SSH 连接失败: ${REMOTE_USER}@${REMOTE_HOST}"; exit 1; }
    log_ok "SSH 连接正常"

    sync_remote
    run_case baseline "" "$TEST_PORT"
    run_case no_locality "--no-locality-stats" "$TEST_PORT"
    run_case no_access_tracking "--no-access-tracking" "$TEST_PORT"
    run_case no_auto_migrate "--no-auto-migrate" "$TEST_PORT"
    fetch_results
    log_ok "全部消融测试完成"
}

main "$@"
