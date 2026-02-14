#!/bin/bash
#===============================================================================
# NUMA CXL 高压测试脚本
# 
# 环境：2 NUMA节点 (3GB + 8GB = 11GB CXL设备)
# 目标：充分利用内存至80%以上 (~8.8GB+)
# 
# 功能：
#   - 测试所有6种NUMA分配策略
#   - 多种数据类型混合测试
#   - NUMA节点内存分布监控
#   - 完整性能报告生成
#===============================================================================

set -e

#===============================================================================
# 配置参数
#===============================================================================
REDIS_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REDIS_SERVER="${REDIS_DIR}/src/redis-server"
REDIS_CLI="${REDIS_DIR}/src/redis-cli"
REDIS_BENCHMARK="${REDIS_DIR}/src/redis-benchmark"
REDIS_PORT=6379
REDIS_HOST="127.0.0.1"

# 内存配置 (单位: MB) - 保守设置避免OOM
TOTAL_MEMORY_MB=11264              # 11GB 总内存
TARGET_USAGE_PERCENT=50            # 降低到50%避免OOM (约5.5GB)
TARGET_MEMORY_MB=$((TOTAL_MEMORY_MB * TARGET_USAGE_PERCENT / 100))  # ~5632MB

# 测试参数 - 降低压力避免OOM
TEST_DURATION=30                   # 每阶段测试时长(秒)
PARALLEL_CLIENTS=20                # 并行客户端数
PIPELINE_DEPTH=8                   # 管道深度
REPORT_DIR="${REDIS_DIR}/tests/reports"
REPORT_FILE="${REPORT_DIR}/numa_stress_$(date +%Y%m%d_%H%M%S).txt"
LOG_FILE="${REPORT_DIR}/redis_stress_$(date +%Y%m%d_%H%M%S).log"

# 诊断配置
ENABLE_PERF=1                      # 启用perf性能分析
ENABLE_MEMORY_MONITOR=1            # 启用内存监控
PERF_DURATION=10                   # perf采样时长(秒)
DIAG_DIR="${REPORT_DIR}/diagnosis_$(date +%Y%m%d_%H%M%S)"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

#===============================================================================
# 工具函数
#===============================================================================

log_info() {
    echo -e "${GREEN}[INFO]${NC} $(date '+%H:%M:%S') $*"
    echo "[INFO] $(date '+%H:%M:%S') $*" >> "$REPORT_FILE"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $(date '+%H:%M:%S') $*"
    echo "[WARN] $(date '+%H:%M:%S') $*" >> "$REPORT_FILE"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $(date '+%H:%M:%S') $*"
    echo "[ERROR] $(date '+%H:%M:%S') $*" >> "$REPORT_FILE"
}

log_section() {
    echo ""
    echo -e "${CYAN}========== $* ==========${NC}"
    echo "" >> "$REPORT_FILE"
    echo "========== $* ==========" >> "$REPORT_FILE"
}

#===============================================================================
# 清理函数
#===============================================================================

