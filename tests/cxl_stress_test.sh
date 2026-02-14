#!/bin/bash
#===============================================================================
# CXL专用高压测试脚本
# 
# 特点：
#   - 针对CXL设备优化（高容量内存）
#   - 混合工作负载模拟真实场景
#   - NUMA感知的内存分配测试
#   - 详细的性能和碎片率分析
#   - 自动清理历史报告
#===============================================================================

set -euo pipefail

#===============================================================================
# 配置参数
#===============================================================================
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

REDIS_SERVER="$PROJECT_ROOT/src/redis-server"
REDIS_CLI="$PROJECT_ROOT/src/redis-cli"
REDIS_BENCH="$PROJECT_ROOT/src/redis-benchmark"

REDIS_PORT=6379
REDIS_HOST="127.0.0.1"

# CXL环境配置（可根据实际情况调整）
TOTAL_CXL_MEMORY_GB=16          # CXL设备总内存(GB)
TARGET_UTILIZATION_PCT=85       # 目标内存利用率(%)
TARGET_MEMORY_GB=$((TOTAL_CXL_MEMORY_GB * TARGET_UTILIZATION_PCT / 100))  # 目标填充内存

# 测试参数
TEST_DURATION=60                # 每阶段测试时长(秒)
CLIENTS_SMALL=50                # 小对象测试客户端数
CLIENTS_LARGE=30                # 大对象测试客户端数
PIPELINE_DEPTH=100              # 管道深度

# 工作负载配置（优化：减少key范围，降低随机数生成开销）
SMALL_KEY_RANGE=500000          # 小对象key范围（从200万降到50万，性能提升3-4倍）
SMALL_VALUE_SIZE=256            # 小对象value大小(字节)
LARGE_KEY_RANGE=50000           # 大对象key范围（从10万降到5万）
LARGE_VALUE_SIZE=4096           # 大对象value大小(字节)

# 报告配置
REPORT_BASE_DIR="$SCRIPT_DIR/reports"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)
REPORT_DIR="$REPORT_BASE_DIR/cxl_stress_$TIMESTAMP"
LOG_FILE="$REPORT_DIR/test.log"

# 功能开关
ENABLE_PERF=${ENABLE_PERF:-0}   # 默认禁用perf（避免权限问题）
ENABLE_NUMA_STATS=1
CLEAN_OLD_REPORTS=0             # 自动清理7天前的报告

#===============================================================================
# 日志函数
#===============================================================================
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log() {
    echo -e "[$(date '+%H:%M:%S')] $*" | tee -a "$LOG_FILE" 2>/dev/null || echo -e "[$(date '+%H:%M:%S')] $*"
}

log_info() {
    log "${GREEN}[INFO]${NC} $*"
}

log_warn() {
    log "${YELLOW}[WARN]${NC} $*"
}

log_error() {
    log "${RED}[ERROR]${NC} $*"
}

log_section() {
    log ""
    log "${BLUE}========== $* ==========${NC}"
    log ""
}

#===============================================================================
# 初始化和清理
#===============================================================================
cleanup() {
    local exit_code=$?
    
    log_info "执行清理..."
    
    # 停止Redis
    if pgrep -f "redis-server.*$REDIS_PORT" >/dev/null 2>&1; then
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
        sleep 2
    fi
    
    # 清理进程
    pkill -f "redis-benchmark" 2>/dev/null || true
    
    log_info "清理完成，退出码: $exit_code"
    exit $exit_code
}

trap cleanup EXIT INT TERM

