#!/bin/bash
#
# YCSB 自动化压力测试框架
# 支持 NUMA 场景测试、自动 Redis 启动、结果收集

set -e

# 配置
YCSB_DIR="$(cd "$(dirname "$0")" && pwd)"
REDIS_DIR="$(dirname "$(dirname "$YCSB_DIR")")"
RESULTS_DIR="$YCSB_DIR/results"
WORKLOADS_DIR="$YCSB_DIR/workloads"

# Redis 配置
REDIS_PORT=6379
REDIS_HOST="127.0.0.1"
REDIS_PID=""

# 测试配置
TEST_WORKLOADS=("workload_numa_hotspot" "workload_numa_mixed" "workload_numa_write_heavy")
TEST_DURATION=300  # 5分钟
RECORD_COUNTS=(100000 500000)
THREAD_COUNTS=(50 100)

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 清理函数
cleanup() {
    echo -e "\n${YELLOW}清理测试环境...${NC}"
    
    if [ -n "$REDIS_PID" ]; then
        echo "停止 Redis 服务器..."
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT shutdown 2>/dev/null || true
        wait $REDIS_PID 2>/dev/null || true
    fi
    
    echo -e "${GREEN}清理完成${NC}"
}
trap cleanup EXIT

# 检查 YCSB 是否安装
check_ycsb() {
    if [ ! -d "$YCSB_DIR/ycsb-0.17.0" ]; then
        echo -e "${RED}YCSB 未安装${NC}"
        echo "请先运行: ./scripts/install_ycsb.sh"
        exit 1
    fi
}

# 启动 Redis
start_redis() {
    echo -e "${BLUE}=== 启动 Redis 服务器 ===${NC}"
    
    # 检查 Redis 是否已运行
    if "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT ping 2>/dev/null | grep -q "PONG"; then
        echo -e "${YELLOW}Redis 已在运行，跳过启动${NC}"
        return 0
    fi
    
    # 启动 Redis (NUMA 优化配置)
    "$REDIS_DIR/src/redis-server" \
        --daemonize yes \
        --port $REDIS_PORT \
        --bind $REDIS_HOST \
        --loglevel notice \
        --save "" \
        --appendonly no \
        --maxmemory 8gb \
        --maxmemory-policy allkeys-lru \
        2>&1 | tee "$RESULTS_DIR/redis_server.log"
    
    # 等待 Redis 就绪
    local retries=0
    while [ $retries -lt 30 ]; do
        if "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT ping 2>/dev/null | grep -q "PONG"; then
            echo -e "${GREEN}Redis 启动成功${NC}"
            return 0
        fi
        sleep 1
        retries=$((retries + 1))
    done
    
    echo -e "${RED}Redis 启动超时${NC}"
    exit 1
}

# 清空 Redis 数据
flush_redis() {
    echo -e "${YELLOW}清空 Redis 数据...${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT FLUSHALL > /dev/null
    echo -e "${GREEN}Redis 数据已清空${NC}"
}

# 收集系统信息
collect_system_info() {
    echo -e "\n${BLUE}=== 系统信息 ===${NC}"
    
    local info_file="$RESULTS_DIR/system_info_$(date +%Y%m%d_%H%M%S).txt"
    
    {
        echo "测试时间: $(date)"
        echo "主机名: $(hostname)"
        echo ""
        echo "=== CPU 信息 ==="
        cat /proc/cpuinfo | grep "model name" | head -1
        echo "CPU 核心数: $(nproc)"
        echo ""
        echo "=== 内存信息 ==="
        free -h
        echo ""
        echo "=== NUMA 信息 ==="
        if command -v numactl &> /dev/null; then
            numactl --hardware
        else
            echo "numactl 未安装"
        fi
        echo ""
        echo "=== Redis 版本 ==="
        "$REDIS_DIR/src/redis-server" --version
        echo ""
        echo "=== YCSB 版本 ==="
        echo "0.17.0"
    } > "$info_file"
    
    echo "系统信息已保存: $info_file"
}

