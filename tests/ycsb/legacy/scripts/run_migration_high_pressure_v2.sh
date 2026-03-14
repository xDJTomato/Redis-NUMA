#!/bin/bash
#
# YCSB NUMA 高强度迁移策略测试脚本 (优化版)
# 使用 Lua 脚本高效批量生成数据
# 内存压力目标：> 8GB

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

REDIS_PORT=6379
REDIS_HOST="127.0.0.1"
REDIS_LOG="$RESULTS_DIR/redis_high_pressure.log"
TEST_REPORT="$RESULTS_DIR/high_pressure_report_$(date +%Y%m%d_%H%M%S).txt"

mkdir -p "$RESULTS_DIR"

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  NUMA 高强度迁移策略测试 (优化版)   ${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "内存压力目标: > 8GB"
echo -e "数据分布计划:"
echo -e "  - 小对象 (64B):    200K × 64B   = 12.8 MB"
echo -e "  - 中对象 (4KB):    400K × 4KB   = 1.6 GB"
echo -e "  - 大对象 (32KB):   200K × 32KB  = 6.4 GB"
echo -e "  - 超大对象 (128KB): 20K × 128KB = 2.5 GB"
echo -e "  - 总计: 820K 条记录，预期 ~10.5 GB"
echo ""

# 清理函数
cleanup() {
    echo -e "\n${YELLOW}清理环境...${NC}"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT shutdown 2>/dev/null || true
    echo -e "${GREEN}清理完成${NC}"
}
trap cleanup EXIT

# 启动 Redis
start_redis() {
    echo -e "${BLUE}=== 启动 Redis ===${NC}"
    
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT shutdown 2>/dev/null || true
    sleep 2
    
    > "$REDIS_LOG"
    
    "$REDIS_DIR/src/redis-server" \
        --bind 127.0.0.1 \
        --port $REDIS_PORT \
        --protected-mode no \
        --daemonize yes \
        --loglevel verbose \
        --logfile "$REDIS_LOG" \
        --maxmemory 16gb \
        --maxmemory-policy noeviction \
        --save "" \
        --appendonly no
    
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
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMACONFIG SET strategy composite_lru || true
    echo -e "\n当前配置:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMACONFIG GET
}

# 使用 Lua 脚本批量生成数据
load_data_lua() {
    local size=$1
    local count=$2
    local prefix=$3
    
    echo -e "\n${BLUE}=== 加载数据: ${prefix} (${size}B × ${count}) ===${NC}"
    
    local start_time=$(date +%s)
    
    # 创建 Lua 脚本批量生成
    local lua_script="
local prefix = ARGV[1]
local size = tonumber(ARGV[2])
local batch_start = tonumber(ARGV[3])
local batch_end = tonumber(ARGV[4])
local value = string.rep('x', size)
local count = 0

for i = batch_start, batch_end do
    redis.call('SET', prefix .. ':' .. i, value)
    count = count + 1
end

return count
"
    
    # 批量执行
    local batch_size=5000
    local loaded=0
    
    for ((i=0; i<count; i+=batch_size)); do
        local batch_end=$((i + batch_size - 1))
        if [ $batch_end -ge $count ]; then
            batch_end=$((count - 1))
        fi
        
        # 执行 Lua 脚本
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT EVAL "$lua_script" 0 "$prefix" "$size" "$i" "$batch_end" > /dev/null
        
        loaded=$((batch_end + 1))
        
        # 显示进度
        if [ $((loaded % 10000)) -eq 0 ] || [ $loaded -eq $count ]; then
            local percent=$((loaded * 100 / count))
            echo -ne "\r  进度: ${loaded}/${count} (${percent}%)  "
        fi
    done
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    local rate=$((count / duration))
    
    echo -e "\n${GREEN}完成: ${loaded} 条记录，耗时 ${duration}s (${rate} keys/s)${NC}"
}

# 收集统计信息
collect_stats() {
    echo -e "\n${BLUE}=== Redis 统计信息 ===${NC}"
    
    echo -e "\n内存使用:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory | grep -E "used_memory_human|maxmemory_human|mem_fragmentation_ratio"
    
    echo -e "\nKeyspace:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO keyspace
    
    echo -e "\nNUMA 统计:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMASTATS | head -20
}

# 热点访问测试
run_hotspot_access() {
    echo -e "\n${BLUE}=== 热点访问测试 (100 万次操作) ===${NC}"
    
    # 使用 redis-benchmark 模拟热点访问
    "$REDIS_DIR/src/redis-benchmark" \
        -h $REDIS_HOST \
        -p $REDIS_PORT \
        -n 1000000 \
        -c 100 \
        -t get,set \
        -r 100000 \
        --csv 2>/dev/null | grep -E "GET|SET"
    
    echo -e "${GREEN}热点测试完成${NC}"
}

# 分析日志
analyze_logs() {
    echo -e "\n${BLUE}=== 迁移日志分析 ===${NC}"
    
    echo -e "\n策略初始化:"
    grep "Strategy initialized" "$REDIS_LOG" | tail -1
    
    local remote=$(grep -c "Remote access detected" "$REDIS_LOG" 2>/dev/null | tr -d '\n\r' || echo "0")
    local migrations=$(grep -c "MIGRATION TRIGGERED" "$REDIS_LOG" 2>/dev/null | tr -d '\n\r' || echo "0")
    local decays=$(grep -c "Executing heat decay cycle" "$REDIS_LOG" 2>/dev/null | tr -d '\n\r' || echo "0")
    
    echo -e "\n事件统计:"
    echo "  远程访问检测: $remote"
    echo "  迁移触发: $migrations"
    echo "  热度衰减: $decays"
    
    if [ "$migrations" -gt 0 ]; then
        echo -e "\n最近 5 次迁移:"
        grep "MIGRATION TRIGGERED" "$REDIS_LOG" | tail -5
    fi
}

# 生成报告
generate_report() {
    echo -e "\n${BLUE}=== 生成测试报告 ===${NC}"
    
    {
        echo "========================================"
        echo "NUMA 高强度迁移策略测试报告"
        echo "========================================"
        echo "测试时间: $(date)"
        echo ""
        
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory | grep "used_memory_human"
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO keyspace
        
        echo ""
        local remote=$(grep -c "Remote access detected" "$REDIS_LOG" 2>/dev/null | tr -d '\n\r' || echo "0")
        local migrations=$(grep -c "MIGRATION TRIGGERED" "$REDIS_LOG" 2>/dev/null | tr -d '\n\r' || echo "0")
        
        echo "迁移事件: 远程访问=$remote, 迁移触发=$migrations"
        echo ""
        echo "详细日志: $REDIS_LOG"
        echo "========================================"
    } | tee "$TEST_REPORT"
    
    echo -e "${GREEN}报告: $TEST_REPORT${NC}"
}

# ==================== 主流程 ====================

start_redis
configure_numa_strategy

echo -e "\n${YELLOW}等待 5 秒...${NC}"
sleep 5

# 多阶段加载
echo -e "\n${BLUE}阶段 1: 小对象 (SLAB)${NC}"
load_data_lua 64 200000 "small"

echo -e "\n${BLUE}阶段 2: 中对象 (Pool)${NC}"
load_data_lua 4096 400000 "medium"

echo -e "\n${BLUE}阶段 3: 大对象 (Direct)${NC}"
load_data_lua 32768 200000 "large"

echo -e "\n${BLUE}阶段 4: 超大对象 (Direct)${NC}"
load_data_lua 131072 20000 "xlarge"

collect_stats

echo -e "\n${YELLOW}等待 10 秒让系统稳定...${NC}"
sleep 10

run_hotspot_access
collect_stats
analyze_logs
generate_report

echo -e "\n${GREEN}测试完成！${NC}"