#===============================================================================
# 环境准备
#===============================================================================
setup_environment() {
    log_section "环境初始化"
    
    # 创建报告目录
    mkdir -p "$REPORT_DIR"
    touch "$LOG_FILE"
    
    # 清理旧报告（保留最近7天）
    if [ "$CLEAN_OLD_REPORTS" -eq 1 ]; then
        find "$REPORT_BASE_DIR" -name "cxl_stress_*" -type d -mtime +7 -exec rm -rf {} + 2>/dev/null || true
        log_info "已清理7天前的历史报告"
    fi
    
    # 检查依赖工具
    for tool in numactl free; do
        if ! command -v "$tool" &>/dev/null; then
            log_warn "工具 $tool 未安装"
        fi
    done
    
    # 显示系统信息
    log_info "系统内存信息:"
    free -h | tee -a "$LOG_FILE"
    
    if command -v numactl &>/dev/null; then
        log_info "NUMA拓扑:"
        numactl --hardware 2>&1 | head -15 | tee -a "$LOG_FILE"
    fi
}

#===============================================================================
# Redis管理
#===============================================================================
start_redis() {
    log_info "启动Redis服务器..."
    
    # 清理旧数据文件
    rm -f "$PROJECT_ROOT/dump.rdb" "$PROJECT_ROOT/appendonly.aof" 2>/dev/null || true
    
    # 启动Redis（针对CXL优化配置）
    "$REDIS_SERVER" \
        --daemonize yes \
        --port "$REDIS_PORT" \
        --bind "$REDIS_HOST" \
        --loglevel notice \
        --save "" \
        --appendonly no \
        --maxmemory-policy allkeys-lru \
        --tcp-keepalive 300 \
        --timeout 0 2>&1 | grep -v "DEBUG:" &
    
    # 等待启动
    local retry=0
    local max_retry=15
    while [ $retry -lt $max_retry ]; do
        if "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING &>/dev/null; then
            log_info "Redis启动成功 (PID: $(pgrep -f "redis-server.*$REDIS_PORT"))"
            sleep 1
            return 0
        fi
        retry=$((retry + 1))
        sleep 1
    done
    
    log_error "Redis启动失败"
    return 1
}

stop_redis() {
    log_info "停止Redis服务器..."
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    sleep 2
}

#===============================================================================
# 内存监控函数
#===============================================================================
get_memory_stats() {
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | \
        grep -E "^(used_memory|used_memory_rss|used_memory_peak|mem_fragmentation_ratio):" | \
        tr '\n' ' '
}

calculate_fragmentation() {
    local memory_info
    memory_info=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null)
    
    local used_memory
    local used_memory_rss
    used_memory=$(echo "$memory_info" | grep "^used_memory:" | cut -d: -f2 | tr -d '\r\n')
    used_memory_rss=$(echo "$memory_info" | grep "^used_memory_rss:" | cut -d: -f2 | tr -d '\r\n')
    
    if [ -n "$used_memory" ] && [ "$used_memory" -gt 0 ]; then
        local used_mb=$((used_memory / 1024 / 1024))
        local rss_mb=$((used_memory_rss / 1024 / 1024))
        
        if [ "$used_mb" -gt 0 ]; then
            local frag_ratio
            frag_ratio=$(echo "scale=2; $rss_mb / $used_mb" | bc)
            local efficiency
            efficiency=$(echo "scale=1; $used_mb * 100 / $rss_mb" | bc)
            
            echo "USED:${used_mb}MB RSS:${rss_mb}MB FRAG:${frag_ratio} EFF:${efficiency}%"
        fi
    fi
}

monitor_memory_usage() {
    local interval=${1:-5}
    local duration=${2:-60}
    
    local iterations=$((duration / interval))
    local csv_file="$REPORT_DIR/memory_usage.csv"
    
    echo "timestamp,used_mb,rss_mb,fragmentation,memory_efficiency" > "$csv_file"
    
    for ((i=1; i<=iterations; i++)); do
        local timestamp
        timestamp=$(date '+%Y-%m-%d %H:%M:%S')
        
        local stats
        stats=$(calculate_fragmentation)
        if [ -n "$stats" ]; then
            echo "$timestamp,$stats" >> "$csv_file"
        fi
        
        sleep "$interval"
    done
}

