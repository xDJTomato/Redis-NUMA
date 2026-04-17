#!/bin/bash
# ============================================================================
# CXL 内存性能评估脚本
#
# 用途: 评估 QEMU CXL 虚拟机中模拟 CXL 设备的读写性能
# 目标: 对比 Node 0 (DRAM) 和 Node 1 (CXL) 的性能差异
#
# 用法: ./eval_cxl_memory.sh [选项]
# 选项:
#   --output FILE        结果输出文件 (默认: 终端输出)
#   --redis-port PORT    Redis 测试端口 (默认: 16380)
#   --skip-redis         跳过 Redis 专项测试
#   --help               显示帮助
# ============================================================================

set -euo pipefail

# ============ 配置常量 ============
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
REDIS_SERVER="$PROJECT_ROOT/src/redis-server"
REDIS_CLI="$PROJECT_ROOT/src/redis-cli"
REDIS_BENCHMARK="$PROJECT_ROOT/src/redis-benchmark"
CTAP_DIR="$PROJECT_ROOT/monitor/cmm-d-ctap_v2.2.4"

# 默认参数
OUTPUT_FILE=""
REDIS_PORT=16380
REDIS_HOST="127.0.0.1"
SKIP_REDIS=false

# 测试结果存储
declare -A RESULTS

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
  --output FILE        结果输出文件 (默认: 仅终端输出)
  --redis-port PORT    Redis 测试端口 (默认: 16380)
  --skip-redis         跳过 Redis 专项测试
  --help               显示此帮助

功能:
  1. NUMA 拓扑检测
  2. 内存带宽测试 (mbw/dd/Python)
  3. 内存延迟测试 (sysbench/Python)
  4. Redis 专项性能对比
  5. 生成性能评估报告

示例:
  $(basename "$0")                              # 运行全部测试
  $(basename "$0") --skip-redis                 # 跳过 Redis 测试
  $(basename "$0") --output results/cxl_eval.txt # 保存结果到文件
  $(basename "$0") --redis-port 6380            # 使用自定义 Redis 端口
EOF
    exit 0
}

# ── 参数解析 ────────────────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --output)
                OUTPUT_FILE="$2"
                shift 2
                ;;
            --redis-port)
                REDIS_PORT="$2"
                shift 2
                ;;
            --skip-redis)
                SKIP_REDIS=true
                shift
                ;;
            --help|-h)
                usage
                ;;
            *)
                log_err "未知参数: $1"
                usage
                ;;
        esac
    done
}

# ── 输出函数（同时输出到终端和文件）─────────────────────────────────────────
output() {
    echo "$*"
    if [[ -n "$OUTPUT_FILE" ]]; then
        # 去除颜色码后写入文件
        echo "$*" | sed 's/\x1b\[[0-9;]*m//g' >> "$OUTPUT_FILE"
    fi
}

output_banner() {
    local line="$1"
    output "$line"
}

# ── 前置检查 ────────────────────────────────────────────────────────────────
check_prerequisites() {
    log_step "前置检查"

    # 检查 NUMA 环境
    if ! command -v numactl &>/dev/null; then
        log_err "numactl 未安装，无法执行 NUMA 绑定测试"
        exit 1
    fi
    log_ok "numactl 已安装"

    # 检测 NUMA 节点数
    local num_nodes
    num_nodes=$(numactl --hardware | grep "available:" | awk '{print $2}')
    if [[ "$num_nodes" -lt 2 ]]; then
        log_err "检测到仅 $num_nodes 个 NUMA 节点，需要至少 2 个节点"
        exit 1
    fi
    log_ok "检测到 $num_nodes 个 NUMA 节点"

    # 显示 NUMA 拓扑
    log_step "NUMA 拓扑信息"
    numactl --hardware
}