# 运行单个测试
run_single_test() {
    local workload=$1
    local record_count=$2
    local threads=$3
    
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local test_name="${workload}_r${record_count}_t${threads}"
    local result_file="$RESULTS_DIR/${test_name}_${timestamp}"
    
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}测试: $test_name${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo "工作负载: $workload"
    echo "记录数: $record_count"
    echo "线程数: $threads"
    
    # 清空数据
    flush_redis
    
    # Load 阶段
    echo -e "\n${YELLOW}>>> Load 阶段${NC}"
    "$YCSB_DIR/ycsb-0.17.0/bin/ycsb" load redis \
        -P "$WORKLOADS_DIR/$workload" \
        -p recordcount="$record_count" \
        -p threadcount="$threads" \
        -p redis.host="$REDIS_HOST" \
        -p redis.port="$REDIS_PORT" \
        -s \
        2>&1 | tee "${result_file}_load.log"
    
    # 获取加载后的内存使用
    local memory_after_load=$("$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory | grep used_memory_human | cut -d: -f2 | tr -d '\r')
    echo -e "\n${GREEN}加载后内存使用: $memory_after_load${NC}"
    
    # Run 阶段
    echo -e "\n${YELLOW}>>> Run 阶段${NC}"
    "$YCSB_DIR/ycsb-0.17.0/bin/ycsb" run redis \
        -P "$WORKLOADS_DIR/$workload" \
        -p operationcount="$((record_count * 10))" \
        -p threadcount="$threads" \
        -p redis.host="$REDIS_HOST" \
        -p redis.port="$REDIS_PORT" \
        -s \
        2>&1 | tee "${result_file}_run.log"
    
    # 收集 Redis 统计
    echo -e "\n${YELLOW}>>> Redis 统计${NC}"
    {
        echo "=== Redis INFO ==="
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO stats
        echo ""
        echo "=== Redis Memory ==="
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory
        echo ""
        echo "=== Redis Keyspace ==="
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO keyspace
    } > "${result_file}_redis_stats.txt"
    
    echo -e "\n${GREEN}测试完成: $test_name${NC}"
    echo "结果保存: ${result_file}_*.log"
}

# 解析结果并生成报告
parse_results() {
    echo -e "\n${BLUE}=== 解析测试结果 ===${NC}"
    
    local report_file="$RESULTS_DIR/summary_report_$(date +%Y%m%d_%H%M%S).txt"
    
    {
        echo "YCSB NUMA 测试报告"
        echo "生成时间: $(date)"
        echo "========================================"
        echo ""
        
        # 解析所有 run 日志
        for log_file in "$RESULTS_DIR"/*_run.log; do
            if [ -f "$log_file" ]; then
                local basename=$(basename "$log_file" _run.log)
                echo "测试: $basename"
                echo "----------------------------------------"
                
                # 提取关键指标
                grep -E "(OVERALL|Throughput|AverageLatency|95thPercentileLatency|99thPercentileLatency)" "$log_file" | head -20
                echo ""
            fi
        done
        
        echo "========================================"
        echo "详细结果位于: $RESULTS_DIR/"
        
    } > "$report_file"
    
    echo -e "${GREEN}报告已生成: $report_file${NC}"
    cat "$report_file"
}

# 生成 CSV 格式的结果
export_csv() {
    local csv_file="$RESULTS_DIR/results_$(date +%Y%m%d_%H%M%S).csv"
    
    {
        echo "TestName,Workload,RecordCount,Threads,Throughput(ops/sec),AvgReadLatency(us),AvgUpdateLatency(us),P95ReadLatency(us),P99ReadLatency(us)"
        
        for log_file in "$RESULTS_DIR"/*_run.log; do
            if [ -f "$log_file" ]; then
                local basename=$(basename "$log_file" _run.log)
                local throughput=$(grep "Throughput(ops/sec)" "$log_file" | awk '{print $3}')
                local avg_read=$(grep "\[READ\], AverageLatency(us)" "$log_file" | awk '{print $3}')
                local avg_update=$(grep "\[UPDATE\], AverageLatency(us)" "$log_file" | awk '{print $3}')
                local p95_read=$(grep "\[READ\], 95thPercentileLatency(us)" "$log_file" | awk '{print $3}')
                local p99_read=$(grep "\[READ\], 99thPercentileLatency(us)" "$log_file" | awk '{print $3}')
                
                echo "$basename,,$throughput,$avg_read,$avg_update,$p95_read,$p99_read"
            fi
        done
    } > "$csv_file"
    
    echo -e "${GREEN}CSV 结果已导出: $csv_file${NC}"
}

# 显示帮助
show_help() {
    cat << EOF
YCSB NUMA 自动化测试框架

用法: $0 [选项]

选项:
    -h, --help              显示帮助信息
    -w, --workloads         指定工作负载 (逗号分隔, 默认: 全部)
    -r, --records           指定记录数 (逗号分隔, 默认: 100000,500000)
    -t, --threads           指定线程数 (逗号分隔, 默认: 50,100)
    -s, --skip-load         跳过 load 阶段 (假设数据已加载)
    -p, --parse-only        仅解析已有结果
    
示例:
    $0                      运行完整测试套件
    $0 -w workload_numa_hotspot   仅运行热点测试
    $0 -r 100000 -t 50      使用指定参数运行
    $0 -p                   仅生成报告

工作负载:
    workload_numa_hotspot       热点访问模式 (80/20)
    workload_numa_mixed         混合工作负载
    workload_numa_write_heavy   写密集型
EOF
}

# 主函数
main() {
    local skip_load=false
    local parse_only=false
    local custom_workloads=""
    local custom_records=""
    local custom_threads=""
    
    # 解析参数
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                show_help
                exit 0
                ;;
            -w|--workloads)
                custom_workloads="$2"
                shift 2
                ;;
            -r|--records)
                custom_records="$2"
                shift 2
                ;;
            -t|--threads)
                custom_threads="$2"
                shift 2
                ;;
            -s|--skip-load)
                skip_load=true
                shift
                ;;
            -p|--parse-only)
                parse_only=true
                shift
                ;;
            *)
                echo "未知参数: $1"
                show_help
                exit 1
                ;;
        esac
    done
    
    # 仅解析结果
    if [ "$parse_only" = true ]; then
        parse_results
        export_csv
        exit 0
    fi
    
    # 应用自定义配置
    [ -n "$custom_workloads" ] && IFS=',' read -ra TEST_WORKLOADS <<< "$custom_workloads"
    [ -n "$custom_records" ] && IFS=',' read -ra RECORD_COUNTS <<< "$custom_records"
    [ -n "$custom_threads" ] && IFS=',' read -ra THREAD_COUNTS <<< "$custom_threads"
    
    # 打印配置
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  YCSB NUMA 自动化压力测试框架        ${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo "工作负载: ${TEST_WORKLOADS[*]}"
    echo "记录数: ${RECORD_COUNTS[*]}"
    echo "线程数: ${THREAD_COUNTS[*]}"
    echo ""
    
    # 检查 YCSB
    check_ycsb
    
    # 启动 Redis
    start_redis
    
    # 收集系统信息
    collect_system_info
    
    # 运行测试
    echo -e "${BLUE}=== 开始测试 ===${NC}"
    
    for workload in "${TEST_WORKLOADS[@]}"; do
        for record_count in "${RECORD_COUNTS[@]}"; do
            for threads in "${THREAD_COUNTS[@]}"; do
                run_single_test "$workload" "$record_count" "$threads"
            done
        done
    done
    
    # 生成报告
    parse_results
    export_csv
    
    echo -e "\n${GREEN}========================================${NC}"
    echo -e "${GREEN}  所有测试完成!                       ${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo "结果目录: $RESULTS_DIR"
}

main "$@"