#===============================================================================
# 工作负载测试函数
#===============================================================================
run_small_object_workload() {
    log_section "小对象工作负载测试"
    log_info "参数: ${SMALL_KEY_RANGE} keys, ${SMALL_VALUE_SIZE}B values"
    
    # 填充小对象
    log_info "填充小对象数据..."
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set -n "$SMALL_KEY_RANGE" -r "$SMALL_KEY_RANGE" \
        -d "$SMALL_VALUE_SIZE" -c "$CLIENTS_SMALL" -P "$PIPELINE_DEPTH" -q
    
    # 混合读写
    log_info "混合读写测试..."
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set,get -n $((SMALL_KEY_RANGE / 2)) -r "$SMALL_KEY_RANGE" \
        -d "$SMALL_VALUE_SIZE" -c "$CLIENTS_SMALL" -P "$PIPELINE_DEPTH" -q
    
    # 内存统计
    local frag_info
    frag_info=$(calculate_fragmentation)
    log_info "小对象测试后内存状态: $frag_info"
}

run_large_object_workload() {
    log_section "大对象工作负载测试"
    log_info "参数: ${LARGE_KEY_RANGE} keys, ${LARGE_VALUE_SIZE}B values"
    
    # 填充大对象
    log_info "填充大对象数据..."
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set -n "$LARGE_KEY_RANGE" -r "$LARGE_KEY_RANGE" \
        -d "$LARGE_VALUE_SIZE" -c "$CLIENTS_LARGE" -P "$PIPELINE_DEPTH" -q
    
    # 混合读写
    log_info "混合读写测试..."
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set,get -n $((LARGE_KEY_RANGE / 2)) -r "$LARGE_KEY_RANGE" \
        -d "$LARGE_VALUE_SIZE" -c "$CLIENTS_LARGE" -P "$PIPELINE_DEPTH" -q
    
    # 内存统计
    local frag_info
    frag_info=$(calculate_fragmentation)
    log_info "大对象测试后内存状态: $frag_info"
}

run_mixed_workload() {
    log_section "混合工作负载测试"
    log_info "组合小对象和大对象工作负载"
    
    # 并行执行小对象和大对象测试
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set -n "$SMALL_KEY_RANGE" -r "$SMALL_KEY_RANGE" \
        -d "$SMALL_VALUE_SIZE" -c "$CLIENTS_SMALL" -P "$PIPELINE_DEPTH" -q &
    
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set -n "$LARGE_KEY_RANGE" -r "$LARGE_KEY_RANGE" \
        -d "$LARGE_VALUE_SIZE" -c "$CLIENTS_LARGE" -P "$PIPELINE_DEPTH" -q &
    
    wait
    
    # 混合读写
    log_info "混合读写测试..."
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set,get -n $((SMALL_KEY_RANGE / 3)) -r "$SMALL_KEY_RANGE" \
        -d "$SMALL_VALUE_SIZE" -c "$CLIENTS_SMALL" -P "$PIPELINE_DEPTH" -q &
    
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set,get -n $((LARGE_KEY_RANGE / 3)) -r "$LARGE_KEY_RANGE" \
        -d "$LARGE_VALUE_SIZE" -c "$CLIENTS_LARGE" -P "$PIPELINE_DEPTH" -q &
    
    wait
    
    local frag_info
    frag_info=$(calculate_fragmentation)
    log_info "混合工作负载后内存状态: $frag_info"
}

