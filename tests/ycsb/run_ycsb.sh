#!/bin/bash
#
# YCSB 压力测试统一入口脚本
# 用法: ./run_ycsb.sh --mode <baseline|stress> [选项]
#
# --mode baseline  基线测试：标准工作负载，轻量参数，验证功能正确性
# --mode stress    压力测试：高并发、大数据量，评估极限性能
#
# 选项:
#   --port PORT        Redis 端口 (默认 6379)
#   --host HOST        Redis 地址 (默认 127.0.0.1)
#   --maxmem MEM       Redis 最大内存 (默认 8gb)
#   --output-dir DIR   结果输出目录 (默认 results/)
#   --no-restart       不重启 Redis，使用已有实例
#   --help             显示帮助

set -euo pipefail

# ── 路径定义 ────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
YCSB_BIN="$SCRIPT_DIR/ycsb-0.17.0/bin/ycsb"
WORKLOADS_DIR="$SCRIPT_DIR/workloads"
RESULTS_DIR="$SCRIPT_DIR/results"
REDIS_SERVER="$ROOT_DIR/src/redis-server"
REDIS_CLI="$ROOT_DIR/src/redis-cli"

# ── 默认参数 ────────────────────────────────────────────────────────────────
MODE=""
REDIS_PORT=6379
REDIS_HOST="127.0.0.1"
REDIS_MAXMEM="8gb"
OUTPUT_DIR="$RESULTS_DIR"
NO_RESTART=0

# ── 颜色输出 ────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()   { echo -e "${RED}[ERR]${NC}   $*"; }
log_step()  { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

# ── 帮助信息 ────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
用法: $(basename "$0") --mode <baseline|stress> [选项]

模式:
  baseline    基线测试 (10 万条记录, 4 线程, 读写混合)
              工作负载: workload_baseline
  stress      压力测试 (100 万条记录, 32 线程, 写密集)
              工作负载: workload_stress

选项:
  --port PORT        Redis 端口      (默认: 6379)
  --host HOST        Redis 地址      (默认: 127.0.0.1)
  --maxmem MEM       Redis 最大内存  (默认: 8gb)
  --output-dir DIR   结果输出目录    (默认: tests/ycsb/results/)
  --no-restart       跳过 Redis 重启，使用已有实例
  --help             显示此帮助

示例:
  ./run_ycsb.sh --mode baseline
  ./run_ycsb.sh --mode stress --maxmem 16gb
  ./run_ycsb.sh --mode baseline --no-restart --port 6380
EOF
    exit 0
}

# ── 参数解析 ────────────────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --mode)       MODE="$2";        shift 2 ;;
            --port)       REDIS_PORT="$2";  shift 2 ;;
            --host)       REDIS_HOST="$2";  shift 2 ;;
            --maxmem)     REDIS_MAXMEM="$2"; shift 2 ;;
            --output-dir) OUTPUT_DIR="$2";  shift 2 ;;
            --no-restart) NO_RESTART=1;     shift   ;;
            --help|-h)    usage ;;
            *) log_err "未知参数: $1"; usage ;;
        esac
    done

    if [[ -z "$MODE" ]]; then
        log_err "必须指定 --mode baseline 或 --mode stress"
        usage
    fi
    if [[ "$MODE" != "baseline" && "$MODE" != "stress" ]]; then
        log_err "无效模式: $MODE，仅支持 baseline / stress"
        usage
    fi
}

# ── 前置检查 ────────────────────────────────────────────────────────────────
check_deps() {
    log_step "前置检查"

    if [[ ! -x "$YCSB_BIN" ]]; then
        log_err "YCSB 未找到: $YCSB_BIN"
        log_info "请先运行: $SCRIPT_DIR/scripts/install_ycsb.sh"
        exit 1
    fi
    log_ok "YCSB: $YCSB_BIN"

    if [[ ! -x "$REDIS_SERVER" ]]; then
        log_err "redis-server 未找到: $REDIS_SERVER"
        log_info "请先编译项目: make -C $ROOT_DIR"
        exit 1
    fi
    log_ok "redis-server: $REDIS_SERVER"

    local workload="$WORKLOADS_DIR/workload_${MODE}"
    if [[ ! -f "$workload" ]]; then
        log_err "工作负载文件不存在: $workload"
        exit 1
    fi
    log_ok "工作负载: $workload"

    mkdir -p "$OUTPUT_DIR"
}

