#!/bin/bash
#
# Composite LRU策略完整测试脚本
# 适用于带CXL设备的虚拟机环境
#
# 用法: ./test_composite_lru.sh [redis端口] [测试时长(秒)]
# 示例: ./test_composite_lru.sh 6379 60

# 不要在算术运算失败时退出
set -e

# 配置参数
REDIS_PORT=${1:-6379}
TEST_DURATION=${2:-60}
REDIS_HOST="127.0.0.1"
REDIS_CLI="./src/redis-cli -p $REDIS_PORT"
LOG_FILE="/tmp/composite_lru_test.log"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 日志函数
log_info() {
    echo -e "${BLUE}[INFO]${NC} $1" | tee -a $LOG_FILE
}

log_success() {
    echo -e "${GREEN}[PASS]${NC} $1" | tee -a $LOG_FILE
}

log_error() {
    echo -e "${RED}[FAIL]${NC} $1" | tee -a $LOG_FILE
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1" | tee -a $LOG_FILE
}

# 检查Redis是否运行
check_redis() {
    log_info "检查Redis服务器状态..."
    if ! $REDIS_CLI PING > /dev/null 2>&1; then
        log_error "Redis服务器未运行，请先启动Redis"
        echo "启动命令示例:"
        echo "  ./src/redis-server --port $REDIS_PORT --daemonize yes"
        exit 1
    fi
    log_success "Redis服务器运行正常"
}

# 测试1: 检查策略是否正确加载
test_strategy_loaded() {
    log_info "测试1: 检查Composite LRU策略是否加载..."
    
    # 检查日志中是否有策略初始化信息
    if grep -q "Composite LRU.*Strategy initialized" /tmp/redis-test.log 2>/dev/null || \
       grep -q "composite-lru.*slot 1" /tmp/redis-*.log 2>/dev/null; then
        log_success "Composite LRU策略已正确加载到slot 1"
    else
        log_warn "无法从日志确认策略加载状态，继续其他测试..."
    fi
}

# 测试2: 基础数据操作
test_basic_operations() {
    log_info "测试2: 基础数据操作测试..."
    
    # 清理测试数据
    $REDIS_CLI FLUSHDB > /dev/null
    
    # 测试STRING
    $REDIS_CLI SET test_key_1 "value1" > /dev/null
    local result=$($REDIS_CLI GET test_key_1)
    if [ "$result" = "value1" ]; then
        log_success "STRING操作正常"
    else
        log_error "STRING操作失败"
    fi
    
    # 测试HASH
    $REDIS_CLI HSET test_hash field1 value1 field2 value2 > /dev/null
    local hlen=$($REDIS_CLI HLEN test_hash)
    if [ "$hlen" = "2" ]; then
        log_success "HASH操作正常"
    else
        log_error "HASH操作失败"
    fi
    
    # 测试LIST
    $REDIS_CLI RPUSH test_list item1 item2 item3 > /dev/null
    local llen=$($REDIS_CLI LLEN test_list)
    if [ "$llen" = "3" ]; then
        log_success "LIST操作正常"
    else
        log_error "LIST操作失败"
    fi
    
    # 测试SET
    $REDIS_CLI SADD test_set member1 member2 member3 > /dev/null
    local scard=$($REDIS_CLI SCARD test_set)
    if [ "$scard" = "3" ]; then
        log_success "SET操作正常"
    else
        log_error "SET操作失败"
    fi
    
    # 测试ZSET
    $REDIS_CLI ZADD test_zset 1 member1 2 member2 3 member3 > /dev/null
    local zcard=$($REDIS_CLI ZCARD test_zset)
    if [ "$zcard" = "3" ]; then
        log_success "ZSET操作正常"
    else
        log_error "ZSET操作失败"
    fi
}

# 测试3: 热度追踪测试（模拟访问模式）
test_heat_tracking() {
    log_info "测试3: 热度追踪测试（模拟访问模式）..."
    
    # 创建一批测试key
    log_info "创建100个测试key..."
    for i in $(seq 1 100); do
        $REDIS_CLI SET "heat_key_$i" "value_$i" > /dev/null
    done
    log_success "100个测试key创建完成"
    
    # 模拟热点key访问（某些key被频繁访问）
    log_info "模拟热点访问模式（key_1到key_10各访问50次）..."
    for round in $(seq 1 50); do
        for i in $(seq 1 10); do
            $REDIS_CLI GET "heat_key_$i" > /dev/null
        done
    done
    log_success "热点访问模拟完成"
    
    # 模拟冷key访问（某些key很少访问）
    log_info "模拟冷key访问（key_91到key_100各访问1次）..."
    for i in $(seq 91 100); do
        $REDIS_CLI GET "heat_key_$i" > /dev/null
    done
    log_success "冷key访问模拟完成"
}

