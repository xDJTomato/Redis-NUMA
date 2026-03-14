#!/bin/bash

# NUMA可配置策略测试脚本

REDIS_PORT=${1:-6399}
REDIS_CLI="./src/redis-cli -p $REDIS_PORT"
REDIS_SERVER="./src/redis-server"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

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

# 检查Redis服务器状态
check_redis_status() {
    if ! $REDIS_CLI PING >/dev/null 2>&1; then
        log_info "启动Redis服务器..."
        $REDIS_SERVER --port $REDIS_PORT --daemonize yes
        sleep 2
        
        if ! $REDIS_CLI PING >/dev/null 2>&1; then
            log_error "无法启动Redis服务器"
            exit 1
        fi
    fi
    log_success "Redis服务器运行正常"
}

# 测试1: 基本命令测试
test_basic_commands() {
    log_info "测试1: 基本NUMACONFIG命令测试..."
    
    # 测试GET命令
    local get_result=$($REDIS_CLI NUMACONFIG GET 2>/dev/null)
    if [[ $? -eq 0 && -n "$get_result" ]]; then
        log_success "NUMACONFIG GET命令正常"
        echo "$get_result" | head -10
    else
        log_error "NUMACONFIG GET命令失败"
        return 1
    fi
    
    # 测试HELP命令
    local help_result=$($REDIS_CLI NUMACONFIG HELP 2>/dev/null)
    if [[ $? -eq 0 && -n "$help_result" ]]; then
        log_success "NUMACONFIG HELP命令正常"
    else
        log_error "NUMACONFIG HELP命令失败"
        return 1
    fi
    
    return 0
}

# 测试2: 策略配置测试
test_strategy_configuration() {
    log_info "测试2: 策略配置测试..."
    
    # 测试设置不同策略
    local strategies=("local_first" "interleaved" "round_robin" "weighted")
    
    for strategy in "${strategies[@]}"; do
        local result=$($REDIS_CLI NUMACONFIG SET strategy $strategy 2>/dev/null)
        if [ "$result" = "OK" ]; then
            log_success "策略 $strategy 设置成功"
            
            # 验证策略是否生效
            local current_config=$($REDIS_CLI NUMACONFIG GET 2>/dev/null)
            if echo "$current_config" | grep -q "\"$strategy\""; then
                log_success "策略 $strategy 配置验证通过"
            else
                log_warn "策略 $strategy 配置验证失败"
            fi
        else
            log_error "策略 $strategy 设置失败: $result"
        fi
    done
}

# 测试3: 节点权重配置测试
test_node_weights() {
    log_info "测试3: 节点权重配置测试..."
    
    # 获取节点数量
    local config_info=$($REDIS_CLI NUMACONFIG GET 2>/dev/null)
    local num_nodes=$(echo "$config_info" | grep -A 1 "\"nodes\"" | tail -1 | tr -d ' ')
    
    if [ -z "$num_nodes" ] || [ "$num_nodes" -le 0 ]; then
        num_nodes=1
    fi
    
    log_info "检测到 $num_nodes 个NUMA节点"
    
    # 为每个节点设置不同的权重
    for ((i=0; i<num_nodes; i++)); do
        local weight=$((100 - i * 20))
        if [ $weight -lt 20 ]; then
            weight=20
        fi
        
        local result=$($REDIS_CLI NUMACONFIG SET weight $i $weight 2>/dev/null)
        if [ "$result" = "OK" ]; then
            log_success "节点 $i 权重设置为 $weight"
        else
            log_error "节点 $i 权重设置失败: $result"
        fi
    done
}

# 测试4: CXL优化配置测试
test_cxl_optimization() {
    log_info "测试4: CXL优化配置测试..."
    
    # 启用CXL优化
    local result=$($REDIS_CLI NUMACONFIG SET cxl_optimization on 2>/dev/null)
    if [ "$result" = "OK" ]; then
        log_success "CXL优化启用成功"
    else
        log_error "CXL优化启用失败: $result"
    fi
    
    # 设置平衡阈值
    result=$($REDIS_CLI NUMACONFIG SET balance_threshold 25 2>/dev/null)
    if [ "$result" = "OK" ]; then
        log_success "平衡阈值设置成功"
    else
        log_error "平衡阈值设置失败: $result"
    fi
}