cleanup() {
    local exit_code=$?
    echo ""
    echo -e "${YELLOW}[WARN]${NC} $(date '+%H:%M:%S') 脚本退出，执行清理..."
    
    # 停止内存监控
    if [ -f "${DIAG_DIR}/monitor.pid" ]; then
        local monitor_pid=$(cat "${DIAG_DIR}/monitor.pid" 2>/dev/null)
        if [ -n "$monitor_pid" ] && kill -0 "$monitor_pid" 2>/dev/null; then
            kill "$monitor_pid" 2>/dev/null || true
            echo -e "${GREEN}[INFO]${NC} $(date '+%H:%M:%S') 已停止内存监控进程 (PID=$monitor_pid)"
        fi
        rm -f "${DIAG_DIR}/monitor.pid"
    fi
    
    # 清理临时文件
    if [ -n "${DIAG_DIR}" ] && [ -d "${DIAG_DIR}" ]; then
        rm -f "${DIAG_DIR}"/*.pid 2>/dev/null || true
    fi
    
    echo -e "${GREEN}[INFO]${NC} $(date '+%H:%M:%S') 清理完成，退出码: $exit_code"
    exit $exit_code
}

# 注册trap处理器
trap cleanup EXIT INT TERM

# 获取Redis内存使用(MB)
get_memory_mb() {
    local rss_kb
    rss_kb=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | \
             grep "used_memory_rss:" | cut -d: -f2 | tr -d '\r\n')
    if [ -z "$rss_kb" ] || [ "$rss_kb" = "0" ]; then
        echo "0"
    else
        echo $((rss_kb / 1024 / 1024))
    fi
}

# 获取Redis key数量
get_key_count() {
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" DBSIZE 2>/dev/null | \
        grep -oE '[0-9]+' | head -1 || echo "0"
}

# 获取NUMA内存分布
get_numa_stats() {
    local redis_pid
    redis_pid=$(pgrep -f "redis-server.*$REDIS_PORT" | head -1)
    if [ -n "$redis_pid" ]; then
        # 添加超时防止卡住
        timeout 5 numastat -p "$redis_pid" 2>/dev/null || echo "numastat timeout or unavailable"
    else
        echo "Redis process not found"
    fi
}

#===============================================================================
# 诊断工具函数
#===============================================================================

# 获取Redis进程PID
get_redis_pid() {
    pgrep -f "redis-server.*${REDIS_PORT}" | head -1
}

# 检查诊断工具是否可用
check_diag_tool() {
    local tool="$1"
    if command -v "$tool" &>/dev/null; then
        return 0
    else
        log_warn "诊断工具 $tool 不可用"
        return 1
    fi
}

# 使用perf分析性能热点
run_perf_analysis() {
    local duration="${1:-$PERF_DURATION}"
    local pid=$(get_redis_pid)
    
    if [ -z "$pid" ]; then
        log_error "Redis进程未运行，跳过perf分析"
        return 1
    fi
    
    if ! check_diag_tool perf; then
        return 1
    fi
    
    log_section "Perf性能分析 (${duration}秒)"
    
    mkdir -p "$DIAG_DIR"
    
    # CPU性能分析
    log_info "采集CPU性能数据..."
    perf record -F 99 -p "$pid" -g -o "${DIAG_DIR}/perf_cpu.data" -- sleep "$duration" 2>&1 | tee -a "$REPORT_FILE"
    
    # 生成报告
    log_info "生成perf报告..."
    perf report -i "${DIAG_DIR}/perf_cpu.data" --stdio > "${DIAG_DIR}/perf_cpu_report.txt" 2>&1
    
    # 显示top 20热点函数
    log_info "Top 20 CPU热点函数:"
    perf report -i "${DIAG_DIR}/perf_cpu.data" --stdio -n --sort overhead,symbol 2>/dev/null | \
        grep -A 20 "^#" | head -25 | tee -a "$REPORT_FILE"
    
    # 内存访问分析 (需要root权限)
    if [ "$(id -u)" -eq 0 ]; then
        log_info "采集内存访问数据..."
        perf mem record -p "$pid" -o "${DIAG_DIR}/perf_mem.data" -- sleep 5 2>&1 | tee -a "$REPORT_FILE" || true
        
        if [ -f "${DIAG_DIR}/perf_mem.data" ]; then
            perf mem report -i "${DIAG_DIR}/perf_mem.data" --stdio > "${DIAG_DIR}/perf_mem_report.txt" 2>&1 || true
        fi
    fi
    
    log_info "Perf数据已保存到: $DIAG_DIR"
}

# 启动后台内存监控
start_memory_monitor() {
    local interval="${1:-2}"
    local pid=$(get_redis_pid)
    
    if [ -z "$pid" ]; then
        log_error "Redis进程未运行，跳过内存监控"
        return 1
    fi
    
    log_info "启动内存监控 (PID=$pid, 间隔=${interval}秒)"
    
    mkdir -p "$DIAG_DIR"
    
    # 后台监控脚本
    (
        echo "Timestamp,RSS_MB,VSZ_MB,CPU%,Keys,UsedMem_MB" > "${DIAG_DIR}/memory_timeline.csv"
        while kill -0 "$pid" 2>/dev/null; do
            # 进程内存统计
            read -r rss vsz cpu < <(ps -p "$pid" -o rss=,vsz=,pcpu= 2>/dev/null | awk '{print $1/1024, $2/1024, $3}')
            
            # Redis内存统计
            local keys=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" DBSIZE 2>/dev/null | grep -oE '[0-9]+' || echo 0)
            local used_mem=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | \
                grep "used_memory:" | cut -d: -f2 | tr -d '\r\n')
            local used_mem_mb=$((used_mem / 1024 / 1024))
            
            echo "$(date +%s),$rss,$vsz,$cpu,$keys,$used_mem_mb" >> "${DIAG_DIR}/memory_timeline.csv"
            sleep "$interval"
        done
    ) &
    
    local monitor_pid=$!
    echo "$monitor_pid" > "${DIAG_DIR}/monitor.pid"
    log_info "内存监控已启动 (监控PID=$monitor_pid)"
}

# 停止内存监控
stop_memory_monitor() {
    if [ -f "${DIAG_DIR}/monitor.pid" ]; then
        local monitor_pid=$(cat "${DIAG_DIR}/monitor.pid")
        if kill -0 "$monitor_pid" 2>/dev/null; then
            kill "$monitor_pid" 2>/dev/null || true
            log_info "内存监控已停止"
        fi
        rm -f "${DIAG_DIR}/monitor.pid"
    fi
}

# 收集内存池统计信息
collect_pool_stats() {
    log_section "内存池统计信息"
    
    local pid=$(get_redis_pid)
    if [ -z "$pid" ]; then
        log_error "Redis进程未运行"
        return 1
    fi
    
    mkdir -p "$DIAG_DIR"
    
    # Redis内存详细信息
    log_info "Redis内存详情:"
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | \
        tee "${DIAG_DIR}/redis_memory_info.txt" | \
        grep -E "used_memory|mem_fragmentation|allocator" | tee -a "$REPORT_FILE"
    
    # 进程内存映射
    log_info "进程内存映射统计:"
    cat "/proc/$pid/smaps" 2>/dev/null | \
        awk '/^Size:|^Rss:|^Pss:|^Shared_Clean:|^Shared_Dirty:|^Private_Clean:|^Private_Dirty:/ {sum[$1]+=$2} END {for(i in sum) print i, sum[i], "kB"}' | \
        tee "${DIAG_DIR}/smaps_summary.txt" | tee -a "$REPORT_FILE"
    
    # NUMA内存分布
    if check_diag_tool numastat; then
        log_info "NUMA内存分布:"
        numastat -p "$pid" 2>/dev/null | tee "${DIAG_DIR}/numa_distribution.txt" | tee -a "$REPORT_FILE"
    fi
}

# 分析内存碎片
analyze_memory_fragmentation() {
    log_section "内存碎片分析"
    
    local pid=$(get_redis_pid)
    if [ -z "$pid" ]; then
        log_error "Redis进程未运行"
        return 1
    fi
    
    mkdir -p "$DIAG_DIR"
    
    # Redis碎片率
    local frag_ratio=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | \
        grep "mem_fragmentation_ratio:" | cut -d: -f2 | tr -d '\r\n')
    
    log_info "内存碎片率: $frag_ratio"
    echo "Fragmentation_Ratio: $frag_ratio" >> "${DIAG_DIR}/fragmentation.txt"
    
    # 详细的内存块分布（如果有的话）
    if [ -f "/proc/$pid/smaps" ]; then
        log_info "分析内存块分布..."
        awk '/^Size:/ {size=$2} /^Rss:/ {rss=$2; if(size>0) print size, rss, (rss*100/size)}' \
            "/proc/$pid/smaps" > "${DIAG_DIR}/memory_blocks.txt"
        
        # 统计大块内存
        local large_blocks=$(awk '$1 > 65536 {count++} END {print count+0}' "${DIAG_DIR}/memory_blocks.txt")
        log_info "大内存块(>64MB)数量: $large_blocks"
    fi
}

# 使用strace跟踪系统调用
run_strace_analysis() {
    local duration="${1:-5}"
    local pid=$(get_redis_pid)
    
    if [ -z "$pid" ]; then
        log_error "Redis进程未运行"
        return 1
    fi
    
    if ! check_diag_tool strace; then
        return 1
    fi
    
    log_section "Strace系统调用分析 (${duration}秒)"
    
    mkdir -p "$DIAG_DIR"
    
    log_info "跟踪系统调用..."
    timeout "$duration" strace -p "$pid" -c -f -e trace=memory,process 2>"${DIAG_DIR}/strace_summary.txt" || true
    
    if [ -f "${DIAG_DIR}/strace_summary.txt" ]; then
        log_info "系统调用统计:"
        cat "${DIAG_DIR}/strace_summary.txt" | tee -a "$REPORT_FILE"
    fi
}

# 生成诊断报告
generate_diagnosis_report() {
    log_section "生成诊断报告"
    
    local report="${DIAG_DIR}/DIAGNOSIS_SUMMARY.txt"
    
    {
        echo "==============================================="
        echo "  Redis NUMA内存池诊断报告"
        echo "  生成时间: $(date)"
        echo "==============================================="
        echo ""
        echo "=== 系统环境 ==="
        echo "操作系统: $(uname -s) $(uname -r)"
        echo "CPU核心: $(nproc)"
        free -h
        echo ""
        
        if check_diag_tool numactl; then
            echo "=== NUMA拓扑 ==="
            numactl --hardware
            echo ""
        fi
        
        echo "=== Redis配置 ==="
        echo "目标内存: ${TARGET_MEMORY_MB}MB (${TARGET_USAGE_PERCENT}%)"
        echo "并发客户端: $PARALLEL_CLIENTS"
        echo "管道深度: $PIPELINE_DEPTH"
        echo ""
        
        echo "=== 诊断数据文件 ==="
        ls -lh "$DIAG_DIR"
        echo ""
        
        echo "=== 关键发现 ==="
        echo "1. 内存碎片率:"
        cat "${DIAG_DIR}/fragmentation.txt" 2>/dev/null || echo "  未采集"
        echo ""
        
        echo "2. CPU热点 (Top 10):"
        if [ -f "${DIAG_DIR}/perf_cpu_report.txt" ]; then
            grep -A 15 "^#" "${DIAG_DIR}/perf_cpu_report.txt" | head -20
        else
            echo "  未采集"
        fi
        echo ""
        
        echo "3. 系统调用开销:"
        cat "${DIAG_DIR}/strace_summary.txt" 2>/dev/null || echo "  未采集"
        echo ""
        
        echo "=== 建议 ==="
        echo "1. 查看 perf_cpu_report.txt 分析CPU热点函数"
        echo "2. 查看 memory_timeline.csv 分析内存增长趋势"
        echo "3. 查看 numa_distribution.txt 分析NUMA节点分布是否均衡"
        echo "4. 如果碎片率>1.5，考虑优化内存池chunk大小"
        echo "5. 如果有大量numa_alloc_onnode调用，考虑增大内存池"
        echo ""
        
    } > "$report" 2>&1
    
    log_info "诊断报告已保存: $report"
    
    # 只输出关键发现，避免卡住
    echo ""
    echo "=== 关键发现 ==="
    echo "1. 内存碎片率:"
    cat "${DIAG_DIR}/fragmentation.txt" 2>/dev/null || echo "  未采集"
    echo ""
    echo "2. 内存使用趋势:"
    local csv_file="${DIAG_DIR}/memory_timeline.csv"
    if [ -f "$csv_file" ]; then
        local sample_count=$(wc -l < "$csv_file")
        local final_state=$(tail -1 "$csv_file")
        echo "  总采样点: $sample_count"
        echo "  最终状态: $final_state"
    else
        echo "  未采集"
    fi
    echo ""
    echo "详细报告: $report"
}

# 获取NUMACONFIG统计
get_numa_config_stats() {
    # 添加超时防止卡住
    timeout 5 "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMACONFIG STATS 2>/dev/null || echo "NUMACONFIG STATS timeout or unavailable"
}

# 获取NUMACONFIG当前配置
get_numa_config() {
    # 添加超时防止卡住
    timeout 5 "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMACONFIG GET 2>/dev/null || echo "NUMACONFIG GET timeout or unavailable"
}

# 设置NUMA策略
set_numa_strategy() {
    local strategy="$1"
    timeout 5 "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMACONFIG SET strategy "$strategy" 2>/dev/null || log_warn "SET strategy timeout"
}

# 设置节点权重
set_node_weight() {
    local node="$1"
    local weight="$2"
    timeout 5 "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMACONFIG SET weight "$node" "$weight" 2>/dev/null || log_warn "SET weight timeout"
}

# 检查Redis是否运行
check_redis() {
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING 2>/dev/null | grep -q "PONG"
}

# 清空数据库
flush_db() {
    log_info "执行FLUSHALL..."
    timeout 10 "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" FLUSHALL 2>/dev/null || log_warn "FLUSHALL timeout"
}

# 等待内存稳定
wait_memory_stable() {
    local prev_mem=0
    local curr_mem
    local stable_count=0
    
    while [ $stable_count -lt 3 ]; do
        sleep 2
        curr_mem=$(get_memory_mb)
        if [ "$curr_mem" = "$prev_mem" ]; then
            stable_count=$((stable_count + 1))
        else
            stable_count=0
        fi
        prev_mem=$curr_mem
    done
}

#===============================================================================
# 数据生成函数 (充分利用redis-benchmark)
#===============================================================================

# 生成STRING数据 (使用redis-benchmark)
generate_string_data() {
    local count="$1"
    local value_size="$2"
    local prefix="$3"
    
    log_info "生成 $count 个STRING对象 (value_size=${value_size}B, prefix=${prefix})"
    
    # -r: 随机key范围, -d: value大小, -P: 管道深度, -n: 请求数
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set -n "$count" -r "$count" -d "$value_size" \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | tail -1 || true
}

# 生成HASH数据
generate_hash_data() {
    local count="$1"
    local fields="$2"
    
    log_info "生成 $count 个HASH对象 (fields=${fields})"
    
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t hset -n "$count" -r "$count" \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | tail -1 || true
}

# 生成LIST数据
generate_list_data() {
    local count="$1"
    local elements="$2"
    
    log_info "生成 $count 个LIST对象 (elements=${elements})"
    
    local total_ops=$((count * elements))
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t lpush -n "$total_ops" -r "$count" \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | tail -1 || true
}

# 生成SET数据
generate_set_data() {
    local count="$1"
    local members="$2"
    
    log_info "生成 $count 个SET对象 (members=${members})"
    
    local total_ops=$((count * members))
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t sadd -n "$total_ops" -r "$count" \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | tail -1 || true
}

# 生成ZSET数据
generate_zset_data() {
    local count="$1"
    local members="$2"
    
    log_info "生成 $count 个ZSET对象 (members=${members})"
    
    local total_ops=$((count * members))
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t zadd -n "$total_ops" -r "$count" \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | tail -1 || true
}

#===============================================================================
# 性能测试函数
#===============================================================================

# 综合性能测试
run_benchmark_suite() {
    local label="$1"
    local requests="${2:-100000}"
    
    log_info "运行性能测试套件: $label ($requests 请求)"
    
    local result
    result=$("$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -n "$requests" -r 1000000 \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        -t set,get,incr,lpush,rpush,lpop,rpop,sadd,hset,spop,zadd,zpopmin \
        --csv 2>/dev/null)
    
    echo "$result" >> "$REPORT_FILE"
    echo "$result" | head -15
}

# 单类型性能测试
run_single_benchmark() {
    local test_type="$1"
    local requests="${2:-50000}"
    local data_size="${3:-256}"
    
    log_info "单类型测试: $test_type ($requests 请求, ${data_size}B)"
    
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t "$test_type" -n "$requests" -r 1000000 \
        -d "$data_size" -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | grep -v "^$" | tail -1
}

# 混合读写测试
run_mixed_benchmark() {
    local duration="$1"
    local ratio_read="${2:-70}"  # 读比例
    
    log_info "混合读写测试: ${duration}s (读:写 = ${ratio_read}:$((100-ratio_read)))"
    
    local read_ops=$((100000 * ratio_read / 100))
    local write_ops=$((100000 * (100 - ratio_read) / 100))
    
    # 并行执行读写
    (
        "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
            -t get -n "$read_ops" -r 1000000 \
            -c $((PARALLEL_CLIENTS / 2)) -P "$PIPELINE_DEPTH" \
            --csv 2>/dev/null
    ) &
    local read_pid=$!
    
    (
        "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
            -t set -n "$write_ops" -r 1000000 \
            -c $((PARALLEL_CLIENTS / 2)) -P "$PIPELINE_DEPTH" \
            --csv 2>/dev/null
    ) &
    local write_pid=$!
    
    wait $read_pid $write_pid 2>/dev/null || true
}

#===============================================================================
# 高压填充函数
#===============================================================================

# 填充内存到目标使用率 (一次性大容量填充)
fill_memory_to_target() {
    local target_mb="$1"
    
    log_section "内存填充阶段 (目标: ${target_mb}MB, 渐进式填充避免OOM)"
    
    local current_mb=$(get_memory_mb)
    log_info "当前内存: ${current_mb}MB, 目标: ${target_mb}MB"
    
    # 如果已经接近目标，直接返回
    if [ "$current_mb" -ge "$((target_mb * 90 / 100))" ]; then
        log_info "内存已接近目标 ($(((current_mb * 100) / target_mb))%)，无需填充"
        return
    fi
    
    local need_fill_mb=$((target_mb - current_mb))
    log_info "需要填充: ${need_fill_mb}MB"
    
    # 使用小批次逐步填充,避免OOM
    local batch_size=50000   # 5万key批次 (降低10倍)
    local value_size=2048    # 2KB value大小
    local total_keys=$((need_fill_mb * 1024 * 1024 / value_size))
    
    log_warn "采用保守填充策略避免OOM: 批次=${batch_size}, value=${value_size}B"
    log_info "计划填充: ${total_keys} 个 ${value_size}B 的key"
    
    # 启动内存监控
    if [ "$ENABLE_MEMORY_MONITOR" -eq 1 ]; then
        start_memory_monitor 2
    fi
    
    # 分批执行，每批5万key
    local filled_keys=0
    local fill_start=$(date +%s)
    
    while [ "$filled_keys" -lt "$total_keys" ]; do
        local this_batch=$((total_keys - filled_keys))
        if [ "$this_batch" -gt "$batch_size" ]; then
            this_batch=$batch_size
        fi
        
        # 适中的随机范围,避免内存爆炸
        local random_range=$((this_batch * 5))
        if [ "$random_range" -lt 500000 ]; then
            random_range=500000
        fi
        
        log_info "填充批次: ${this_batch} keys (range: ${random_range})"
        
        # 检查内存是否接近限制
        local current_mem=$(get_memory_mb)
        if [ "$current_mem" -ge "$((target_mb * 95 / 100))" ]; then
            log_warn "内存已接近目标(${current_mem}MB/${target_mb}MB),停止填充"
            break
        fi
        
        "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
            -t set -n "$this_batch" -r "$random_range" -d "$value_size" \
            -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
            --csv 2>/dev/null | tail -1 || true
        
        filled_keys=$((filled_keys + this_batch))
        local progress_pct=$((filled_keys * 100 / total_keys))
        log_info "已填充: $filled_keys/$total_keys keys (${progress_pct}%)"
        
        # 批次间短暂休息,让系统稳定
        sleep 0.5
    done
    
    local fill_duration=$(($(date +%s) - fill_start))
    
    # 等待内存稳定
    sleep 3
    local final_mb=$(get_memory_mb)
    local final_keys=$(get_key_count)
    log_info "填充完成: ${final_mb}MB / ${target_mb}MB ($(((final_mb * 100) / target_mb))%), Keys: $final_keys, 耗时: ${fill_duration}s"
    
    # 收集填充后的内存池统计
    if [ "$ENABLE_MEMORY_MONITOR" -eq 1 ]; then
        collect_pool_stats
        analyze_memory_fragmentation
    fi
}

#===============================================================================
# 策略测试函数
#===============================================================================

# 测试单个NUMA策略
test_numa_strategy() {
    local strategy="$1"
    local test_requests="${2:-100000}"
    
    log_section "测试NUMA策略: $strategy"
    
    # 清理现有数据，确保纯净测试环境
    log_info "清理现有数据..."
    flush_db
    sleep 2
    
    # 验证数据库已清空
    local initial_keys=$(get_key_count)
    local initial_mem=$(get_memory_mb)
    log_info "初始状态: ${initial_keys} keys, ${initial_mem}MB"
    
    # 设置策略
    set_numa_strategy "$strategy"
    sleep 1
    
    # 显示当前配置
    log_info "当前配置:"
    get_numa_config
    
    local start_time=$(date +%s)
    
    # 填充2GB数据
    fill_memory_to_target 2048
    
    local fill_time=$(($(date +%s) - start_time))
    log_info "填充耗时: ${fill_time}s"
    
    # 运行性能测试
    run_benchmark_suite "$strategy" "$test_requests"
    
    # 记录NUMA分布
    log_info "NUMA分配统计:"
    get_numa_config_stats
    
    log_info "系统NUMA统计:"
    get_numa_stats
    
    # 混合读写测试
    run_mixed_benchmark 30 70
    
    # 测试完成后再次清理数据
    log_info "清理测试数据..."
    flush_db
    sleep 2
    
    local final_keys=$(get_key_count)
    local final_mem=$(get_memory_mb)
    log_info "清理后状态: ${final_keys} keys, ${final_mem}MB"
    
    # 返回性能数据
    echo "Strategy: $strategy, Fill: ${fill_time}s, Final_Mem: ${final_mem}MB"
}

# 测试加权策略的不同配置
test_weighted_configurations() {
    log_section "测试WEIGHTED策略不同权重配置"
    
    local configs=(
        "100:100"   # 均衡
        "200:100"   # 节点0优先 (模拟本地DRAM优先)
        "100:200"   # 节点1优先 (模拟CXL优先)
        "150:50"    # 3:1比例
    )
    
    for config in "${configs[@]}"; do
        local w0=$(echo "$config" | cut -d: -f1)
        local w1=$(echo "$config" | cut -d: -f2)
        
        log_info "测试权重配置: 节点0=$w0, 节点1=$w1"
        
        # 每次测试前都清理数据
        log_info "  清理上一轮测试数据..."
        flush_db
        sleep 1
        
        set_numa_strategy "weighted"
        set_node_weight 0 "$w0"
        set_node_weight 1 "$w1"
        
        # 填充1GB测试
        fill_memory_to_target 1024
        
        log_info "权重 $config 结果:"
        get_numa_config_stats
        
        # 性能测试
        run_single_benchmark "set" 50000 512
        run_single_benchmark "get" 50000 512
        
        # 测试后清理数据
        log_info "  清理本轮测试数据..."
        flush_db
        sleep 1
    done
    
    log_info "加权策略测试完成，最终清理..."
    flush_db
    sleep 2
}

# 阶段4: 大key混合测试
test_large_keys_mixed() {
    log_section "大Key混合压力测试"
    
    # 保持总内存压力约2GB，但使用不同大小的key
    local total_memory_target=2048  # MB
    
    # 测试配置: [value_size, key_count, description]
    local configs=(
        "1024:50000:小key密集访问 (1KB x 50K = 50MB)"
        "4096:12500:中等key平衡 (4KB x 12.5K = 50MB)"
        "16384:3125:大key稀疏访问 (16KB x 3.1K = 50MB)"
        "65536:781:超大key极稀疏 (64KB x 781 = 50MB)"
        "262144:195:巨型key极少 (256KB x 195 = 50MB)"
        "1048576:48:超巨型key (1MB x 48 = 48MB)"
    )
    
    # 混合测试：同时创建不同大小的key
    log_info "混合大key压力测试 (总目标: ${total_memory_target}MB)"
    
    # 第一轮：创建各种大小的key
    for config in "${configs[@]}"; do
        local size=$(echo "$config" | cut -d: -f1)
        local count=$(echo "$config" | cut -d: -f2)
        local desc=$(echo "$config" | cut -d: -f3)
        
        log_info "创建 $desc"
        
        # 增大随机数范围减少重复
        local random_range=$((count * 10))
        if [ "$random_range" -lt 1000000 ]; then
            random_range=1000000
        fi
        
        "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
            -t set -n "$count" -r "$random_range" -d "$size" \
            -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
            --csv 2>/dev/null | tail -1
    done
    
    # 第二轮：混合访问所有key
    log_info "混合访问所有大key..."
    
    # 使用较小的请求数避免超时
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t get,set -n 50000 -r 100000 \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | tail -2
    
    # 第三轮：热点大key测试
    log_info "热点大key访问测试..."
    
    # 针对最大的几个key进行高频访问
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t get,set -n 100000 -r 100 \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null | tail -2
    
    local final_mem=$(get_memory_mb)
    local final_keys=$(get_key_count)
    log_info "大key测试完成: ${final_mem}MB, ${final_keys} keys"
}

#===============================================================================
# 超高压测试
#===============================================================================

run_ultra_high_pressure_test() {
    log_section "超高压测试 (目标: 80%+ 内存使用)"
    
    # 使用压力感知策略
    set_numa_strategy "pressure_aware"
    log_info "设置策略: pressure_aware"
    
    flush_db
    sleep 2
    
    local start_time=$(date +%s)
    
    # 阶段1: 大规模数据填充
    log_info "=== 阶段1: 大规模数据填充 ==="
    fill_memory_to_target "$TARGET_MEMORY_MB"
    
    local phase1_time=$(($(date +%s) - start_time))
    local phase1_mem=$(get_memory_mb)
    local phase1_keys=$(get_key_count)
    log_info "阶段1完成: ${phase1_mem}MB, ${phase1_keys} keys, 耗时${phase1_time}s"
    
    # 阶段2: 高并发读写压力
    log_info "=== 阶段2: 高并发读写压力 (${TEST_DURATION}s) ==="
    local phase2_start=$(date +%s)
    
    while [ $(($(date +%s) - phase2_start)) -lt "$TEST_DURATION" ]; do
        # 混合操作
        "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
            -t set,get,incr,lpush,rpush,sadd,hset,zadd \
            -n 50000 -r 1000000 \
            -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
            --csv 2>/dev/null | tail -1
        
        local curr_mem=$(get_memory_mb)
        log_info "压力测试中... 内存: ${curr_mem}MB"
    done
    
    # 阶段3: 热点访问模式
    log_info "=== 阶段3: 热点访问模式 ==="
    # 创建热点数据（少量key高频访问）
    "$REDIS_BENCHMARK" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t get,set -n 200000 -r 1000 \
        -c "$PARALLEL_CLIENTS" -P "$PIPELINE_DEPTH" \
        --csv 2>/dev/null
    
    # 阶段4: 大key混合测试
    log_info "=== 阶段4: 大key混合压力测试 ==="
    test_large_keys_mixed
    
    # 阶段5: 触发负载均衡
    log_info "=== 阶段5: 负载均衡测试 ==="
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMACONFIG REBALANCE
    sleep 5
    
    # 最终状态
    local total_time=$(($(date +%s) - start_time))
    local final_mem=$(get_memory_mb)
    local final_keys=$(get_key_count)
    
    log_section "超高压测试结果"
    log_info "总耗时: ${total_time}s"
    log_info "最终内存: ${final_mem}MB / ${TARGET_MEMORY_MB}MB ($(((final_mem * 100) / TARGET_MEMORY_MB))%)"
    log_info "总Key数: $final_keys"
    log_info "NUMA分配统计:"
    get_numa_config_stats
    log_info "系统NUMA统计:"
    get_numa_stats
}

#===============================================================================
# 完整性能报告
#===============================================================================

generate_report() {
    log_section "生成完整性能报告"
    
    {
        echo "=============================================="
        echo "  Redis NUMA CXL 高压测试报告"
        echo "  生成时间: $(date)"
        echo "=============================================="
        echo ""
        echo "=== 测试环境 ==="
        echo "NUMA节点: 2 (3GB + 8GB = 11GB)"
        echo "目标内存使用: ${TARGET_USAGE_PERCENT}% (~${TARGET_MEMORY_MB}MB)"
        echo "并行客户端: $PARALLEL_CLIENTS"
        echo "管道深度: $PIPELINE_DEPTH"
        echo ""
        echo "=== Redis信息 ==="
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO server 2>/dev/null | head -10
        echo ""
        echo "=== 内存信息 ==="
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null
        echo ""
        echo "=== NUMA配置 ==="
        get_numa_config
        echo ""
        echo "=== NUMA分配统计 ==="
        get_numa_config_stats
        echo ""
        echo "=== 系统NUMA统计 ==="
        get_numa_stats
        echo ""
    } >> "$REPORT_FILE"
    
    log_info "报告已保存至: $REPORT_FILE"
}

#===============================================================================
# 主函数
#===============================================================================

main() {
    # 创建报告目录
    mkdir -p "$REPORT_DIR"
    mkdir -p "$DIAG_DIR"
    
    # 初始化报告
    {
        echo "=============================================="
        echo "  Redis NUMA CXL 高压测试日志"
        echo "  开始时间: $(date)"
        echo "  诊断模式: 已启用"
        echo "  - Perf分析: $([ $ENABLE_PERF -eq 1 ] && echo '已启用' || echo '未启用')"
        echo "  - 内存监控: $([ $ENABLE_MEMORY_MONITOR -eq 1 ] && echo '已启用' || echo '未启用')"
        echo "  - 诊断目录: $DIAG_DIR"
        echo "=============================================="
    } > "$REPORT_FILE"
    
    log_section "环境检查"
    
    # 检查Redis
    if ! check_redis; then
        log_error "Redis未运行，请先启动Redis服务器"
        log_info "启动命令: $REDIS_SERVER --loglevel verbose"
        exit 1
    fi
    log_info "Redis连接正常"
    
    # 检查NUMA
    if ! command -v numactl &> /dev/null; then
        log_warn "numactl未安装，部分NUMA统计将不可用"
    else
        log_info "NUMA节点信息:"
        numactl --hardware 2>/dev/null | head -10
    fi
    
    # 检查NUMACONFIG命令
    if ! "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMACONFIG HELP &>/dev/null; then
        log_warn "NUMACONFIG命令不可用，将跳过策略测试"
    fi
    
    # 显示初始状态
    log_info "初始内存: $(get_memory_mb)MB"
    log_info "初始Keys: $(get_key_count)"
    
    #---------------------------------------------------------------------------
    # 测试流程
    #---------------------------------------------------------------------------
    
    # 1. 基准性能测试
    log_section "基准性能测试"
    flush_db
    run_benchmark_suite "baseline" 100000
    
    # 1.5 性能热点分析
    if [ "$ENABLE_PERF" -eq 1 ]; then
        run_perf_analysis $PERF_DURATION
    fi
    
    # 2. 策略对比测试（如果可用）
    if "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMACONFIG HELP &>/dev/null; then
        local strategies=("local_first" "interleave" "round_robin" "weighted" "pressure_aware" "cxl_optimized")
        
        log_section "开始策略对比测试"
        
        for strategy in "${strategies[@]}"; do
            log_info "准备测试策略: $strategy"
            
            # 在每个策略测试前清理数据
            flush_db
            sleep 2
            
            test_numa_strategy "$strategy" 50000
            sleep 3
        done
        
        # 加权策略详细测试
        test_weighted_configurations
        
        # 所有策略测试完成后最终清理
        log_info "所有策略测试完成，执行最终清理..."
        flush_db
        sleep 2
    fi
    
    # 3. 超高压测试
    run_ultra_high_pressure_test
    
    # 3.5 停止内存监控
    if [ "$ENABLE_MEMORY_MONITOR" -eq 1 ]; then
        stop_memory_monitor
    fi
    
    # 3.6 最终strace分析(可选)
    if check_diag_tool strace; then
        run_strace_analysis 5
    fi
    
    # 4. 生成报告
    generate_report
    
    # 5. 生成诊断报告
    if [ "$ENABLE_PERF" -eq 1 ] || [ "$ENABLE_MEMORY_MONITOR" -eq 1 ]; then
        generate_diagnosis_report
    fi
    
    log_section "测试完成"
    log_info "报告文件: $REPORT_FILE"
    log_info "诊断目录: $DIAG_DIR"
    log_info "总内存使用: $(get_memory_mb)MB"
    log_info "总Key数量: $(get_key_count)"
}

#===============================================================================
# 快速测试模式
#===============================================================================

quick_test() {
    log_section "快速测试模式"
    
    mkdir -p "$REPORT_DIR"
    mkdir -p "$DIAG_DIR"
    echo "快速测试 $(date)" > "$REPORT_FILE"
    
    if ! check_redis; then
        log_error "Redis未运行"
        exit 1
    fi
    
    flush_db
    sleep 2
    
    # 快速填充1.5GB (降低目标避免OOM)
    log_info "快速填充1.5GB数据..."
    fill_memory_to_target 1536
    
    # 基础性能测试
    log_info "运行基础性能测试..."
    run_benchmark_suite "quick_test" 50000
    
    # 收集诊断数据
    if [ "$ENABLE_MEMORY_MONITOR" -eq 1 ]; then
        log_info "收集内存池统计..."
        collect_pool_stats
        analyze_memory_fragmentation
    fi
    
    log_info "快速测试完成"
    log_info "最终内存: $(get_memory_mb)MB"
    log_info "最终Keys: $(get_key_count)"
    log_info "NUMA配置:"
    get_numa_config_stats
    
    # 停止内存监控
    if [ "$ENABLE_MEMORY_MONITOR" -eq 1 ]; then
        stop_memory_monitor
    fi
    
    # 生成简单诊断报告
    if [ "$ENABLE_PERF" -eq 1 ] || [ "$ENABLE_MEMORY_MONITOR" -eq 1 ]; then
        generate_diagnosis_report
    fi
}

#===============================================================================
# 脚本入口
#===============================================================================

case "${1:-full}" in
    quick)
        quick_test
        ;;
    full)
        main
        ;;
    fill)
        mkdir -p "$REPORT_DIR"
        echo "填充测试 $(date)" > "$REPORT_FILE"
        fill_memory_to_target "${2:-$TARGET_MEMORY_MB}"
        ;;
    benchmark)
        mkdir -p "$REPORT_DIR"
        echo "性能测试 $(date)" > "$REPORT_FILE"
        run_benchmark_suite "manual" "${2:-100000}"
        ;;
    strategy)
        mkdir -p "$REPORT_DIR"
        echo "策略测试 $(date)" > "$REPORT_FILE"
        test_numa_strategy "${2:-pressure_aware}" 50000
        ;;
    *)
        echo "用法: $0 [命令] [参数]"
        echo ""
        echo "命令:"
        echo "  full       - 完整测试 (默认)"
        echo "  quick      - 快速测试 (4GB填充)"
        echo "  fill [MB]  - 仅填充内存到指定大小"
        echo "  benchmark [N] - 仅运行性能测试"
        echo "  strategy [name] - 测试指定策略"
        echo ""
        echo "示例:"
        echo "  $0 full              # 完整测试"
        echo "  $0 quick             # 快速测试"
        echo "  $0 fill 8000         # 填充8GB"
        echo "  $0 strategy weighted # 测试weighted策略"
        ;;
esac
