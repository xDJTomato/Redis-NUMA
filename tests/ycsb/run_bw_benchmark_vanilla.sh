#!/usr/bin/env bash
# ============================================================================
# NUMA 带宽饱和基准测试脚本（原版 Redis 对比版）
#
# 功能与 run_bw_benchmark.sh 完全一致，仅修改 redis-server/redis-cli 路径
# 指向 ../redis-7.2.6-vanilla/src（与本 fork 同版本的原版 Redis 7.2.6，
# jemalloc，无任何 NUMA 模块——这样才是单一变量对比：同一份 Redis 核心，
# 唯一的差异是有没有本项目的 NUMA/CXL 分层。该目录可以直接用本仓库自带的
# upstream 7.2.6 tag 生成：
#   git worktree add ../redis-7.2.6-vanilla 7.2.6 && \
#   cd ../redis-7.2.6-vanilla/src && make -j$(nproc)
# （早期版本曾对比 ../redis-6.2.21，但那是迁移前的旧内核版本，不是公平的
# 同版本对比，已改用 7.2.6。）
#
# 用法: ./run_bw_benchmark_vanilla.sh [选项]
# 选项:
#   --port PORT          Redis 端口 (默认: 6380)
#   --maxmem MEM         最大内存 (默认: 8gb)
#   --output-dir DIR     输出目录 (默认: results/bw_bench_vanilla_<timestamp>)
#   --phase 1|2|3|all    运行哪个阶段 (默认: all)
#   --skip-fill          跳过填充阶段
#   --no-restart         不重启 Redis
#   --help               显示帮助
# ============================================================================

set -euo pipefail

# 调试：捕获任何非 0 退出，打印行号和命令
_on_err() {
    local exit_code=$?
    local line_no=${BASH_LINENO[0]}
    echo -e "\033[0;31m[ERR-TRAP]\033[0m 脚本在第 ${line_no} 行以退出码 ${exit_code} 失败" >&2
    echo -e "\033[0;31m[ERR-TRAP]\033[0m 失败命令: ${BASH_COMMAND}" >&2
}
trap '_on_err' ERR

# ============ 配置常量 ============
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
# 支持环境变量覆盖，方便远程部署到非标准路径
VANILLA_ROOT="${VANILLA_REDIS_ROOT:-$PROJECT_ROOT/../redis-7.2.6-vanilla}"
REDIS_SERVER="$VANILLA_ROOT/src/redis-server"
REDIS_CLI="$VANILLA_ROOT/src/redis-cli"
YCSB_DIR="$SCRIPT_DIR/ycsb-0.17.0"
YCSB_BIN="$YCSB_DIR/bin/ycsb.sh"
WORKLOAD="$SCRIPT_DIR/workloads/workload_bw_saturate"
VISUALIZE_SCRIPT="$SCRIPT_DIR/scripts/visualize_bw_benchmark.py"

# 创建无空格的临时符号链接（ycsb Python wrapper 不支持路径中的空格）
SAFE_LINK="/tmp/redis-vanilla-bench-$$"
ln -sfn "$PROJECT_ROOT" "$SAFE_LINK"
# 优先 ycsb.sh（Java wrapper），回退到 ycsb（Python）
if [[ -x "$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb.sh" ]]; then
    YCSB_BIN="$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb.sh"
else
    YCSB_BIN="$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb"
fi
WORKLOAD="$SAFE_LINK/tests/ycsb/workloads/workload_bw_saturate"

# 默认参数 — 使用 6380 避免与 CXL 版本冲突
REDIS_PORT=6380
REDIS_HOST="127.0.0.1"
MAX_MEMORY="8gb"
VANILLA_CPU_NODE="${VANILLA_CPU_NODE:-0}"
VANILLA_MEM_NODE="${VANILLA_MEM_NODE:-0}"
VANILLA_MEM_POLICY="${VANILLA_MEM_POLICY:-bind}"
OUTPUT_DIR=""
RUN_PHASE="all"
SKIP_FILL=false
NO_RESTART=false

# Phase 参数（与 CXL 版完全一致）
PHASE1_RECORDS=1000000
PHASE1_FIELD_LENGTH=4096
PHASE1_THREADS=8
PHASE2_OPS=4000000
PHASE2_THREADS=64
PHASE3_OPS=4000000
PHASE3_THREADS=64

YCSB_TIMEOUT_MS=30000

# 全局变量
METRICS_CSV=""
COLLECTOR_FLAG=""
PHASE_FLAG=""
COLLECTOR_PID=""

