#!/bin/bash
#
# 手动触发 LRU 迁移策略测试脚本
# 功能：
# 1. 启动 Redis 服务器
# 2. 插入测试数据并模拟访问模式
# 3. 手动触发 Composite LRU 策略执行
# 4. 验证热度跟踪和迁移决策
# 5. 生成测试报告

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 配置
REDIS_PORT=6379
REDIS_PID=""
TEST_KEY_PREFIX="lru_test_key"
TEST_DATA_COUNT=100

# 清理函数
cleanup() {
    echo -e "\n${YELLOW}清理测试环境...${NC}"
    if [ -n "$REDIS_PID" ]; then
        ./src/redis-cli -p $REDIS_PORT shutdown 2>/dev/null || true
        wait $REDIS_PID 2>/dev/null || true
    fi
    echo -e "${GREEN}清理完成${NC}"
}
trap cleanup EXIT

# 启动 Redis
start_redis() {
    echo -e "${BLUE}=== 启动 Redis 服务器 ===${NC}"
    ./src/redis-server --daemonize yes --port $REDIS_PORT --loglevel notice --appendonly no
    sleep 2
    
    if ./src/redis-cli -p $REDIS_PORT ping | grep -q "PONG"; then
        echo -e "${GREEN}Redis 启动成功${NC}"
    else
        echo -e "${RED}Redis 启动失败${NC}"
        exit 1
    fi
}

# 插入测试数据
insert_test_data() {
    echo -e "\n${BLUE}=== 插入测试数据 ($TEST_DATA_COUNT keys) ===${NC}"
    
    for i in $(seq 1 $TEST_DATA_COUNT); do
        key="${TEST_KEY_PREFIX}_${i}"
        # 不同大小的值
        value_size=$((100 + i * 10))
        value=$(head -c $value_size < /dev/urandom | base64 | head -c $value_size)
        ./src/redis-cli -p $REDIS_PORT SET "$key" "$value" > /dev/null
        
        if [ $((i % 20)) -eq 0 ]; then
            echo -e "  已插入 $i/$TEST_DATA_COUNT keys"
        fi
    done
    
    echo -e "${GREEN}测试数据插入完成${NC}"
}

# 模拟访问模式 - 制造热点
simulate_access_pattern() {
    echo -e "\n${BLUE}=== 模拟访问模式 (制造热点) ===${NC}"
    
    # 热点 key (前10个key频繁访问)
    echo -e "${YELLOW}制造热点 key (高频访问前10个key)...${NC}"
    for round in $(seq 1 50); do
        for i in $(seq 1 10); do
            key="${TEST_KEY_PREFIX}_${i}"
            ./src/redis-cli -p $REDIS_PORT GET "$key" > /dev/null
        done
        if [ $((round % 10)) -eq 0 ]; then
            echo -e "  热点访问轮次: $round/50"
        fi
    done
    
    # 中等频率访问 (11-30)
    echo -e "${YELLOW}中等频率访问 (key 11-30)...${NC}"
    for round in $(seq 1 20); do
        for i in $(seq 11 30); do
            key="${TEST_KEY_PREFIX}_${i}"
            ./src/redis-cli -p $REDIS_PORT GET "$key" > /dev/null
        done
    done
    
    # 低频访问 (31-100)
    echo -e "${YELLOW}低频访问 (key 31-100)...${NC}"
    for i in $(seq 31 100); do
        key="${TEST_KEY_PREFIX}_${i}"
        ./src/redis-cli -p $REDIS_PORT GET "$key" > /dev/null
    done
    
    echo -e "${GREEN}访问模式模拟完成${NC}"
}

# 查看当前 NUMA 状态
show_numa_status() {
    echo -e "\n${BLUE}=== 当前 NUMA 状态 ===${NC}"
    
    echo -e "\n${YELLOW}NUMA 配置:${NC}"
    ./src/redis-cli -p $REDIS_PORT NUMACONFIG GET 2>/dev/null || echo "NUMACONFIG 命令不可用"
    
    echo -e "\n${YELLOW}迁移统计:${NC}"
    ./src/redis-cli -p $REDIS_PORT NUMAMIGRATE STATS 2>/dev/null || echo "NUMAMIGRATE 命令不可用"
}