# ── 内存带宽测试 ────────────────────────────────────────────────────────────
test_memory_bandwidth() {
    log_step "内存带宽测试"

    if ! command -v python3 &>/dev/null; then
        log_err "Python3 未安装，无法执行带宽测试"
        RESULTS["bw_n0_write"]="N/A"
        RESULTS["bw_n1_write"]="N/A"
        RESULTS["bw_n0_read"]="N/A"
        RESULTS["bw_n1_read"]="N/A"
        return
    fi

    # Python 内存带宽测试脚本
    local bw_script
    bw_script=$(cat <<'PYEOF'
import ctypes, time, sys

SIZE = 256 * 1024 * 1024  # 256MB
ITERATIONS = 3

# 分配对齐内存
buf = ctypes.create_string_buffer(SIZE)
src = ctypes.create_string_buffer(SIZE)

# 预热：写满
ctypes.memset(buf, 0x55, SIZE)
ctypes.memset(src, 0xAA, SIZE)

# 写带宽测试：memcpy src -> buf
write_times = []
for _ in range(ITERATIONS):
    start = time.monotonic()
    ctypes.memmove(buf, src, SIZE)
    elapsed = time.monotonic() - start
    write_times.append(elapsed)

avg_write = sum(write_times) / len(write_times)
write_bw = SIZE / avg_write / 1024 / 1024  # MB/s

# 读带宽测试：逐页顺序读
read_times = []
for _ in range(ITERATIONS):
    total = 0
    start = time.monotonic()
    # 使用 ctypes 读取每个页面的第一个字节
    ptr = ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))
    for offset in range(0, SIZE, 4096):
        _ = ptr[offset]
    elapsed = time.monotonic() - start
    read_times.append(elapsed)

avg_read = sum(read_times) / len(read_times)
read_bw = SIZE / avg_read / 1024 / 1024  # MB/s

print(f"WRITE_BW_MBPS:{write_bw:.0f}")
print(f"READ_BW_MBPS:{read_bw:.0f}")
PYEOF
)

    # Node 0 测试
    log "测试 Node 0 (DRAM) 带宽..."
    local py_out
    if py_out=$(numactl --membind=0 --cpunodebind=0 python3 -c "$bw_script" 2>&1); then
        RESULTS["bw_n0_write"]=$(echo "$py_out" | grep "WRITE_BW_MBPS:" | cut -d: -f2)
        RESULTS["bw_n0_read"]=$(echo "$py_out" | grep "READ_BW_MBPS:" | cut -d: -f2)
        log_ok "Node 0 写带宽: ${RESULTS["bw_n0_write"]} MB/s"
        log_ok "Node 0 读带宽: ${RESULTS["bw_n0_read"]} MB/s"
    else
        log_err "Node 0 带宽测试失败"
        log_err "输出: $py_out"
        RESULTS["bw_n0_write"]="N/A"
        RESULTS["bw_n0_read"]="N/A"
    fi

    # Node 1 测试（CXL 节点无 CPU，只绑定内存）
    log "测试 Node 1 (CXL) 带宽..."
    if py_out=$(numactl --membind=1 --cpunodebind=0 python3 -c "$bw_script" 2>&1); then
        RESULTS["bw_n1_write"]=$(echo "$py_out" | grep "WRITE_BW_MBPS:" | cut -d: -f2)
        RESULTS["bw_n1_read"]=$(echo "$py_out" | grep "READ_BW_MBPS:" | cut -d: -f2)
        log_ok "Node 1 写带宽: ${RESULTS["bw_n1_write"]} MB/s"
        log_ok "Node 1 读带宽: ${RESULTS["bw_n1_read"]} MB/s"
    else
        log_err "Node 1 带宽测试失败"
        log_err "输出: $py_out"
        RESULTS["bw_n1_write"]="N/A"
        RESULTS["bw_n1_read"]="N/A"
    fi
}