# ── 颜色输出 ────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log()     { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()  { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err() { echo -e "${RED}[ERR]${NC}   $*"; }
log_step() { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

# ── 帮助信息 ────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --port PORT          Redis 端口 (默认: 6380，避免与 CXL 版本冲突)
  --maxmem MEM         最大内存 (默认: 8gb)
  --output-dir DIR     输出目录 (默认: results/bw_bench_vanilla_<timestamp>)
  --phase 1|2|3|all    运行哪个阶段 (默认: all)
  --skip-fill          跳过填充阶段
  --no-restart         不重启 Redis
  --help               显示此帮助

阶段说明:
  Phase 1 (Fill):      填充阶段，加载数据吃满内存
  Phase 2 (Hotspot):   热点迁移，Zipfian 热点访问
  Phase 3 (Sustain):   持续高压，写密集模式

示例:
  $(basename "$0")                              # 运行全部阶段
  $(basename "$0") --phase 2                    # 仅运行 Phase 2
  $(basename "$0") --skip-fill                  # 跳过填充阶段
  $(basename "$0") --no-restart --port 6380     # 使用已有 Redis
  $(basename "$0") --maxmem 8gb                 # 自定义内存限制
EOF
    exit 0
}

# ── 参数解析 ────────────────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --port)      REDIS_PORT="$2"; shift 2 ;;
            --maxmem)    MAX_MEMORY="$2"; shift 2 ;;
            --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
            --phase)
                RUN_PHASE="$2"
                if [[ "$RUN_PHASE" != "1" && "$RUN_PHASE" != "2" && "$RUN_PHASE" != "3" && "$RUN_PHASE" != "all" ]]; then
                    log_err "无效的 phase: $RUN_PHASE, 必须是 1, 2, 3 或 all"
                    exit 1
                fi
                shift 2
                ;;
            --skip-fill)  SKIP_FILL=true; shift ;;
            --no-restart) NO_RESTART=true; shift ;;
            --help|-h)    usage ;;
            *)
                log_err "未知参数: $1"
                usage
                ;;
        esac
    done
}

# ── 前置检查 ────────────────────────────────────────────────────────────────
check_prerequisites() {
    log_step "前置检查"

    if [[ ! -x "$REDIS_SERVER" ]]; then
        log_err "redis-server 未找到: $REDIS_SERVER"
        log "请先生成同版本的原版 Redis 7.2.6 对比基线:"
        log "  git -C $PROJECT_ROOT worktree add ../redis-7.2.6-vanilla 7.2.6"
        log "  cd $PROJECT_ROOT/../redis-7.2.6-vanilla/src && make -j\$(nproc)"
        exit 1
    fi
    log_ok "redis-server: $REDIS_SERVER"

    if [[ ! -x "$REDIS_CLI" ]]; then
        log_err "redis-cli 未找到: $REDIS_CLI"
        exit 1
    fi
    log_ok "redis-cli: $REDIS_CLI"

    if [[ ! -x "$YCSB_BIN" ]]; then
        log_err "YCSB 未找到: $YCSB_BIN"
        log "请先安装: $SCRIPT_DIR/scripts/install_ycsb.sh"
        exit 1
    fi
    log_ok "YCSB: $YCSB_BIN"

    if [[ ! -f "$WORKLOAD" ]]; then
        log_err "工作负载文件不存在: $WORKLOAD"
        exit 1
    fi
    log_ok "工作负载: $WORKLOAD"

    if ! command -v bc &>/dev/null; then
        log_warn "bc 未安装，部分计算可能失败"
    fi

    if command -v python3 &>/dev/null; then
        if python3 -c "import matplotlib" 2>/dev/null; then
            log_ok "python3 + matplotlib 可用"
        else
            log_warn "matplotlib 未安装，可视化将被跳过"
        fi
    else
        log_warn "python3 未安装，可视化将被跳过"
    fi
}