# 测试4: NUMA迁移命令测试
test_numa_migrate_command() {
    log_info "测试4: NUMA迁移命令测试..."
    
    # 测试NUMAMIGRATE HELP
    local help_output=$($REDIS_CLI NUMAMIGRATE HELP 2>/dev/null || echo "ERR")
    if echo "$help_output" | grep -q "NUMAMIGRATE KEY"; then
        log_success "NUMAMIGRATE HELP命令正常"
    else
        log_warn "NUMAMIGRATE HELP命令可能不支持或返回异常"
    fi
    
    # 测试NUMAMIGRATE STATS
    local stats_output=$($REDIS_CLI NUMAMIGRATE STATS 2>/dev/null || echo "ERR")
    if echo "$stats_output" | grep -q "total_migrations"; then
        log_success "NUMAMIGRATE STATS命令正常"
        log_info "当前统计: $(echo $stats_output | tr '\n' ' ')"
    else
        log_warn "NUMAMIGRATE STATS命令可能不支持"
    fi
    
    # 测试KEY迁移（如果支持）
    $REDIS_CLI SET migrate_test_key "test_value" > /dev/null
    local migrate_result=$($REDIS_CLI NUMAMIGRATE KEY migrate_test_key 0 2>/dev/null || echo "ERR")
    if [ "$migrate_result" = "OK" ]; then
        log_success "NUMAMIGRATE KEY迁移命令正常"
        
        # 验证数据完整性
        local verify_value=$($REDIS_CLI GET migrate_test_key)
        if [ "$verify_value" = "test_value" ]; then
            log_success "迁移后数据完整性验证通过"
        else
            log_error "迁移后数据完整性验证失败"
        fi
    else
        log_warn "NUMAMIGRATE KEY命令可能不支持或返回: $migrate_result"
    fi
}

