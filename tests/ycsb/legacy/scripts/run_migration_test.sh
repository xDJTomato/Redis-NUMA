#!/bin/bash
#
# YCSB NUMA 迁移策略完整测试脚本
# 测试目标：验证 Composite LRU 迁移策略在混合内存分配场景下的表现
# 覆盖：SLAB (≤128B) + Pool (129B-16KB) + Direct (>16KB)

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 配置
REDIS_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
YCSB_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$YCSB_DIR/results"
WORKLOAD="workloads/workload_numa_migration_full"

REDIS_PORT=6379
REDIS_HOST="127.0.0.1"
REDIS_LOG="$RESULTS_DIR/redis_migration_test.log"

# 测试参数
RECORD_COUNT=100000
OPERATION_COUNT=1000000
THREADS=50

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  NUMA 迁移策略完整测试                ${NC}"
echo -e "${BLUE}========================================${NC}"
echo "工作负载: $WORKLOAD"
echo "记录数: $RECORD_COUNT"
echo "操作数: $OPERATION_COUNT"
echo "线程数: $THREADS"
echo ""

# 清理函数
cleanup() {
    echo -e "\n${YELLOW}清理环境...${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT shutdown 2>/dev/null || true
    echo -e "${GREEN}清理完成${NC}"
}
trap cleanup EXIT