# ── 保存系统信息 ─────────────────────────────────────────────────────────────
save_system_info() {
    local sysinfo_file="$OUTPUT_DIR/system_info.txt"

    {
        echo "=============================================="
        echo "NUMA 带宽饱和基准测试（原版 Redis 对比）- 系统信息"
        echo "=============================================="
        echo ""
        echo "测试时间: $(date)"
        echo "主机名: $(hostname)"
        echo ""
        echo "=== 内核信息 ==="
        uname -a
        echo ""
        echo "=== CPU 信息 ==="
        awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo
        echo "核心数: $(nproc)"
        echo ""
        echo "=== 内存信息 ==="
        free -h
        echo ""
        echo "=== NUMA 拓扑 ==="
        if command -v numactl &>/dev/null; then
            numactl --hardware
        else
            echo "numactl 未安装"
        fi
        echo ""
        echo "=== Redis 版本 ==="
        "$REDIS_SERVER" --version
        echo ""
        echo "=== Redis 分配器 ==="
        "$REDIS_SERVER" --version 2>&1 | grep -i jemalloc && echo "jemalloc" || echo "libc/unknown"
        echo ""
        echo "=== YCSB 版本 ==="
        "$YCSB_BIN" --version 2>&1 | head -3 || echo "YCSB 0.17.0"
        echo ""
        echo "=== 测试参数 ==="
        echo "Redis 端口: $REDIS_PORT"
        echo "Redis 最大内存: $MAX_MEMORY"
        echo "Vanilla CPU node: $VANILLA_CPU_NODE"
        echo "Vanilla memory node: $VANILLA_MEM_NODE"
        echo "Vanilla memory policy: $VANILLA_MEM_POLICY"
        echo "Phase 1 记录数: $PHASE1_RECORDS"
        echo "Phase 1 字段大小: $PHASE1_FIELD_LENGTH bytes"
        echo "Phase 2 操作数: $PHASE2_OPS"
        echo "Phase 2 线程数: $PHASE2_THREADS"
        echo "Phase 3 操作数: $PHASE3_OPS"
        echo "Phase 3 线程数: $PHASE3_THREADS"
    } > "$sysinfo_file"

    log "系统信息已保存: $sysinfo_file"
}

# ── Redis 启动 ──────────────────────────────────────────────────────────────
start_redis() {
    log_step "启动 Redis（原版）"
    log "停止已有 Redis 实例..."

    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    sleep 1
    pkill -f "redis-server.*:${REDIS_PORT}" 2>/dev/null || true
    sleep 1

    log "启动 Redis (port=$REDIS_PORT, maxmemory=$MAX_MEMORY, cpu_node=$VANILLA_CPU_NODE, mem_node=$VANILLA_MEM_NODE, mem_policy=$VANILLA_MEM_POLICY)..."
    local redis_cmd=(
        "$REDIS_SERVER"
        --port "$REDIS_PORT"
        --bind "$REDIS_HOST"
        --maxmemory "$MAX_MEMORY"
        --maxmemory-policy allkeys-lru
        --save ""
        --appendonly no
        --loglevel verbose
        --logfile "$OUTPUT_DIR/redis.log"
        --daemonize yes
    )

    if command -v numactl &>/dev/null; then
        if [[ "$VANILLA_MEM_POLICY" == "interleave" ]]; then
            numactl --cpunodebind="$VANILLA_CPU_NODE" --interleave="$VANILLA_MEM_NODE" "${redis_cmd[@]}"
        else
            numactl --cpunodebind="$VANILLA_CPU_NODE" --membind="$VANILLA_MEM_NODE" "${redis_cmd[@]}"
        fi
    else
        log_warn "numactl 未安装，原版 Redis 将不进行本地内存绑定"
        "${redis_cmd[@]}"
    fi

    local retries=30
    while [[ $retries -gt 0 ]]; do
        local pong
        pong=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING 2>/dev/null || true)
        if [[ "$pong" == *PONG* ]]; then
            log_ok "Redis 已就绪"
            "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" FLUSHALL > /dev/null 2>&1
            return 0
        fi
        retries=$((retries - 1))
        sleep 1
    done

    log_err "Redis 启动失败"
    return 1
}

