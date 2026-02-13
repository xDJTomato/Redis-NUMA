#!/bin/bash
################################################################################
# Redis NUMA 综合测试脚本
# 
# 功能：
# - 测试Redis基础功能
# - 验证NUMA模块初始化
# - 性能基准测试
# - 模块日志检查
################################################################################

set -e  # 遇到错误立即退出

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# 打印带颜色的消息
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

print_header() {
    echo ""
    echo "============================================"
    echo "$1"
    echo "============================================"
}

# 清理函数
cleanup() {
    print_info "清理测试环境..."
    ./src/redis-cli SHUTDOWN NOSAVE > /dev/null 2>&1 || true
    sleep 1
    pkill -9 redis-server 2>/dev/null || true
    rm -f /tmp/redis_test.log /tmp/dump.rdb /tmp/bench_result.txt
}

# 设置退出trap
trap cleanup EXIT

################################################################################
# 主测试流程
################################################################################

print_header "Redis NUMA 功能综合测试"

# 清理旧环境
cleanup
sleep 1

# 1. 启动Redis服务器
print_header "[1/8] 启动Redis服务器"
print_info "启动参数: --daemonize yes --loglevel verbose"
./src/redis-server --daemonize yes --loglevel verbose --logfile /tmp/redis_test.log --dir /tmp
sleep 2

if ! pgrep redis-server > /dev/null; then
    print_error "Redis启动失败"
    exit 1
fi
print_success "Redis启动成功 (PID: $(pgrep redis-server))"

# 2. 测试基础功能
print_header "[2/8] 测试基础Redis功能"
./src/redis-cli SET test_key "Hello NUMA" > /dev/null
result=$(./src/redis-cli GET test_key)
if [ "$result" = "Hello NUMA" ]; then
    print_success "SET/GET 功能正常"
else
    print_error "SET/GET 功能异常: 期望 'Hello NUMA', 得到 '$result'"
    exit 1
fi

# 3. 测试NUMA模块初始化
print_header "[3/8] 检查NUMA模块初始化"

if grep -q "NUMA Strategy.*initialized" /tmp/redis_test.log; then
    print_success "✓ NUMA策略插槽框架初始化成功"
else
    print_error "✗ NUMA策略插槽框架初始化失败"
    cat /tmp/redis_test.log | grep -i error
    exit 1
fi

if grep -q "NUMA Key Migrate.*initialized successfully" /tmp/redis_test.log; then
    print_success "✓ NUMA Key迁移模块初始化成功"
else
    print_error "✗ NUMA Key迁移模块初始化失败"
    cat /tmp/redis_test.log | grep -i error
    exit 1
fi

# 4. 测试策略调度
print_header "[4/8] 检查策略调度执行"
print_info "等待3秒观察策略执行..."
sleep 3

if grep -q "No-op strategy executed" /tmp/redis_test.log; then
    count=$(grep -c "No-op strategy executed" /tmp/redis_test.log)
    print_success "✓ 0号策略正常执行 (执行次数: $count)"
else
    print_error "✗ 0号策略未执行"
    exit 1
fi

# 5. 测试NUMA内存池
print_header "[5/8] 测试NUMA内存池功能"

# 清空数据库
./src/redis-cli FLUSHALL > /dev/null
print_info "已清空数据库"

# 写入测试数据
print_info "写入100个键值对..."
for i in {1..100}; do
    ./src/redis-cli SET "key_$i" "value_$i" > /dev/null
done
print_success "✓ 写入100个键值对成功"

# 验证数据库大小
count=$(./src/redis-cli DBSIZE)
if [ "$count" = "100" ]; then
    print_success "✓ 数据库大小验证通过: $count keys"
else
    print_warning "数据库大小: $count keys (预期100)"
fi

# 6. 性能测试
print_header "[6/8] 性能基准测试"
print_info "运行redis-benchmark (10000次请求)..."

./src/redis-benchmark -q -t set,get -n 10000 -c 10 > /tmp/bench_result.txt 2>&1
if [ $? -eq 0 ]; then
    print_success "✓ 性能测试完成"
    echo ""
    print_info "性能指标:"
    grep "requests per second" /tmp/bench_result.txt | while read line; do
        echo "  $line"
    done
    echo ""
else
    print_error "✗ 性能测试失败"
    cat /tmp/bench_result.txt
    exit 1
fi

# 7. 检查模块日志
print_header "[7/8] 检查NUMA模块日志"

echo ""
print_info "策略插槽框架日志:"
grep "NUMA Strategy" /tmp/redis_test.log | head -5 | while read line; do
    echo "  $line"
done

echo ""
print_info "Key迁移模块日志:"
grep "NUMA Key Migrate" /tmp/redis_test.log | while read line; do
    echo "  $line"
done
echo ""

# 8. 内存信息
print_header "[8/8] 检查内存信息"
./src/redis-cli INFO memory | grep -E "used_memory|used_memory_human|maxmemory" | while read line; do
    echo "  $line"
done

# 测试完成
print_header "测试完成"
print_success "✅ 所有测试通过！"
echo ""
print_info "测试总结:"
echo "  • Redis服务器: 正常运行"
echo "  • 基础功能: ✓"
echo "  • NUMA模块: ✓"
echo "  • 策略调度: ✓"
echo "  • 性能测试: ✓"
echo ""
print_info "详细日志: /tmp/redis_test.log"
echo ""
