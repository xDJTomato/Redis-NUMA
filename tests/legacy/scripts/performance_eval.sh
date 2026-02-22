#!/bin/bash

# Composite LRU策略性能评估脚本
# 作者: Redis NUMA项目组
# 版本: 1.0

set -euo pipefail

REDIS_PORT=${1:-6379}
REDIS_CLI="./src/redis-cli -p $REDIS_PORT"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

log_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1"
}

# 检查Redis连接
check_redis() {
    if ! $REDIS_CLI PING >/dev/null 2>&1; then
        log_error "无法连接到Redis服务器 (端口: $REDIS_PORT)"
        exit 1
    fi
    log_success "Redis连接正常"
}

# 1. 内存使用评估
evaluate_memory_usage() {
    log_info "=== 内存使用评估 ==="
    
    local mem_info=$($REDIS_CLI INFO memory)
    
    echo "$mem_info" | grep -E "used_memory_human|used_memory_rss_human|used_memory_peak_human"
    
    # 使用RSS值计算（避免used_memory异常）
    local rss_line=$(echo "$mem_info" | grep used_memory_rss: | tr -d '\r')
    if [ -n "$rss_line" ]; then
        local rss=$(echo "$rss_line" | cut -d: -f2)
        local rss_mb=$((rss / 1024 / 1024))
        log_info "RSS Memory: ${rss_mb} MB"
    fi
}

# 2. 迁移性能评估
evaluate_migration_performance() {
    log_info "=== 迁移性能评估 ==="
    
    # 获取初始统计
    local initial_stats=$($REDIS_CLI NUMAMIGRATE STATS | tr -d '\r')
    echo "初始迁移统计:"
    echo "$initial_stats"
    
    # 执行一批迁移操作
    log_info "执行测试迁移..."
    for i in {1..100}; do
        $REDIS_CLI SET "perf_test_$i" "value_$i" >/dev/null 2>&1
        $REDIS_CLI NUMAMIGRATE KEY "perf_test_$i" 0 >/dev/null 2>&1 || true
    done
    
    # 获取最终统计
    sleep 1
    local final_stats=$($REDIS_CLI NUMAMIGRATE STATS | tr -d '\r')
    echo "迁移后统计:"
    echo "$final_stats"
    
    # 计算迁移速率
    local initial_total=$(echo "$initial_stats" | grep -A 1 "total_migrations" | tail -1 | tr -d ' \t\r')
    local final_total=$(echo "$final_stats" | grep -A 1 "total_migrations" | tail -1 | tr -d ' \t\r')
    
    # 处理可能的空值
    initial_total=${initial_total:-0}
    final_total=${final_total:-0}
    
    local migrated=$((final_total - initial_total))
    
    log_success "本次测试迁移了 $migrated 个key (从 $initial_total 到 $final_total)"
}

# 3. 响应时间测试
evaluate_response_time() {
    log_info "=== 响应时间评估 ==="
    
    # 清理测试数据
    $REDIS_CLI FLUSHDB >/dev/null
    
    # 准备测试数据
    log_info "准备测试数据..."
    ./src/redis-benchmark -p $REDIS_PORT -n 10000 -r 10000 -d 1024 -t set --csv > /dev/null 2>&1
    
    # GET性能测试
    log_info "GET性能测试 (1KB数据)..."
    ./src/redis-benchmark -p $REDIS_PORT -n 50000 -c 10 -t get -q
    
    # SET性能测试
    log_info "SET性能测试 (1KB数据)..."
    ./src/redis-benchmark -p $REDIS_PORT -n 50000 -c 10 -t set -q
    
    # Pipeline性能测试
    log_info "Pipeline性能测试..."
    ./src/redis-benchmark -p $REDIS_PORT -n 50000 -c 10 -P 10 -t get -q
}

# 4. 热点识别准确性测试
evaluate_hotspot_detection() {
    log_info "=== 热点识别准确性评估 ==="
    
    $REDIS_CLI FLUSHDB >/dev/null
    
    # 创建测试数据
    for i in {1..1000}; do
        $REDIS_CLI SET "hotspot_test_$i" "data_$i" >/dev/null
    done
    
    # 模拟热点访问模式
    log_info "模拟热点访问..."
    for i in {1..100}; do  # 热点key
        for j in {1..50}; do  # 高频访问
            $REDIS_CLI GET "hotspot_test_$i" >/dev/null
        done
    done
    
    for i in {901..1000}; do  # 冷key
        $REDIS_CLI GET "hotspot_test_$i" >/dev/null
    done
    
    log_info "热点识别测试完成"
    log_warn "注意: 需要查看内部热度统计来评估准确性"
}

# 5. 并发性能测试
evaluate_concurrency() {
    log_info "=== 并发性能评估 ==="
    
    $REDIS_CLI FLUSHDB >/dev/null
    
    # 不同并发级别的测试
    for clients in 1 10 50 100; do
        log_info "测试并发数: $clients"
        ./src/redis-benchmark -p $REDIS_PORT -n 10000 -c $clients -t get -q
    done
}

# 6. 内存分布分析
analyze_memory_distribution() {
    log_info "=== 内存分布分析 ==="
    
    # 获取key的数量统计
    local db_size=$($REDIS_CLI DBSIZE)
    log_info "数据库总key数: $db_size"
    
    # 按key模式统计（如果有特定命名规则）
    local test_keys=$($REDIS_CLI KEYS "test_*" | wc -l)
    local heat_keys=$($REDIS_CLI KEYS "heat_*" | wc -l)
    local perf_keys=$($REDIS_CLI KEYS "perf_*" | wc -l)
    
    log_info "测试key数量: $test_keys"
    log_info "热度测试key数量: $heat_keys"  
    log_info "性能测试key数量: $perf_keys"
}

# 主函数
main() {
    echo "========================================"
    echo "Composite LRU策略性能评估工具"
    echo "========================================"
    echo "Redis端口: $REDIS_PORT"
    echo ""
    
    check_redis
    
    evaluate_memory_usage
    echo ""
    
    evaluate_migration_performance
    echo ""
    
    evaluate_response_time
    echo ""
    
    evaluate_hotspot_detection
    echo ""
    
    evaluate_concurrency
    echo ""
    
    analyze_memory_distribution
    echo ""
    
    log_success "性能评估完成！"
    echo "建议查看Redis日志获取更详细的策略执行信息"
}

# 运行主函数
main "$@"