# ── 后台采集器 ──────────────────────────────────────────────────────────────
start_collector() {
    local csv_file="$1"

    echo "timestamp,phase,ops_total,ops_sec,used_mem_mb,rss_mb,frag_ratio,evicted_keys" > "$csv_file"

    local prev_ops=0

    while [[ -f "$COLLECTOR_FLAG" ]]; do
        local ts=$(date +%s)
        local phase=$(cat "$PHASE_FLAG" 2>/dev/null || echo "unknown")

        local stats=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO stats 2>/dev/null || echo "")
        local ops_total=$(echo "$stats" | grep "total_commands_processed:" | cut -d: -f2 | tr -d '\r')
        local evicted=$(echo "$stats" | grep "evicted_keys:" | cut -d: -f2 | tr -d '\r')

        local meminfo=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null || echo "")
        local used_mem=$(echo "$meminfo" | grep "used_memory:" | head -1 | cut -d: -f2 | tr -d '\r')
        local rss_mem=$(echo "$meminfo" | grep "used_memory_rss:" | head -1 | cut -d: -f2 | tr -d '\r')
        local frag=$(echo "$meminfo" | grep "mem_fragmentation_ratio:" | cut -d: -f2 | tr -d '\r')

        local used_mb rss_mb
        if command -v bc &>/dev/null; then
            used_mb=$(echo "scale=1; ${used_mem:-0} / 1048576" | bc 2>/dev/null || echo "0")
            rss_mb=$(echo "scale=1; ${rss_mem:-0} / 1048576" | bc 2>/dev/null || echo "0")
        else
            used_mb=$((${used_mem:-0} / 1048576))
            rss_mb=$((${rss_mem:-0} / 1048576))
        fi

        local ops_sec=$(( ${ops_total:-0} - ${prev_ops:-0} ))
        [[ "${ops_sec:-0}" -lt 0 ]] 2>/dev/null && ops_sec=0

        echo "${ts},${phase},${ops_total:-0},${ops_sec},${used_mb},${rss_mb},${frag:-0},${evicted:-0}" >> "$csv_file"

        prev_ops=${ops_total:-0}

        sleep 1
    done
}

# ── 阶段执行函数 ────────────────────────────────────────────────────────────
run_phase1_fill() {
    log_step "Phase 1: Fill (吃满内存)"
    echo "1_fill" > "$PHASE_FLAG"

    echo "PHASE_MARKER,1,fill_start,$(date +%s)" >> "$METRICS_CSV"

    local total_gb=$((PHASE1_RECORDS * PHASE1_FIELD_LENGTH / 1024 / 1024 / 1024))
    log "Loading $PHASE1_RECORDS records x ${PHASE1_FIELD_LENGTH}B (~${total_gb}GB)..."

    "$YCSB_BIN" load redis -s \
        -P "$WORKLOAD" \
        -p "recordcount=$PHASE1_RECORDS" \
        -p "fieldlength=$PHASE1_FIELD_LENGTH" \
        -p "redis.host=$REDIS_HOST" \
        -p "redis.port=$REDIS_PORT" \
        -p "redis.timeout=$YCSB_TIMEOUT_MS" \
        -p "threadcount=$PHASE1_THREADS" \
        > "$OUTPUT_DIR/phase1_load.txt" 2>&1

    echo "PHASE_MARKER,1,fill_end,$(date +%s)" >> "$METRICS_CSV"

    local throughput
    throughput=$(grep 'OVERALL.*Throughput' "$OUTPUT_DIR/phase1_load.txt" 2>/dev/null || echo "See phase1_load.txt")
    log_ok "Phase 1 完成. $throughput"
}

run_phase2_hotspot() {
    log_step "Phase 2: Hotspot Migration (极端热点)"
    echo "2_hotspot" > "$PHASE_FLAG"
    echo "PHASE_MARKER,2,hotspot_start,$(date +%s)" >> "$METRICS_CSV"

    log "Running $PHASE2_OPS ops with Zipfian α=0.99, $PHASE2_THREADS threads..."

    "$YCSB_BIN" run redis -s \
        -P "$WORKLOAD" \
        -p "operationcount=$PHASE2_OPS" \
        -p "threadcount=$PHASE2_THREADS" \
        -p "redis.host=$REDIS_HOST" \
        -p "redis.port=$REDIS_PORT" \
        -p "redis.timeout=$YCSB_TIMEOUT_MS" \
        > "$OUTPUT_DIR/phase2_hotspot.txt" 2>&1

    echo "PHASE_MARKER,2,hotspot_end,$(date +%s)" >> "$METRICS_CSV"

    local throughput
    throughput=$(grep 'OVERALL.*Throughput' "$OUTPUT_DIR/phase2_hotspot.txt" 2>/dev/null || echo "See phase2_hotspot.txt")
    log_ok "Phase 2 完成. $throughput"
}