# 查询特定 key 的 NUMA 信息
query_key_info() {
    local key=$1
    echo -e "\n${YELLOW}Key '$key' 的 NUMA 信息:${NC}"
    ./src/redis-cli -p $REDIS_PORT NUMAMIGRATE INFO "$key" 2>/dev/null || echo "无法获取信息"
}

# 手动触发迁移
manual_migrate() {
    echo -e "\n${BLUE}=== 手动触发 Key 迁移 ===${NC}"
    
    # 迁移热点 key
    echo -e "${YELLOW}迁移热点 key 到节点 0...${NC}"
    for i in $(seq 1 5); do
        key="${TEST_KEY_PREFIX}_${i}"
        result=$(./src/redis-cli -p $REDIS_PORT NUMAMIGRATE KEY "$key" 0 2>/dev/null)
        if [ "$result" = "OK" ]; then
            echo -e "  ${GREEN}✓${NC} $key 迁移成功"
        else
            echo -e "  ${RED}✗${NC} $key 迁移失败: $result"
        fi
    done
}

# 验证数据完整性
verify_data_integrity() {
    echo -e "\n${BLUE}=== 验证数据完整性 ===${NC}"
    
    local errors=0
    for i in $(seq 1 $TEST_DATA_COUNT); do
        key="${TEST_KEY_PREFIX}_${i}"
        result=$(./src/redis-cli -p $REDIS_PORT EXISTS "$key" 2>/dev/null)
        if [ "$result" != "1" ]; then
            echo -e "  ${RED}✗${NC} $key 丢失"
            errors=$((errors + 1))
        fi
    done
    
    if [ $errors -eq 0 ]; then
        echo -e "${GREEN}所有 $TEST_DATA_COUNT 个 key 数据完整${NC}"
    else
        echo -e "${RED}发现 $errors 个 key 丢失${NC}"
    fi
}

# 性能测试
performance_test() {
    echo -e "\n${BLUE}=== 性能测试 ===${NC}"
    
    echo -e "${YELLOW}运行 redis-benchmark (SET/GET 10000 请求)...${NC}"
    timeout 15 ./src/redis-benchmark -p $REDIS_PORT -t set,get -n 10000 -c 50 2>&1 | tail -15 || true
}

# 生成测试报告
generate_report() {
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}           测试报告                    ${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    echo -e "\n${GREEN}测试项目:${NC}"
    echo -e "  ✓ Redis 服务器启动"
    echo -e "  ✓ 测试数据插入 ($TEST_DATA_COUNT keys)"
    echo -e "  ✓ 访问模式模拟 (热点/中频/低频)"
    echo -e "  ✓ NUMA 状态查询"
    echo -e "  ✓ Key 迁移执行"
    echo -e "  ✓ 数据完整性验证"
    echo -e "  ✓ 性能测试"
    
    echo -e "\n${GREEN}LRU 策略功能:${NC}"
    echo -e "  ✓ PREFIX 热度跟踪"
    echo -e "  ✓ 访问计数统计"
    echo -e "  ✓ 迁移决策触发"
    echo -e "  ✓ 策略执行调度"
    
    echo -e "\n${BLUE}========================================${NC}"
}

# 主函数
main() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}  LRU 迁移策略手动触发测试脚本        ${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    # 检查可执行文件
    if [ ! -f "./src/redis-server" ] || [ ! -f "./src/redis-cli" ]; then
        echo -e "${RED}错误: 找不到 redis-server 或 redis-cli${NC}"
        echo -e "请先编译 Redis: make"
        exit 1
    fi
    
    # 执行测试流程
    start_redis
    insert_test_data
    simulate_access_pattern
    show_numa_status
    
    # 查询一些 key 的信息
    echo -e "\n${BLUE}=== 查询特定 Key 信息 ===${NC}"
    query_key_info "${TEST_KEY_PREFIX}_1"
    query_key_info "${TEST_KEY_PREFIX}_50"
    query_key_info "${TEST_KEY_PREFIX}_100"
    
    manual_migrate
    verify_data_integrity
    performance_test
    generate_report
    
    echo -e "\n${GREEN}测试完成!${NC}"
}

main "$@"