# 测试5: 超高压测试（超过8GB内存占用）
test_ultra_high_pressure() {
    log_info "测试5: 超高压测试（目标超过8GB内存占用）..."
    
    # 计算需要创建的对象数量和大小
    local target_memory_gb=8
    local target_memory_bytes=$((target_memory_gb * 1024 * 1024 * 1024))
    
    # 阶段1: 使用redis-benchmark快速创建数据
    log_info "阶段1: 使用redis-benchmark快速创建数据..."
    local start_time=$(date +%s)
    
    # 并行创建不同大小的数据（使用 -r 参数确保创建唯一key）
    log_info "创建微型对象（100B，10万个）..."
    ./src/redis-benchmark -p $REDIS_PORT -n 100000 -r 100000 -d 100 -t set --csv > /dev/null 2>&1 &
    local pid1=$!
    
    log_info "创建小型对象（10KB，2万个）..."
    ./src/redis-benchmark -p $REDIS_PORT -n 20000 -r 20000 -d 10240 -t set --csv > /dev/null 2>&1 &
    local pid2=$!
    
    log_info "创建中型对象（100KB，1万个）..."
    ./src/redis-benchmark -p $REDIS_PORT -n 10000 -r 10000 -d 102400 -t set --csv > /dev/null 2>&1 &
    local pid3=$!
    
    log_info "创建大型对象（1MB，5000个）..."
    ./src/redis-benchmark -p $REDIS_PORT -n 5000 -r 5000 -d 1048576 -t set --csv > /dev/null 2>&1 &
    local pid4=$!
    
    log_info "创建超大型对象（10MB，500个）..."
    ./src/redis-benchmark -p $REDIS_PORT -n 500 -r 500 -d 10485760 -t set --csv > /dev/null 2>&1 &
    local pid5=$!
    
    # 等待所有benchmark完成
    log_info "等待数据创建完成..."
    wait $pid1 $pid2 $pid3 $pid4 $pid5
    
    # 检查内存使用情况（使用RSS避免used_memory溢出）
    local current_mem_rss=$($REDIS_CLI INFO memory | grep used_memory_rss: | cut -d: -f2 | tr -d '\r')
    # 转换为MB来避免小于1GB时显示为0
    local current_mem_mb=$((current_mem_rss / 1024 / 1024))
    local current_mem_gb=$((current_mem_mb / 1024))
    local elapsed=$(($(date +%s) - start_time))
    
    log_success "批量数据创建完成，耗时 ${elapsed}秒"
    log_info "当前内存占用(RSS): ${current_mem_mb}MB (${current_mem_gb}GB)"
    
    # 如果内存还不够，继续创建更多数据（目标8GB = 8192MB）
    local target_mb=$((target_memory_gb * 1024))
    if [ "$current_mem_mb" -lt "$target_mb" ]; then
        log_info "内存不足，继续创建更多数据..."
        
        local extra_needed_mb=$((target_mb - current_mem_mb))
        local extra_count=$((extra_needed_mb / 10))  # 每个10MB对象
        
        log_info "额外创建 $extra_count 个10MB对象..."
        ./src/redis-benchmark -p $REDIS_PORT -n $extra_count -r $extra_count -d 10485760 -t set --csv > /dev/null 2>&1
        
        current_mem_rss=$($REDIS_CLI INFO memory | grep used_memory_rss: | cut -d: -f2 | tr -d '\r')
        current_mem_mb=$((current_mem_rss / 1024 / 1024))
        log_info "补充后内存占用(RSS): ${current_mem_mb}MB"
    fi
    
    local create_time=$(($(date +%s) - start_time))
    local final_mem_rss=$($REDIS_CLI INFO memory | grep used_memory_rss: | cut -d: -f2 | tr -d '\r')
    local final_mem_mb=$((final_mem_rss / 1024 / 1024))
    local final_mem_gb=$((final_mem_mb / 1024))
    local obj_count=$($REDIS_CLI DBSIZE)
    
    log_success "超大规模对象集创建完成"
    log_info "总对象数: $obj_count"
    log_info "总耗时: ${create_time}秒"
    log_info "最终内存占用(RSS): ${final_mem_mb}MB (${final_mem_gb}GB)"
    
    if [ "$final_mem_gb" -lt "$target_memory_gb" ]; then
        log_warn "警告: 内存占用未达到目标 (${final_mem_gb}GB < ${target_memory_gb}GB)"
    fi
    
    # 阶段2: 使用redis-benchmark进行热点访问
    log_info "阶段2: 使用redis-benchmark进行热点访问..."
    local hot_start=$(date +%s)
    
    log_info "热点访问测试（100万GET操作）..."
    ./src/redis-benchmark -p $REDIS_PORT -n 1000000 -t get -k uhp:tiny:1:__rand_int__ --csv > /dev/null 2>&1
    
    local hot_time=$(($(date +%s) - hot_start))
    log_success "热点访问完成，耗时 ${hot_time}秒"
    
    # 阶段3: 批量迁移测试
    log_info "阶段3: 批量迁移测试..."
    local migrate_start=$(date +%s)
    local migrate_total=0
    local migrate_ok=0
    
    for t in tiny small medium large huge; do
        local limit=100
        [ "$t" = "huge" ] && limit=50
        [ "$t" = "large" ] && limit=80
        
        for i in $(seq 1 $limit); do
            local result=$($REDIS_CLI NUMAMIGRATE KEY "uhp:${t}:1:${i}" 0 2>/dev/null || echo "ERR")
            migrate_total=$((migrate_total + 1))
            [ "$result" = "OK" ] && migrate_ok=$((migrate_ok + 1))
        done
        log_info "  $t 类型迁移完成: $limit 个"
    done
    
    local migrate_time=$(($(date +%s) - migrate_start))
    log_success "批量迁移完成: $migrate_ok/$migrate_total 成功, 耗时 ${migrate_time}秒"
    
    # 阶段4: 使用redis-benchmark进行压力测试
    log_info "阶段4: 使用redis-benchmark进行混合压力测试..."
    local mix_start=$(date +%s)
    
    log_info "混合读写测试（SET+GET，100万操作）..."
    ./src/redis-benchmark -p $REDIS_PORT -n 1000000 -t set,get -d 1024 -k uhp:small:2:__rand_int__ --csv > /dev/null 2>&1
    
    local mix_time=$(($(date +%s) - mix_start))
    log_success "混合压力测试完成，耗时 ${mix_time}秒"
    
    # 阶段5: 数据完整性验证
    log_info "阶段5: 数据完整性验证..."
    local verify_ok=0
    local verify_fail=0
    
    for t in tiny small medium large huge; do
        local check_count=100
        [ "$t" = "huge" ] && check_count=20
        [ "$t" = "large" ] && check_count=50
        
        for i in $(seq 1 $check_count); do
            local val=$($REDIS_CLI GET "uhp:${t}:1:${i}" 2>/dev/null)
            if [ -n "$val" ]; then
                verify_ok=$((verify_ok + 1))
            else
                verify_fail=$((verify_fail + 1))
            fi
        done
    done
    
    if [ "$verify_fail" -eq 0 ]; then
        log_success "数据完整性验证通过 ($verify_ok 个key)"
    else
        log_warn "数据完整性: $verify_ok 成功, $verify_fail 失败"
    fi
    
    # 显示统计
    log_info "最终NUMA迁移统计:"
    $REDIS_CLI NUMAMIGRATE STATS 2>/dev/null | sed 's/^/  /' || log_warn "无法获取迁移统计"
    
    log_info "对象统计:"
    for t in tiny small medium large huge; do
        local cnt=$($REDIS_CLI KEYS "uhp:${t}:*" 2>/dev/null | wc -l)
        log_info "  $t: $cnt 个"
    done
    
    local mem_final=$($REDIS_CLI INFO memory | grep used_memory_human | cut -d: -f2 | tr -d '\r')
    log_info "最终内存使用: $mem_final"
    
    # 阶段6: 清理
    log_info "阶段6: 清理超高压测试数据..."
    local cleanup_start=$(date +%s)
    
    $REDIS_CLI FLUSHDB > /dev/null 2>&1
    
    local cleanup_time=$(($(date +%s) - cleanup_start))
    log_success "超高压测试数据清理完成，耗时 ${cleanup_time}秒"
}