run_phase3_sustain() {
    log_step "Phase 3: Sustained Pressure (写密集高压)"
    echo "3_sustain" > "$PHASE_FLAG"
    echo "PHASE_MARKER,3,sustain_start,$(date +%s)" >> "$METRICS_CSV"

    log "Running $PHASE3_OPS ops with write-heavy (60%), $PHASE3_THREADS threads..."

    "$YCSB_BIN" run redis -s \
        -P "$WORKLOAD" \
        -p "operationcount=$PHASE3_OPS" \
        -p "threadcount=$PHASE3_THREADS" \
        -p "readproportion=0.3" \
        -p "updateproportion=0.7" \
        -p "insertproportion=0" \
        -p "redis.host=$REDIS_HOST" \
        -p "redis.port=$REDIS_PORT" \
        -p "redis.timeout=$YCSB_TIMEOUT_MS" \
        > "$OUTPUT_DIR/phase3_sustain.txt" 2>&1

    echo "PHASE_MARKER,3,sustain_end,$(date +%s)" >> "$METRICS_CSV"

    local throughput
    throughput=$(grep 'OVERALL.*Throughput' "$OUTPUT_DIR/phase3_sustain.txt" 2>/dev/null || echo "See phase3_sustain.txt")
    log_ok "Phase 3 完成. $throughput"
}

# ── 生成报告 ────────────────────────────────────────────────────────────────
generate_report() {
    log "生成可视化报告..."

    if [[ ! -f "$VISUALIZE_SCRIPT" ]]; then
        log_warn "可视化脚本不存在: $VISUALIZE_SCRIPT"
        return
    fi

    if ! command -v python3 &>/dev/null; then
        log_warn "python3 未找到，跳过可视化"
        return
    fi

    # 复用 NUMA 版 venv（已含 matplotlib/pandas，且与 NumPy 版本兼容）
    local VENV_DIR="$SCRIPT_DIR/scripts/.venv"
    local PYTHON="$VENV_DIR/bin/python"

    if [[ ! -x "$PYTHON" ]]; then
        log "首次运行，创建 Python 虚拟环境..."
        python3 -m venv "$VENV_DIR" || {
            log_warn "创建 venv 失败，跳过可视化（sudo apt install python3-venv）"
            return
        }
        "$VENV_DIR/bin/pip" install --quiet matplotlib pandas || {
            log_warn "依赖安装失败，跳过可视化"
            return
        }
        log_ok "虚拟环境就绪"
    else
        if ! "$PYTHON" -c "import matplotlib, pandas" 2>/dev/null; then
            "$VENV_DIR/bin/pip" install --quiet matplotlib pandas
        fi
    fi

    "$PYTHON" "$VISUALIZE_SCRIPT" \
        --input "$METRICS_CSV" \
        --label "Vanilla Redis" \
        --phase-dir "$OUTPUT_DIR" \
        --output "$OUTPUT_DIR/benchmark_report.png" \
        2>&1 || log_warn "可视化失败，请查看 metrics.csv"

    if [[ -f "$OUTPUT_DIR/benchmark_report.png" ]]; then
        log_ok "报告已生成: $OUTPUT_DIR/benchmark_report.png"
    fi
}

# ── 打印摘要 ────────────────────────────────────────────────────────────────
print_summary() {
    log_step "测试摘要"

    echo ""
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo -e "${BOLD}${CYAN}   NUMA 带宽基准测试结果（原版 Redis）${NC}"
    echo -e "${BOLD}${CYAN}========================================${NC}"
    echo ""

    if [[ -f "$OUTPUT_DIR/phase1_load.txt" ]]; then
        echo -e "${BOLD}Phase 1 (Fill):${NC}"
        grep 'OVERALL.*Throughput' "$OUTPUT_DIR/phase1_load.txt" 2>/dev/null || echo "  (无吞吐量数据)"
        echo ""
    fi

    if [[ -f "$OUTPUT_DIR/phase2_hotspot.txt" ]]; then
        echo -e "${BOLD}Phase 2 (Hotspot):${NC}"
        grep 'OVERALL.*Throughput' "$OUTPUT_DIR/phase2_hotspot.txt" 2>/dev/null || echo "  (无吞吐量数据)"
        grep -E '^\[READ\].*AverageLatency|^\[UPDATE\].*AverageLatency' "$OUTPUT_DIR/phase2_hotspot.txt" 2>/dev/null || true
        echo ""
    fi

    if [[ -f "$OUTPUT_DIR/phase3_sustain.txt" ]]; then
        echo -e "${BOLD}Phase 3 (Sustain):${NC}"
        grep 'OVERALL.*Throughput' "$OUTPUT_DIR/phase3_sustain.txt" 2>/dev/null || echo "  (无吞吐量数据)"
        grep -E '^\[READ\].*AverageLatency|^\[UPDATE\].*AverageLatency' "$OUTPUT_DIR/phase3_sustain.txt" 2>/dev/null || true
        echo ""
    fi

    # 最终内存状态
    echo -e "${BOLD}最终内存状态:${NC}"
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | grep -E "used_memory:|used_memory_rss:|mem_fragmentation_ratio:" || echo "  (无内存数据)"
    echo ""

    echo -e "${BOLD}输出文件:${NC}"
    echo "  目录: $OUTPUT_DIR"
    echo "  指标: $(basename "$METRICS_CSV")"
    [[ -f "$OUTPUT_DIR/phase1_load.txt" ]] && echo "  Phase 1: phase1_load.txt"
    [[ -f "$OUTPUT_DIR/phase2_hotspot.txt" ]] && echo "  Phase 2: phase2_hotspot.txt"
    [[ -f "$OUTPUT_DIR/phase3_sustain.txt" ]] && echo "  Phase 3: phase3_sustain.txt"
    [[ -f "$OUTPUT_DIR/redis.log" ]] && echo "  Redis 日志: redis.log"
    [[ -f "$OUTPUT_DIR/benchmark_report.png" ]] && echo "  报告: benchmark_report.png"
}