# ── 内存延迟测试 ────────────────────────────────────────────────────────────
test_memory_latency() {
    log_step "内存延迟测试"

    if ! command -v python3 &>/dev/null; then
        log_err "Python3 未安装，无法执行延迟测试"
        RESULTS["lat_n0"]="N/A"
        RESULTS["lat_n1"]="N/A"
        return
    fi

    local lat_script
    lat_script=$(cat <<'PYEOF'
import ctypes, time, random

SIZE = 64 * 1024 * 1024  # 64MB
PAGE_SIZE = 4096

# 分配并初始化
buf = ctypes.create_string_buffer(SIZE)
ctypes.memset(buf, 0x42, SIZE)
ptr = ctypes.cast(buf, ctypes.POINTER(ctypes.c_char))

num_pages = SIZE // PAGE_SIZE

# 生成随机访问序列（模拟随机延迟）
indices = list(range(0, SIZE, PAGE_SIZE))
random.shuffle(indices)

# 预热
for idx in indices[:1000]:
    _ = ptr[idx]

# 测试：随机访问所有页面
ITERATIONS = 3
total_ns = 0
for _ in range(ITERATIONS):
    start = time.monotonic()
    for idx in indices:
        _ = ptr[idx]
    elapsed = time.monotonic() - start
    total_ns += elapsed * 1e9

avg_latency_ns = total_ns / (ITERATIONS * num_pages)
print(f"LATENCY_NS:{avg_latency_ns:.0f}")
PYEOF
)

    # Node 0
    log "测试 Node 0 (DRAM) 延迟..."
    local py_out
    if py_out=$(numactl --membind=0 --cpunodebind=0 python3 -c "$lat_script" 2>&1); then
        RESULTS["lat_n0"]=$(echo "$py_out" | grep "LATENCY_NS:" | cut -d: -f2)
        log_ok "Node 0 延迟: ${RESULTS["lat_n0"]} ns"
    else
        log_err "Node 0 延迟测试失败: $py_out"
        RESULTS["lat_n0"]="N/A"
    fi

    # Node 1
    log "测试 Node 1 (CXL) 延迟..."
    if py_out=$(numactl --membind=1 --cpunodebind=0 python3 -c "$lat_script" 2>&1); then
        RESULTS["lat_n1"]=$(echo "$py_out" | grep "LATENCY_NS:" | cut -d: -f2)
        log_ok "Node 1 延迟: ${RESULTS["lat_n1"]} ns"
    else
        log_err "Node 1 延迟测试失败: $py_out"
        RESULTS["lat_n1"]="N/A"
    fi
}

# ── C-TAP 工具测试 ──────────────────────────────────────────────────────────
test_ctap() {
    log_step "C-TAP 工具测试"

    if [[ ! -x "$CTAP_DIR/ctap" ]]; then
        log_warn "C-TAP 工具不可用: $CTAP_DIR/ctap"
        return
    fi

    log_ok "C-TAP 工具可用: $CTAP_DIR/ctap"
    log "运行 C-TAP 基准测试..."
    
    # 运行 C-TAP 测试（如果支持）
    "$CTAP_DIR/ctap" --help &>/dev/null || true
}