# 启动 Redis (启用 VERBOSE 日志以观察迁移策略)
start_redis() {
    echo -e "${BLUE}=== 启动 Redis (VERBOSE 日志) ===${NC}"
    
    # 停止旧进程
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT shutdown 2>/dev/null || true
    sleep 2
    
    # 启动 Redis 并启用详细日志
    "$REDIS_DIR/src/redis-server" \
        --bind 127.0.0.1 \
        --port $REDIS_PORT \
        --protected-mode no \
        --daemonize yes \
        --loglevel verbose \
        --logfile "$REDIS_LOG" \
        --maxmemory 4gb \
        --maxmemory-policy allkeys-lru \
        --save "" \
        --appendonly no
    
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

# 配置 NUMA 策略
configure_numa_strategy() {
    echo -e "\n${BLUE}=== 配置 NUMA 策略 ===${NC}"
    
    # 启用 Composite LRU 策略
    echo "设置策略为 Composite LRU..."
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMACONFIG SET strategy composite_lru || true
    
    # 显示当前配置
    echo -e "\n当前 NUMA 配置:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMACONFIG GET
}

# 运行 YCSB Load 阶段
run_ycsb_load() {
    echo -e "\n${BLUE}=== YCSB Load 阶段 ===${NC}"
    
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local load_log="$RESULTS_DIR/migration_full_load_${timestamp}.log"
    
    java -cp "ycsb-0.17.0/redis-binding/lib/*:ycsb-0.17.0/lib/*" \
        site.ycsb.Client -load \
        -db site.ycsb.db.RedisClient \
        -P "$WORKLOAD" \
        -p recordcount="$RECORD_COUNT" \
        -p threadcount="$THREADS" \
        -p redis.host="$REDIS_HOST" \
        -p redis.port="$REDIS_PORT" \
        2>&1 | tee "$load_log" | grep -E "(OVERALL|INSERT)" | head -10
    
    echo -e "${GREEN}Load 阶段完成${NC}"
    
    # 显示内存使用
    echo -e "\n${YELLOW}加载后内存使用:${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory | grep -E "used_memory_human|mem_fragmentation"
}

# 运行 YCSB Run 阶段
run_ycsb_run() {
    echo -e "\n${BLUE}=== YCSB Run 阶段 (触发迁移) ===${NC}"
    
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local run_log="$RESULTS_DIR/migration_full_run_${timestamp}.log"
    
    echo "开始执行 $OPERATION_COUNT 次操作..."
    echo "监控 Redis 日志: tail -f $REDIS_LOG"
    echo ""
    
    java -cp "ycsb-0.17.0/redis-binding/lib/*:ycsb-0.17.0/lib/*" \
        site.ycsb.Client -t \
        -db site.ycsb.db.RedisClient \
        -P "$WORKLOAD" \
        -p operationcount="$OPERATION_COUNT" \
        -p threadcount="$THREADS" \
        -p redis.host="$REDIS_HOST" \
        -p redis.port="$REDIS_PORT" \
        2>&1 | tee "$run_log" | grep -E "(OVERALL|READ|UPDATE|INSERT)" | head -25
    
    echo -e "${GREEN}Run 阶段完成${NC}"
}

# 收集测试结果
collect_results() {
    echo -e "\n${BLUE}=== 收集测试结果 ===${NC}"
    
    # Redis 统计
    echo -e "\n${YELLOW}Redis 统计:${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO stats | grep -E "total_commands|keyspace"
    
    echo -e "\n${YELLOW}内存统计:${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory | grep -E "used_memory_human|mem_fragmentation|maxmemory"
    
    echo -e "\n${YELLOW}Keyspace 统计:${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO keyspace
    
    # NUMA 迁移统计
    echo -e "\n${YELLOW}NUMA 迁移统计:${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMAMIGRATE STATS 2>/dev/null || echo "NUMAMIGRATE 命令不可用"
}

# 分析日志中的迁移事件
analyze_migration_logs() {
    echo -e "\n${BLUE}=== 分析迁移日志 ===${NC}"
    
    if [ ! -f "$REDIS_LOG" ]; then
        echo -e "${RED}日志文件不存在: $REDIS_LOG${NC}"
        return 1
    fi
    
    echo -e "\n${YELLOW}策略初始化:${NC}"
    grep "Composite LRU.*initialized" "$REDIS_LOG" | tail -5
    
    echo -e "\n${YELLOW}远程访问检测 (触发迁移评估):${NC}"
    grep "Remote access detected" "$REDIS_LOG" | wc -l | xargs echo "  总计:"
    grep "Remote access detected" "$REDIS_LOG" | head -10
    
    echo -e "\n${YELLOW}迁移触发事件:${NC}"
    grep "MIGRATION TRIGGERED" "$REDIS_LOG" | wc -l | xargs echo "  总计:"
    grep "MIGRATION TRIGGERED" "$REDIS_LOG" | head -10
    
    echo -e "\n${YELLOW}热度衰减周期:${NC}"
    grep "Executing heat decay cycle" "$REDIS_LOG" | wc -l | xargs echo "  总计:"
    
    echo -e "\n${YELLOW}待迁移队列处理:${NC}"
    grep "Processing.*pending migrations" "$REDIS_LOG" | wc -l | xargs echo "  总计:"
    grep "Processing.*pending migrations" "$REDIS_LOG" | tail -5
}

# 生成测试报告
generate_report() {
    local timestamp=$(date +%Y%m%d_%H%M%S)
    local report_file="$RESULTS_DIR/migration_test_report_${timestamp}.txt"
    
    echo -e "\n${BLUE}=== 生成测试报告 ===${NC}"
    
    {
        echo "========================================"
        echo "NUMA 迁移策略完整测试报告"
        echo "========================================"
        echo "测试时间: $(date)"
        echo "工作负载: workload_numa_migration_full"
        echo ""
        
        echo "测试配置:"
        echo "  记录数: $RECORD_COUNT"
        echo "  操作数: $OPERATION_COUNT"
        echo "  线程数: $THREADS"
        echo ""
        
        echo "内存分配覆盖:"
        echo "  SLAB分配: ≤128B (小对象)"
        echo "  Pool分配: 129B-16KB (中等对象)"
        echo "  Direct分配: >16KB (大对象)"
        echo ""
        
        echo "策略触发统计:"
        echo "  远程访问检测: $(grep -c "Remote access detected" "$REDIS_LOG" 2>/dev/null || echo 0)"
        echo "  迁移触发: $(grep -c "MIGRATION TRIGGERED" "$REDIS_LOG" 2>/dev/null || echo 0)"
        echo "  热度衰减: $(grep -c "Executing heat decay cycle" "$REDIS_LOG" 2>/dev/null || echo 0)"
        echo ""
        
        echo "详细日志: $REDIS_LOG"
        echo "========================================"
        
    } > "$report_file"
    
    cat "$report_file"
    echo -e "\n${GREEN}报告已保存: $report_file${NC}"
}

# 主函数
main() {
    # 检查 YCSB
    if [ ! -d "$YCSB_DIR/ycsb-0.17.0" ]; then
        echo -e "${RED}YCSB 未安装，请先运行 ./scripts/install_ycsb.sh${NC}"
        exit 1
    fi
    
    # 检查工作负载文件
    if [ ! -f "$YCSB_DIR/$WORKLOAD" ]; then
        echo -e "${RED}工作负载文件不存在: $WORKLOAD${NC}"
        exit 1
    fi
    
    # 执行测试流程
    start_redis
    configure_numa_strategy
    run_ycsb_load
    
    echo -e "\n${YELLOW}等待 5 秒让系统稳定...${NC}"
    sleep 5
    
    run_ycsb_run
    collect_results
    analyze_migration_logs
    generate_report
    
    echo -e "\n${GREEN}========================================${NC}"
    echo -e "${GREEN}  测试完成！                          ${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo -e "结果目录: $RESULTS_DIR"
    echo -e "Redis 日志: $REDIS_LOG"
}

main "$@"