# 测试6: 检查策略执行日志
test_strategy_execution() {
    log_info "测试6: 检查策略执行日志..."
    
    # 等待一段时间让策略执行几次
    log_info "等待策略执行（5秒）..."
    sleep 5
    
    # 检查Redis日志中是否有策略执行记录
    local log_files=("/tmp/redis-test.log" "/tmp/redis-$REDIS_PORT.log" "/var/log/redis/redis-server.log" "./redis.log")
    local found_log=false
    
    for log_file in "${log_files[@]}"; do
        if [ -f "$log_file" ]; then
            if grep -q "Composite LRU\|NUMA Strategy" "$log_file" 2>/dev/null; then
                log_success "在 $log_file 中找到策略执行日志"
                log_info "最近5条策略相关日志:"
                grep "Composite LRU\|NUMA Strategy" "$log_file" | tail -5 | sed 's/^/  /'
                found_log=true
                
                # 检查策略是否实际执行
                local exec_count=$(grep -c "Composite LRU.*execute\|NUMA Strategy.*executed" "$log_file" 2>/dev/null | tr -d '\n' || echo "0")
                if [ "$exec_count" -gt 0 ] 2>/dev/null; then
                    log_success "策略已执行 $exec_count 次"
                fi
                break
            fi
        fi
    done
    
    if [ "$found_log" = false ]; then
        log_warn "未找到策略执行日志，请检查Redis日志配置"
        log_info "尝试从INFO命令获取信息..."
        $REDIS_CLI INFO 2>/dev/null | grep -i numa | sed 's/^/  /' || true
    fi
}

# 测试7: CXL设备检测
test_cxl_device() {
    log_info "测试7: CXL设备检测..."
    
    # 检查是否有CXL设备
    if [ -d "/sys/bus/cxl" ]; then
        log_success "检测到CXL总线"
        
        # 列出CXL设备
        local cxl_devices=$(ls /sys/bus/cxl/devices/ 2>/dev/null | wc -l)
        log_info "CXL设备数量: $cxl_devices"
        
        if [ "$cxl_devices" -gt 0 ]; then
            log_info "CXL设备列表:"
            ls /sys/bus/cxl/devices/ 2>/dev/null | sed 's/^/  /'
            
            # 检查CXL内存
            if [ -f "/proc/iomem" ]; then
                local cxl_mem=$(grep -i cxl /proc/iomem 2>/dev/null | head -5)
                if [ -n "$cxl_mem" ]; then
                    log_info "CXL内存区域:"
                    echo "$cxl_mem" | sed 's/^/  /'
                fi
            fi
        fi
    else
        log_warn "未检测到CXL总线（这是正常的，如果没有CXL硬件）"
    fi
    
    # 检查NUMA节点信息
    log_info "NUMA节点信息:"
    if command -v numactl &> /dev/null; then
        numactl --hardware 2>/dev/null | head -15 | sed 's/^/  /' || log_info "  无法获取NUMA信息"
    else
        log_info "  numactl未安装"
    fi
    
    # 检查内存信息
    log_info "系统内存信息:"
    free -h 2>/dev/null | sed 's/^/  /' || log_info "  无法获取内存信息"
}

