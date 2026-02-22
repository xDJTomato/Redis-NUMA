#!/bin/bash
#
# YCSB NUMA 高强度迁移策略测试脚本
# 测试目标：通过多阶段加载覆盖 SLAB/Pool/Direct 三种内存分配
# 内存压力目标：> 8GB
# 
# 测试策略：
# 1. Phase 1: 加载小对象 (64B) - SLAB 分配
# 2. Phase 2: 加载中对象 (8KB) - Pool 分配
# 3. Phase 3: 加载大对象 (32KB) - Direct 分配
# 4. Phase 4: 加载超大对象 (1MB) - Direct 分配
# 5. Phase 5: 热点访问测试

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
YCSB_BIN="$YCSB_DIR/ycsb-0.17.0/bin/ycsb"

REDIS_PORT=6379
REDIS_HOST="127.0.0.1"
REDIS_LOG="$RESULTS_DIR/redis_high_pressure.log"
TEST_REPORT="$RESULTS_DIR/high_pressure_report_$(date +%Y%m%d_%H%M%S).txt"

# 创建结果目录
mkdir -p "$RESULTS_DIR"

# 测试阶段配置
declare -A PHASES
PHASES[small]="64:500000:small"      # 大小:记录数:前缀
PHASES[medium]="8192:500000:medium"  # 8KB
PHASES[large]="32768:500000:large"   # 32KB
PHASES[xlarge]="1048576:50000:xlarge" # 1MB

echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}  NUMA 高强度迁移策略测试            ${NC}"
echo -e "${BLUE}========================================${NC}"
echo -e "内存压力目标: > 8GB"
echo -e "总记录数: 1,550,000"
echo -e "预期内存分布:"
echo -e "  - 小对象 (64B):   500K × 64B   = 32 MB"
echo -e "  - 中对象 (8KB):   500K × 8KB   = 4 GB"
echo -e "  - 大对象 (32KB):  500K × 32KB  = 16 GB (热点约6-8GB)"
echo -e "  - 超大对象 (1MB): 50K × 1MB    = 50 GB (热点约2-3GB)"
echo -e "  - 总计预期: 10-12 GB"
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
    echo -e "${BLUE}=== 启动 Redis (VERBOSE 日志, 16GB maxmemory) ===${NC}"
    
    # 停止旧进程
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT shutdown 2>/dev/null || true
    sleep 2
    
    # 清理日志
    > "$REDIS_LOG"
    
    # 启动 Redis - 分配16GB内存以容纳测试数据
    "$REDIS_DIR/src/redis-server" \
        --bind 127.0.0.1 \
        --port $REDIS_PORT \
        --protected-mode no \
        --daemonize yes \
        --loglevel verbose \
        --logfile "$REDIS_LOG" \
        --maxmemory 16gb \
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
    
    echo "设置策略为 Composite LRU..."
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMACONFIG SET strategy composite_lru || true
    
    echo -e "\n当前 NUMA 配置:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMACONFIG GET
}

# 使用 redis-cli 直接插入数据（绕过 YCSB 限制）
load_data_with_redis_cli() {
    local size=$1
    local count=$2
    local prefix=$3
    
    echo -e "\n${BLUE}=== 加载数据: ${prefix} (${size}B × ${count}) ===${NC}"
    
    # 生成指定大小的值
    local value=$(head -c $size </dev/zero | tr '\0' 'x')
    
    local start_time=$(date +%s)
    local batch_size=1000
    local loaded=0
    
    # 使用 redis-cli 批量插入
    for ((i=0; i<count; i+=batch_size)); do
        local batch_end=$((i + batch_size))
        if [ $batch_end -gt $count ]; then
            batch_end=$count
        fi
        
        # 构建批量命令
        local commands=""
        for ((j=i; j<batch_end; j++)); do
            commands+="SET ${prefix}:key:${j} \"${value}\"\n"
        done
        
        # 执行批量插入
        echo -e "$commands" | "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT --pipe > /dev/null 2>&1
        
        loaded=$batch_end
        
        # 显示进度
        if [ $((loaded % 10000)) -eq 0 ]; then
            local percent=$((loaded * 100 / count))
            echo -ne "\r  进度: ${loaded}/${count} (${percent}%)  "
        fi
    done
    
    local end_time=$(date +%s)
    local duration=$((end_time - start_time))
    
    echo -e "\n${GREEN}完成: ${loaded} 条记录，耗时 ${duration}s${NC}"
}

# 运行热点访问测试
run_hotspot_test() {
    echo -e "\n${BLUE}=== 热点访问测试 (5,000,000 次操作) ===${NC}"
    
    local workload="$RESULTS_DIR/workload_hotspot_temp"
    
    # 创建临时工作负载配置
    cat > "$workload" <<EOF
recordcount=1550000
operationcount=5000000
workload=site.ycsb.workloads.CoreWorkload

readproportion=0.65
updateproportion=0.25
scanproportion=0
insertproportion=0.1

requestdistribution=hotspot
hotspotdatafraction=0.2
hotspotopnfraction=0.8

fieldcount=1
fieldlength=64

threadcount=100

redis.host=$REDIS_HOST
redis.port=$REDIS_PORT
redis.timeout=10000
redis.database=0
EOF
    
    local output="$RESULTS_DIR/high_pressure_run_$(date +%Y%m%d_%H%M%S).log"
    
    echo "执行 YCSB Run 阶段..."
    if java -cp "$YCSB_DIR/ycsb-0.17.0/lib/*:$YCSB_DIR/ycsb-0.17.0/redis-binding/lib/*" \
        site.ycsb.Client -t \
        -db site.ycsb.db.RedisClient \
        -P "$workload" \
        -s \
        -threads 100 \
        > "$output" 2>&1; then
        
        cat "$output"
        echo -e "${GREEN}热点测试完成${NC}"
    else
        echo -e "${RED}热点测试失败${NC}"
        cat "$output"
    fi
    
    rm -f "$workload"
}