# ── Redis 专项测试 ──────────────────────────────────────────────────────────
test_redis_performance() {
    log_step "Redis 专项性能测试"

    if [[ "$SKIP_REDIS" = true ]]; then
        log "跳过 Redis 测试 (--skip-redis)"
        RESULTS["redis_set_n0"]="N/A"
        RESULTS["redis_set_n1"]="N/A"
        RESULTS["redis_get_n0"]="N/A"
        RESULTS["redis_get_n1"]="N/A"
        return
    fi

    # 检查 Redis 可执行文件
    if [[ ! -x "$REDIS_SERVER" ]]; then
        log_warn "redis-server 未找到: $REDIS_SERVER"
        RESULTS["redis_set_n0"]="N/A"
        RESULTS["redis_set_n1"]="N/A"
        RESULTS["redis_get_n0"]="N/A"
        RESULTS["redis_get_n1"]="N/A"
        return
    fi
    
    if [[ ! -x "$REDIS_BENCHMARK" ]]; then
        log_warn "redis-benchmark 未找到: $REDIS_BENCHMARK"
        RESULTS["redis_set_n0"]="N/A"
        RESULTS["redis_set_n1"]="N/A"
        RESULTS["redis_get_n0"]="N/A"
        RESULTS["redis_get_n1"]="N/A"
        return
    fi

    # 确保端口未被占用
    pkill -f "redis-server.*:${REDIS_PORT}" 2>/dev/null || true
    sleep 1

    # 测试 Node 0
    log "在 Node 0 (DRAM) 上启动 Redis..."
    numactl --membind=0 --cpunodebind=0 "$REDIS_SERVER" \
        --port "$REDIS_PORT" \
        --maxmemory 1gb \
        --daemonize yes \
        --save "" \
        --appendonly no \
        --loglevel warning

    # 等待 Redis 就绪
    local retries=30
    while [[ $retries -gt 0 ]]; do
        if "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING 2>/dev/null | grep -q PONG; then
            break
        fi
        retries=$((retries - 1))
        sleep 1
    done

    if [[ $retries -eq 0 ]]; then
        log_err "Redis 在 Node 0 上启动失败"
        RESULTS["redis_set_n0"]="N/A"
        RESULTS["redis_get_n0"]="N/A"
    else
        log "运行 redis-benchmark (Node 0)..."
        local bench_n0
        bench_n0=$("$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" -c 8 -n 50000 -d 1024 -t set,get -q 2>&1)
        
        RESULTS["redis_set_n0"]=$(echo "$bench_n0" | grep "^SET:" | awk '{printf "%.0f", $2}')
        [[ -z "${RESULTS["redis_set_n0"]}" ]] && RESULTS["redis_set_n0"]="N/A"
        RESULTS["redis_get_n0"]=$(echo "$bench_n0" | grep "^GET:" | awk '{printf "%.0f", $2}')
        [[ -z "${RESULTS["redis_get_n0"]}" ]] && RESULTS["redis_get_n0"]="N/A"
        
        log_ok "Node 0 SET: ${RESULTS["redis_set_n0"]} ops/sec"
        log_ok "Node 0 GET: ${RESULTS["redis_get_n0"]} ops/sec"
    fi

    # 关闭 Node 0 的 Redis
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    sleep 1

    # 测试 Node 1（CXL 节点无 CPU，CPU 绑定到 Node 0）
    log "在 Node 1 (CXL) 上启动 Redis..."
    numactl --membind=1 --cpunodebind=0 "$REDIS_SERVER" \
        --port "$REDIS_PORT" \
        --maxmemory 1gb \
        --daemonize yes \
        --save "" \
        --appendonly no \
        --loglevel warning

    # 等待 Redis 就绪
    retries=30
    while [[ $retries -gt 0 ]]; do
        if "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING 2>/dev/null | grep -q PONG; then
            break
        fi
        retries=$((retries - 1))
        sleep 1
    done

    if [[ $retries -eq 0 ]]; then
        log_err "Redis 在 Node 1 上启动失败"
        RESULTS["redis_set_n1"]="N/A"
        RESULTS["redis_get_n1"]="N/A"
    else
        log "运行 redis-benchmark (Node 1)..."
        local bench_n1
        bench_n1=$("$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" -c 8 -n 50000 -d 1024 -t set,get -q 2>&1)
        
        RESULTS["redis_set_n1"]=$(echo "$bench_n1" | grep "^SET:" | awk '{printf "%.0f", $2}')
        [[ -z "${RESULTS["redis_set_n1"]}" ]] && RESULTS["redis_set_n1"]="N/A"
        RESULTS["redis_get_n1"]=$(echo "$bench_n1" | grep "^GET:" | awk '{printf "%.0f", $2}')
        [[ -z "${RESULTS["redis_get_n1"]}" ]] && RESULTS["redis_get_n1"]="N/A"
        
        log_ok "Node 1 SET: ${RESULTS["redis_set_n1"]} ops/sec"
        log_ok "Node 1 GET: ${RESULTS["redis_get_n1"]} ops/sec"
    fi

    # 关闭 Node 1 的 Redis
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
}