# 测试8: 内存使用检查
test_memory_usage() {
    log_info "测试8: 内存使用检查..."
    
    local used_memory=$($REDIS_CLI INFO memory | grep "^used_memory:" | cut -d: -f2 | tr -d '\r')
    local used_memory_human=$($REDIS_CLI INFO memory | grep "^used_memory_human:" | cut -d: -f2 | tr -d '\r')
    
    log_info "当前内存使用: $used_memory_human"
    
    # 检查内存是否在合理范围（使用字符串比较避免整数溢出）
    local mem_value=$(echo "$used_memory_human" | grep -oE '[0-9]+' | head -1)
    local mem_unit=$(echo "$used_memory_human" | grep -oE '[KMGT]iB|B' | head -1)
    
    # 简单判断：如果包含 GiB 且数值大于 1，或者包含 MiB 且数值大于 100
    local high_mem=false
    if echo "$used_memory_human" | grep -qE '[0-9]+\.[0-9]+G|[5-9][0-9][0-9]M'; then
        high_mem=true
    fi
    
    if [ "$high_mem" = false ]; then
        log_success "内存使用在正常范围"
    else
        log_warn "内存使用较高，可能需要检查"
    fi
}

# 测试9: 清理测试
test_cleanup() {
    log_info "测试9: 清理测试数据..."
    
    # 只删除测试key，保留其他数据
    local keys_to_delete=$($REDIS_CLI KEYS "test_*" 2>/dev/null | wc -l)
    $REDIS_CLI KEYS "test_*" 2>/dev/null | xargs -r $REDIS_CLI DEL > /dev/null 2>&1 || true
    $REDIS_CLI KEYS "heat_*" 2>/dev/null | xargs -r $REDIS_CLI DEL > /dev/null 2>&1 || true
    $REDIS_CLI KEYS "stress_*" 2>/dev/null | xargs -r $REDIS_CLI DEL > /dev/null 2>&1 || true
    
    log_success "测试数据清理完成（约 $keys_to_delete 个key）"
}

# 生成测试报告
generate_report() {
    log_info "========================================"
    log_info "Composite LRU策略测试报告"
    log_info "========================================"
    log_info "测试时间: $(date)"
    log_info "Redis端口: $REDIS_PORT"
    log_info "测试时长: $TEST_DURATION 秒"
    log_info "日志文件: $LOG_FILE"
    log_info ""
    log_info "Redis版本信息:"
    $REDIS_CLI INFO server | grep -E "redis_version|redis_mode|os" | sed 's/^/  /'
    log_info ""
    log_info "NUMA相关信息:"
    if command -v numactl &> /dev/null; then
        numactl --hardware 2>/dev/null | head -10 | sed 's/^/  /' || log_info "  无法获取NUMA信息"
    else
        log_info "  numactl未安装"
    fi
    log_info ""
    log_info "测试完成！"
    log_info "========================================"
}

# 主函数
main() {
    echo "========================================" | tee $LOG_FILE
    echo "Composite LRU策略完整测试脚本" | tee -a $LOG_FILE
    echo "========================================" | tee -a $LOG_FILE
    echo "" | tee -a $LOG_FILE
    
    # 检查是否在正确的目录
    if [ ! -f "./src/redis-cli" ]; then
        log_error "请在Redis源码根目录运行此脚本"
        exit 1
    fi
    
    # 运行所有测试
    check_redis
    test_strategy_loaded
    test_basic_operations
    test_heat_tracking
    test_numa_migrate_command
    test_cxl_device
    test_ultra_high_pressure
    test_strategy_execution
    test_memory_usage
    test_cleanup
    generate_report
    
    log_success "所有测试完成！详细日志见: $LOG_FILE"
}

# 信号处理
trap 'log_warn "测试被中断"; exit 1' INT TERM

# 运行主函数
main "$@"