# 收集 Redis 统计信息
collect_redis_stats() {
    echo -e "\n${BLUE}=== Redis 统计信息 ===${NC}"
    
    echo -e "\n命令统计:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO stats | grep -E "total_commands_processed|keyspace_hits|keyspace_misses"
    
    echo -e "\n内存统计:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory | grep -E "used_memory_human|maxmemory|mem_fragmentation"
    
    echo -e "\nKeyspace 统计:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO keyspace
    
    echo -e "\nNUMA 迁移统计:"
    "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT NUMASTATS | grep -E "migrations|bytes_migrated|migration_time"
}

# 分析迁移日志
analyze_migration_logs() {
    echo -e "\n${BLUE}=== 分析迁移日志 ===${NC}"
    
    echo -e "\n策略初始化:"
    grep "Strategy initialized" "$REDIS_LOG" | tail -1
    
    echo -e "\n远程访问检测:"
    local remote_access=$(grep -c "Remote access detected" "$REDIS_LOG" 2>/dev/null || echo "0")
    echo "  总计: $remote_access"
    
    echo -e "\n迁移触发事件:"
    local migrations=$(grep -c "MIGRATION TRIGGERED" "$REDIS_LOG" 2>/dev/null || echo "0")
    echo "  总计: $migrations"
    
    if [ "$migrations" -gt 0 ]; then
        echo -e "\n最近 5 次迁移:"
        grep "MIGRATION TRIGGERED" "$REDIS_LOG" | tail -5
    fi
    
    echo -e "\n热度衰减周期:"
    local decays=$(grep -c "Executing heat decay cycle" "$REDIS_LOG" 2>/dev/null || echo "0")
    echo "  总计: $decays"
}

# 生成测试报告
generate_report() {
    echo -e "\n${BLUE}=== 生成测试报告 ===${NC}"
    
    {
        echo "========================================"
        echo "NUMA 高强度迁移策略测试报告"
        echo "========================================"
        echo "测试时间: $(date)"
        echo ""
        echo "内存压力目标: > 8GB"
        echo "总记录数: 1,550,000"
        echo ""
        echo "数据分布:"
        echo "  - 小对象 (64B):   500,000 条"
        echo "  - 中对象 (8KB):   500,000 条"
        echo "  - 大对象 (32KB):  500,000 条"
        echo "  - 超大对象 (1MB): 50,000 条"
        echo ""
        
        # Redis 统计
        echo "Redis 内存使用:"
        "$REDIS_DIR/src/redis-cli" -p $REDIS_PORT INFO memory | grep "used_memory_human:" | sed 's/used_memory_human:/  /'
        
        echo ""
        echo "迁移统计:"
        local remote_access=$(grep -c "Remote access detected" "$REDIS_LOG" 2>/dev/null || echo "0")
        local migrations=$(grep -c "MIGRATION TRIGGERED" "$REDIS_LOG" 2>/dev/null || echo "0")
        local decays=$(grep -c "Executing heat decay cycle" "$REDIS_LOG" 2>/dev/null || echo "0")
        
        echo "  远程访问检测: $remote_access"
        echo "  迁移触发: $migrations"
        echo "  热度衰减: $decays"
        
        echo ""
        echo "详细日志: $REDIS_LOG"
        echo "========================================"
    } | tee "$TEST_REPORT"
    
    echo -e "${GREEN}报告已保存到: $TEST_REPORT${NC}"
}

# ==================== 主流程 ====================

# 1. 启动 Redis
start_redis

# 2. 配置 NUMA 策略
configure_numa_strategy

echo -e "\n${YELLOW}等待 5 秒让系统稳定...${NC}"
sleep 5

# 3. 多阶段数据加载
echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}  阶段 1: 加载小对象 (SLAB 分配)      ${NC}"
echo -e "${BLUE}========================================${NC}"
load_data_with_redis_cli 64 500000 "small"

echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}  阶段 2: 加载中对象 (Pool 分配)      ${NC}"
echo -e "${BLUE}========================================${NC}"
load_data_with_redis_cli 8192 500000 "medium"

echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}  阶段 3: 加载大对象 (Direct 分配)    ${NC}"
echo -e "${BLUE}========================================${NC}"
load_data_with_redis_cli 32768 500000 "large"

echo -e "\n${BLUE}========================================${NC}"
echo -e "${BLUE}  阶段 4: 加载超大对象 (Direct 分配)  ${NC}"
echo -e "${BLUE}========================================${NC}"
load_data_with_redis_cli 1048576 50000 "xlarge"

# 4. 收集加载后统计
collect_redis_stats

echo -e "\n${YELLOW}等待 10 秒让系统稳定...${NC}"
sleep 10

# 5. 热点访问测试
run_hotspot_test

# 6. 收集最终统计
collect_redis_stats

# 7. 分析迁移日志
analyze_migration_logs

# 8. 生成报告
generate_report

echo -e "\n${GREEN}测试完成！${NC}"