# ── Redis 重启 ────────────────────────────────────────────────────────────
restart_redis() {
    log_step "重启 Redis"
    log_info "停止已有 Redis 实例..."
    pkill -9 -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
    sleep 2

    log_info "启动 Redis (port=$REDIS_PORT, maxmem=$REDIS_MAXMEM)..."
    "$REDIS_SERVER" \
        --daemonize yes \
        --port "$REDIS_PORT" \
        --bind "$REDIS_HOST" \
        --loglevel warning \
        --save "" \
        --appendonly no \
        --maxmemory "$REDIS_MAXMEM" \
        --maxmemory-policy allkeys-lru

    # 等待就绪（最多 10s）
    local i=0
    while [[ $i -lt 10 ]]; do
        if "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" ping 2>/dev/null | grep -q "PONG"; then
            log_ok "Redis 启动成功"
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    log_err "Redis 启动超时"
    exit 1
}

# ── 检查 Redis 连接 ──────────────────────────────────────────────────────────
check_redis() {
    if ! "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" ping 2>/dev/null | grep -q "PONG"; then
        log_err "Redis 未响应 ($REDIS_HOST:$REDIS_PORT)"
        log_info "请先启动 Redis 或去掉 --no-restart 参数"
        exit 1
    fi
    log_ok "Redis 连接正常 ($REDIS_HOST:$REDIS_PORT)"
}

# ── 收集系统信息 ─────────────────────────────────────────────────────────────
collect_sysinfo() {
    local outfile="$1"
    {
        echo "测试时间:   $(date)"
        echo "主机名:     $(hostname)"
        echo "测试模式:   $MODE"
        echo ""
        echo "=== CPU ==="
        grep "model name" /proc/cpuinfo | head -1 | cut -d: -f2 | xargs
        echo "核心数: $(nproc)"
        echo ""
        echo "=== 内存 ==="
        free -h
        echo ""
        echo "=== NUMA ==="
        command -v numactl &>/dev/null && numactl --hardware || echo "numactl 未安装"
        echo ""
        echo "=== Redis ==="
        "$REDIS_SERVER" --version
        echo ""
        echo "=== YCSB ==="
        "$YCSB_BIN" --version 2>&1 | head -3 || echo "YCSB 0.17.0"
    } > "$outfile"
}

# ── 运行 YCSB 测试 ──────────────────────────────────────────────────────────
run_ycsb() {
    local workload="workload_${MODE}"
    local timestamp
    timestamp="$(date +%Y%m%d_%H%M%S)"
    local run_dir="$OUTPUT_DIR/${MODE}_${timestamp}"
    mkdir -p "$run_dir"

    # 系统信息
    collect_sysinfo "$run_dir/sysinfo.txt"
    log_info "系统信息: $run_dir/sysinfo.txt"

    # 模式参数
    local record_count threads
    if [[ "$MODE" == "baseline" ]]; then
        record_count=100000
        threads=4
    else
        record_count=1000000
        threads=32
    fi

    log_step "Load 阶段 (recordcount=$record_count, threads=$threads)"
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" FLUSHALL > /dev/null
    log_info "已清空 Redis 数据"

    "$YCSB_BIN" load redis \
        -P "$WORKLOADS_DIR/$workload" \
        -p recordcount="$record_count" \
        -p threadcount="$threads" \
        -p redis.host="$REDIS_HOST" \
        -p redis.port="$REDIS_PORT" \
        -s 2>&1 | tee "$run_dir/load.txt"

    log_step "Run 阶段 (operationcount=$record_count, threads=$threads)"
    "$YCSB_BIN" run redis \
        -P "$WORKLOADS_DIR/$workload" \
        -p recordcount="$record_count" \
        -p operationcount="$record_count" \
        -p threadcount="$threads" \
        -p redis.host="$REDIS_HOST" \
        -p redis.port="$REDIS_PORT" \
        -s 2>&1 | tee "$run_dir/run.txt"

    # 摘要
    log_step "测试摘要"
    echo -e "${BOLD}吞吐量 (Throughput):${NC}"
    grep -E "^\[OVERALL\].*Throughput" "$run_dir/run.txt" || true
    echo -e "${BOLD}延迟 (READ/UPDATE 平均值):${NC}"
    grep -E "^\[READ\].*AverageLatency|^\[UPDATE\].*AverageLatency" "$run_dir/run.txt" || true

    log_ok "结果保存至: $run_dir"
}

# ── 清理 ────────────────────────────────────────────────────────────────────
cleanup() {
    if [[ $NO_RESTART -eq 0 ]]; then
        log_info "测试完成，停止 Redis..."
        pkill -9 -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
    fi
}
trap cleanup EXIT

# ── 主流程 ──────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"

    echo -e "${BOLD}${CYAN}"
    echo "╔══════════════════════════════════════╗"
    echo "║   YCSB 压力测试框架  mode=$MODE"
    echo "╚══════════════════════════════════════╝"
    echo -e "${NC}"

    check_deps

    if [[ $NO_RESTART -eq 1 ]]; then
        check_redis
    else
        restart_redis
    fi

    run_ycsb

    log_ok "全部完成"
}

main "$@"