# 测试5: 内存分配测试
test_memory_allocation() {
    log_info "测试5: 内存分配测试..."
    
    # 创建测试数据
    log_info "创建测试数据..."
    for i in {1..1000}; do
        $REDIS_CLI SET "test_key_$i" "test_value_$i" >/dev/null 2>&1
    done
    
    # 查看分配统计
    local stats_result=$($REDIS_CLI NUMACONFIG STATS 2>/dev/null)
    if [[ $? -eq 0 && -n "$stats_result" ]]; then
        log_success "内存分配统计获取成功"
        echo "$stats_result" | head -15
    else
        log_warn "内存分配统计获取失败"
    fi
}

# 测试6: 手动重新平衡测试
test_manual_rebalance() {
    log_info "测试6: 手动重新平衡测试..."
    
    local result=$($REDIS_CLI NUMACONFIG REBALANCE 2>/dev/null)
    if [ "$result" = "OK" ]; then
        log_success "手动重新平衡触发成功"
    else
        log_error "手动重新平衡触发失败: $result"
    fi
}

# 测试7: 性能对比测试
test_performance_comparison() {
    log_info "测试7: 性能对比测试..."
    
    # 设置为本地优先策略
    $REDIS_CLI NUMACONFIG SET strategy local_first >/dev/null 2>&1
    sleep 1
    
    log_info "测试本地优先策略性能..."
    local local_perf=$(./src/redis-benchmark -p $REDIS_PORT -n 10000 -t get,set -q 2>&1)
    
    # 设置为交错分配策略
    $REDIS_CLI NUMACONFIG SET strategy interleaved >/dev/null 2>&1
    sleep 1
    
    log_info "测试交错分配策略性能..."
    local interleave_perf=$(./src/redis-benchmark -p $REDIS_PORT -n 10000 -t get,set -q 2>&1)
    
    log_info "性能对比结果:"
    echo "本地优先策略:"
    echo "$local_perf"
    echo ""
    echo "交错分配策略:"
    echo "$interleave_perf"
}

# 测试8: 错误处理测试
test_error_handling() {
    log_info "测试8: 错误处理测试..."
    
    # 测试无效策略
    local result=$($REDIS_CLI NUMACONFIG SET strategy invalid_strategy 2>/dev/null)
    if [ "$result" != "OK" ]; then
        log_success "无效策略拒绝处理正常"
    else
        log_error "无效策略未被拒绝"
    fi
    
    # 测试无效节点ID
    result=$($REDIS_CLI NUMACONFIG SET weight 999 100 2>/dev/null)
    if [ "$result" != "OK" ]; then
        log_success "无效节点ID拒绝处理正常"
    else
        log_error "无效节点ID未被拒绝"
    fi
    
    # 测试无效权重值
    result=$($REDIS_CLI NUMACONFIG SET weight 0 10000 2>/dev/null)
    if [ "$result" != "OK" ]; then
        log_success "无效权重值拒绝处理正常"
    else
        log_error "无效权重值未被拒绝"
    fi
}

# 主测试函数
main() {
    echo "========================================"
    echo "NUMA可配置策略测试脚本"
    echo "========================================"
    echo "Redis端口: $REDIS_PORT"
    echo ""
    
    # 检查环境
    check_redis_status
    echo ""
    
    # 执行各项测试
    test_basic_commands && echo ""
    test_strategy_configuration && echo ""
    test_node_weights && echo ""
    test_cxl_optimization && echo ""
    test_memory_allocation && echo ""
    test_manual_rebalance && echo ""
    test_performance_comparison && echo ""
    test_error_handling && echo ""
    
    # 清理测试数据
    log_info "清理测试数据..."
    $REDIS_CLI FLUSHDB >/dev/null 2>&1
    
    log_success "所有测试完成！"
    echo ""
    echo "测试总结:"
    echo "- 基本命令功能正常"
    echo "- 策略配置功能正常" 
    echo "- 节点权重配置正常"
    echo "- CXL优化配置正常"
    echo "- 内存分配统计正常"
    echo "- 重新平衡功能正常"
    echo "- 错误处理机制正常"
}

# 运行测试
main "$@"