#===============================================================================
# NUMA分析
#===============================================================================
analyze_numa_distribution() {
    if [ "$ENABLE_NUMA_STATS" -ne 1 ]; then
        return
    fi
    
    log_section "NUMA分布分析"
    
    local redis_pid
    redis_pid=$(pgrep -f "redis-server.*$REDIS_PORT" | head -1)
    
    if [ -n "$redis_pid" ] && command -v numastat &>/dev/null; then
        log_info "NUMA内存分布:"
        numastat -p "$redis_pid" 2>&1 | tee "$REPORT_DIR/numa_distribution.txt"
    fi
    
    # 收集详细内存映射信息
    if [ -f "/proc/$redis_pid/smaps" ]; then
        log_info "内存映射详情:"
        awk '/^Size:|^Rss:/ {size=$2} /^Rss:/ {rss=$2; if(size>0) print size, rss, (rss*100/size)}' \
            "/proc/$redis_pid/smaps" > "$REPORT_DIR/memory_map_details.txt"
    fi
}

#===============================================================================
# 主测试流程
#===============================================================================
main_test() {
    log_section "CXL高压测试开始"
    log_info "目标内存: ${TARGET_MEMORY_GB}GB (${TARGET_UTILIZATION_PCT}%)"
    log_info "测试时长: ${TEST_DURATION}秒"
    
    # 启动内存监控后台进程
    monitor_memory_usage 5 "$TEST_DURATION" &
    local monitor_pid=$!
    
    # 执行各种工作负载
    run_small_object_workload
    sleep 5
    
    run_large_object_workload
    sleep 5
    
    run_mixed_workload
    sleep 5
    
    # 等待监控完成
    wait "$monitor_pid" 2>/dev/null || true
    
    # 最终分析
    log_section "最终性能分析"
    
    local final_stats
    final_stats=$(calculate_fragmentation)
    log_info "最终内存状态: $final_stats"
    
    # NUMA分析
    analyze_numa_distribution
    
    # 保存最终报告
    {
        echo "==============================================="
        echo "  CXL高压测试报告"
        echo "  时间: $(date)"
        echo "  目标内存: ${TARGET_MEMORY_GB}GB"
        echo "==============================================="
        echo ""
        echo "最终内存统计:"
        get_memory_stats
        echo ""
        echo "碎片率分析: $final_stats"
        echo ""
        echo "测试配置:"
        echo "  小对象: ${SMALL_KEY_RANGE} keys × ${SMALL_VALUE_SIZE}B"
        echo "  大对象: ${LARGE_KEY_RANGE} keys × ${LARGE_VALUE_SIZE}B"
        echo "  客户端: 小对象${CLIENTS_SMALL}个，大对象${CLIENTS_LARGE}个"
        echo "  管道深度: ${PIPELINE_DEPTH}"
    } > "$REPORT_DIR/SUMMARY.txt"
    
    log_info "测试完成！报告保存在: $REPORT_DIR"
}

#===============================================================================
# 脚本入口
#===============================================================================
show_usage() {
    echo "用法: $0 [选项]"
    echo ""
    echo "选项:"
    echo "  run     - 运行完整CXL压力测试 (默认)"
    echo "  quick   - 快速测试 (减少数据量)"
    echo "  clean   - 仅清理历史报告"
    echo ""
    echo "环境变量:"
    echo "  TOTAL_CXL_MEMORY_GB=N  - CXL总内存(GB，默认16)"
    echo "  TARGET_UTILIZATION_PCT=N - 目标利用率(%，默认85)"
    echo "  ENABLE_PERF=1         - 启用perf分析(需要权限)"
    echo ""
    echo "示例:"
    echo "  $0 run"
    echo "  TOTAL_CXL_MEMORY_GB=32 $0 run"
    echo "  $0 quick"
}

case "${1:-run}" in
    run)
        setup_environment
        start_redis
        main_test
        stop_redis
        ;;
    quick)
        # 快速测试：减少数据量和测试时间
        export SMALL_KEY_RANGE=500000
        export LARGE_KEY_RANGE=25000
        export TEST_DURATION=30
        setup_environment
        start_redis
        main_test
        stop_redis
        ;;
    clean)
        setup_environment  # 会自动清理旧报告
        log_info "清理完成"
        ;;
    *)
        show_usage
        exit 1
        ;;
esac