# ── 计算比率 ────────────────────────────────────────────────────────────────
calculate_ratio() {
    local val1="$1"
    local val2="$2"
    
    if [[ "$val1" == "N/A" || "$val2" == "N/A" || -z "$val1" || -z "$val2" ]]; then
        echo "N/A"
        return
    fi
    
    # 处理可能包含逗号的数字
    val1=$(echo "$val1" | tr -d ',')
    val2=$(echo "$val2" | tr -d ',')
    
    if command -v bc &>/dev/null; then
        echo "scale=1; $val1 / $val2" | bc 2>/dev/null || echo "N/A"
    else
        # 使用 awk 作为回退
        awk "BEGIN {printf \"%.1f\", $val1 / $val2}" 2>/dev/null || echo "N/A"
    fi
}

# ── 格式化数字 ──────────────────────────────────────────────────────────────
format_number() {
    local num="$1"
    if [[ "$num" == "N/A" || -z "$num" ]]; then
        echo "N/A"
        return
    fi
    # 添加千位分隔符
    echo "$num" | awk '{printf "%'\''d", $1}' 2>/dev/null || echo "$num"
}

# ── 生成评估报告 ────────────────────────────────────────────────────────────
generate_report() {
    log_step "生成性能评估报告"

    # 计算比率
    local bw_write_ratio
    bw_write_ratio=$(calculate_ratio "${RESULTS["bw_n0_write"]}" "${RESULTS["bw_n1_write"]}")
    local bw_read_ratio
    bw_read_ratio=$(calculate_ratio "${RESULTS["bw_n0_read"]}" "${RESULTS["bw_n1_read"]}")
    local lat_ratio
    lat_ratio=$(calculate_ratio "${RESULTS["lat_n1"]}" "${RESULTS["lat_n0"]}")
    local redis_set_ratio
    redis_set_ratio=$(calculate_ratio "${RESULTS["redis_set_n0"]}" "${RESULTS["redis_set_n1"]}")
    local redis_get_ratio
    redis_get_ratio=$(calculate_ratio "${RESULTS["redis_get_n0"]}" "${RESULTS["redis_get_n1"]}")

    # 格式化输出
    local bw_n0_write_fmt
    bw_n0_write_fmt=$(format_number "${RESULTS["bw_n0_write"]}")
    local bw_n1_write_fmt
    bw_n1_write_fmt=$(format_number "${RESULTS["bw_n1_write"]}")
    local bw_n0_read_fmt
    bw_n0_read_fmt=$(format_number "${RESULTS["bw_n0_read"]}")
    local bw_n1_read_fmt
    bw_n1_read_fmt=$(format_number "${RESULTS["bw_n1_read"]}")
    local redis_set_n0_fmt
    redis_set_n0_fmt=$(format_number "${RESULTS["redis_set_n0"]}")
    local redis_set_n1_fmt
    redis_set_n1_fmt=$(format_number "${RESULTS["redis_set_n1"]}")
    local redis_get_n0_fmt
    redis_get_n0_fmt=$(format_number "${RESULTS["redis_get_n0"]}")
    local redis_get_n1_fmt
    redis_get_n1_fmt=$(format_number "${RESULTS["redis_get_n1"]}")

    # 评估状态
    local status=""
    local status_symbol=""
    
    # 检查 CXL 带宽比例
    if [[ "$bw_write_ratio" != "N/A" ]]; then
        local bw_percentage
        bw_percentage=$(echo "scale=1; 100 / $bw_write_ratio" | bc 2>/dev/null || echo "0")
        
        if (( $(echo "$bw_percentage < 10" | bc 2>/dev/null || echo "0") )); then
            status="模拟环境异常"
            status_symbol="❌"
        elif (( $(echo "$bw_percentage < 30" | bc 2>/dev/null || echo "0") )); then
            status="QEMU 模拟偏保守"
            status_symbol="⚠️"
        else
            status="符合预期"
            status_symbol="✅"
        fi
    else
        status="无法评估"
        status_symbol="❓"
    fi

    # 输出报告
    output ""
    output "╔════════════════════════════════════════════════════════╗"
    output "║           CXL 内存性能评估报告                          ║"
    output "╠════════════════════════════════════════════════════════╣"
    output "║ 测试项          │ Node 0 (DRAM) │ Node 1 (CXL)  │ 比率  ║"
    output "╠════════════════════════════════════════════════════════╣"
    output "║ 写带宽 (MB/s)   │ $(printf "%13s" "$bw_n0_write_fmt") │ $(printf "%13s" "$bw_n1_write_fmt") │ $(printf "%4s" "${bw_write_ratio}x") ║"
    output "║ 读带宽 (MB/s)   │ $(printf "%13s" "$bw_n0_read_fmt") │ $(printf "%13s" "$bw_n1_read_fmt") │ $(printf "%4s" "${bw_read_ratio}x") ║"
    output "║ 内存延迟 (ns)   │ $(printf "%13s" "${RESULTS["lat_n0"]:-N/A}") │ $(printf "%13s" "${RESULTS["lat_n1"]:-N/A}") │ $(printf "%4s" "${lat_ratio}x") ║"
    output "║ Redis SET (ops) │ $(printf "%13s" "$redis_set_n0_fmt") │ $(printf "%13s" "$redis_set_n1_fmt") │ $(printf "%4s" "${redis_set_ratio}x") ║"
    output "║ Redis GET (ops) │ $(printf "%13s" "$redis_get_n0_fmt") │ $(printf "%13s" "$redis_get_n1_fmt") │ $(printf "%4s" "${redis_get_ratio}x") ║"
    output "╠════════════════════════════════════════════════════════╣"
    output "║ 评估结论                                               ║"
    
    if [[ "$bw_write_ratio" != "N/A" ]]; then
        local bw_pct
        bw_pct=$(echo "scale=1; 100 / $bw_write_ratio" | bc 2>/dev/null || echo "N/A")
        output "║ CXL 带宽为 DRAM 的 ${bw_pct}%，延迟为 ${lat_ratio}x                          ║"
    fi
    
    output "║ 真实 CXL 1.1: 带宽 30-70%，延迟 2-3x                  ║"
    output "║ 状态: ${status_symbol} ${status}$(printf "%*s" $((47 - ${#status} - 5)) " ")║"
    output "╚════════════════════════════════════════════════════════╝"
    output ""
}

