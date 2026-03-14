#!/bin/bash
#
# NUMA 测试统一入口脚本
# 运行所有基础测试并生成报告

set -e

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# 测试目录
TEST_DIR="$(cd "$(dirname "$0")" && pwd)"
REDIS_DIR="$(dirname "$(dirname "$TEST_DIR")")"

# 测试结果
PASSED=0
FAILED=0
SKIPPED=0

# 打印标题
print_header() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}========================================${NC}"
}

# 运行单个测试
run_test() {
    local test_name=$1
    local test_script=$2
    
    echo -e "\n${YELLOW}>>> 运行: $test_name${NC}"
    
    if [ ! -f "$test_script" ]; then
        echo -e "${RED}✗ 测试脚本不存在: $test_script${NC}"
        FAILED=$((FAILED + 1))
        return 1
    fi
    
    # 运行测试
    if timeout 120 "$test_script" > /tmp/test_${test_name}.log 2>&1; then
        echo -e "${GREEN}✓ 通过${NC}"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo -e "${RED}✗ 失败 (查看 /tmp/test_${test_name}.log)${NC}"
        FAILED=$((FAILED + 1))
        return 1
    fi
}

# 编译单元测试
compile_unit_tests() {
    echo -e "\n${YELLOW}>>> 编译单元测试...${NC}"
    
    cd "$REDIS_DIR"
    
    # 编译 test_prefix_heat
    if gcc -DHAVE_NUMA -o "$TEST_DIR/test_prefix_heat" "$TEST_DIR/test_prefix_heat.c" \
        src/zmalloc.o src/numa_pool.o -I. -lnuma -lpthread 2>/dev/null; then
        echo -e "${GREEN}✓ test_prefix_heat 编译成功${NC}"
    else
        echo -e "${RED}✗ test_prefix_heat 编译失败${NC}"
    fi
    
    # 编译 test_prefix_heat_direct
    if gcc -DHAVE_NUMA -o "$TEST_DIR/test_prefix_heat_direct" "$TEST_DIR/test_prefix_heat_direct.c" \
        src/zmalloc.o src/numa_pool.o -I. -lnuma -lpthread 2>/dev/null; then
        echo -e "${GREEN}✓ test_prefix_heat_direct 编译成功${NC}"
    else
        echo -e "${RED}✗ test_prefix_heat_direct 编译失败${NC}"
    fi
}

# 运行单元测试
run_unit_tests() {
    echo -e "\n${YELLOW}>>> 运行单元测试...${NC}"
    
    cd "$TEST_DIR"
    
    # test_prefix_heat
    if [ -f "./test_prefix_heat" ]; then
        if timeout 30 ./test_prefix_heat > /tmp/test_prefix_heat.log 2>&1; then
            echo -e "${GREEN}✓ test_prefix_heat${NC}"
            PASSED=$((PASSED + 1))
        else
            echo -e "${RED}✗ test_prefix_heat${NC}"
            FAILED=$((FAILED + 1))
        fi
    else
        echo -e "${YELLOW}⊘ test_prefix_heat (未编译)${NC}"
        SKIPPED=$((SKIPPED + 1))
    fi
    
    # test_prefix_heat_direct
    if [ -f "./test_prefix_heat_direct" ]; then
        if timeout 30 ./test_prefix_heat_direct > /tmp/test_prefix_heat_direct.log 2>&1; then
            echo -e "${GREEN}✓ test_prefix_heat_direct${NC}"
            PASSED=$((PASSED + 1))
        else
            echo -e "${RED}✗ test_prefix_heat_direct${NC}"
            FAILED=$((FAILED + 1))
        fi
    else
        echo -e "${YELLOW}⊘ test_prefix_heat_direct (未编译)${NC}"
        SKIPPED=$((SKIPPED + 1))
    fi
}

# 主函数
main() {
    print_header "Redis NUMA 测试套件"
    
    echo -e "测试目录: $TEST_DIR"
    echo -e "Redis目录: $REDIS_DIR"
    
    # 检查 Redis 是否已编译
    if [ ! -f "$REDIS_DIR/src/redis-server" ]; then
        echo -e "${RED}错误: Redis 未编译，请先运行 'make'${NC}"
        exit 1
    fi
    
    # 编译单元测试
    compile_unit_tests
    
    # 运行功能测试
    echo -e "\n${BLUE}=== 功能测试 ===${NC}"
    run_test "lru_migration" "$TEST_DIR/test_lru_migration.sh"
    run_test "numa_basic" "$TEST_DIR/test_numa.sh"
    run_test "numa_config" "$TEST_DIR/test_numa_config.sh"
    
    # 运行单元测试
    run_unit_tests
    
    # 生成测试报告
    echo -e "\n${BLUE}========================================${NC}"
    echo -e "${BLUE}           测试报告                    ${NC}"
    echo -e "${BLUE}========================================${NC}"
    echo -e "通过: ${GREEN}$PASSED${NC}"
    echo -e "失败: ${RED}$FAILED${NC}"
    echo -e "跳过: ${YELLOW}$SKIPPED${NC}"
    echo -e "总计: $((PASSED + FAILED + SKIPPED))"
    echo -e "${BLUE}========================================${NC}"
    
    if [ $FAILED -eq 0 ]; then
        echo -e "${GREEN}所有测试通过!${NC}"
        exit 0
    else
        echo -e "${RED}有 $FAILED 个测试失败${NC}"
        exit 1
    fi
}

main "$@"