# ── 清理函数 ────────────────────────────────────────────────────────────────
cleanup() {
    local exit_code=$?
    log "清理中... (退出码: ${exit_code})"

    rm -f "$SAFE_LINK" 2>/dev/null || true

    if [[ -n "$COLLECTOR_FLAG" && -f "$COLLECTOR_FLAG" ]]; then
        rm -f "$COLLECTOR_FLAG"
        if [[ -n "$COLLECTOR_PID" ]]; then
            wait $COLLECTOR_PID 2>/dev/null || true
        fi
    fi

    [[ -n "$PHASE_FLAG" ]] && rm -f "$PHASE_FLAG"

    if [[ "$NO_RESTART" = false ]]; then
        log "停止 Redis..."
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    fi
}

# ── 主流程 ──────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"

    if [[ -z "$OUTPUT_DIR" ]]; then
        OUTPUT_DIR="$SCRIPT_DIR/results/bw_bench_vanilla_$(date +%Y%m%d_%H%M%S)"
    fi
    mkdir -p "$OUTPUT_DIR"

    METRICS_CSV="$OUTPUT_DIR/metrics.csv"
    COLLECTOR_FLAG="$OUTPUT_DIR/.collector_running"
    PHASE_FLAG="$OUTPUT_DIR/.current_phase"

    echo -e "${BOLD}${CYAN}"
    echo "╔════════════════════════════════════════════════════╗"
    echo "║  NUMA 带宽饱和基准测试（原版 Redis 对比）           ║"
    echo "╚════════════════════════════════════════════════════╝"
    echo -e "${NC}"

    log "输出目录: $OUTPUT_DIR"
    log "运行阶段: $RUN_PHASE"

    trap cleanup EXIT

    check_prerequisites
    save_system_info

    if [[ "$NO_RESTART" = false ]]; then
        start_redis
    else
        local pong
        pong=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING 2>/dev/null || true)
        if [[ "$pong" != *PONG* ]]; then
            log_err "Redis 未响应 ($REDIS_HOST:$REDIS_PORT)"
            log "请先启动 Redis 或去掉 --no-restart 参数"
            exit 1
        fi
        log_ok "Redis 连接正常 ($REDIS_HOST:$REDIS_PORT)"
    fi

    touch "$COLLECTOR_FLAG"
    echo "init" > "$PHASE_FLAG"
    start_collector "$METRICS_CSV" &
    COLLECTOR_PID=$!
    log "指标采集器已启动 (PID: $COLLECTOR_PID)"

    if [[ "$RUN_PHASE" = "all" || "$RUN_PHASE" = "1" ]]; then
        if [[ "$SKIP_FILL" = false ]]; then
            run_phase1_fill
        else
            log "跳过 Phase 1 (--skip-fill)"
        fi
    fi

    if [[ "$RUN_PHASE" = "all" || "$RUN_PHASE" = "2" ]]; then
        run_phase2_hotspot
    fi

    if [[ "$RUN_PHASE" = "all" || "$RUN_PHASE" = "3" ]]; then
        run_phase3_sustain
    fi

    rm -f "$COLLECTOR_FLAG"
    wait $COLLECTOR_PID 2>/dev/null || true
    rm -f "$PHASE_FLAG"
    log "指标采集器已停止"

    generate_report
    print_summary

    log_ok "基准测试完成！结果保存在: $OUTPUT_DIR"
}

main "$@"