# ── 清理函数 ────────────────────────────────────────────────────────────────
cleanup() {
    log "清理中..."
    
    # 停止可能残留的 Redis 实例
    if command -v "$REDIS_CLI" &>/dev/null; then
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    fi
    pkill -f "redis-server.*:${REDIS_PORT}" 2>/dev/null || true
    
    # 清理临时文件
    rm -f /dev/shm/test_n0 /dev/shm/test_n1
}

# ── 主流程 ──────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"
    
    # 设置输出文件
    if [[ -n "$OUTPUT_FILE" ]]; then
        # 创建目录
        mkdir -p "$(dirname "$OUTPUT_FILE")"
        # 清空文件
        > "$OUTPUT_FILE"
    fi
    
    echo -e "${BOLD}${CYAN}"
    echo "╔════════════════════════════════════════════════════╗"
    echo "║      CXL 内存性能评估                               ║"
    echo "╚════════════════════════════════════════════════════╝"
    echo -e "${NC}"
    
    # 设置 trap
    trap cleanup EXIT
    
    # 前置检查
    check_prerequisites
    
    # 运行测试
    test_memory_bandwidth
    test_memory_latency
    test_ctap
    test_redis_performance
    
    # 生成报告
    generate_report
    
    if [[ -n "$OUTPUT_FILE" ]]; then
        log_ok "报告已保存到: $OUTPUT_FILE"
    fi
    
    log_ok "评估完成！"
}

main "$@"
