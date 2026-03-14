#!/bin/bash

# CXL内存平衡测试脚本
# 验证NUMA负载均衡修复效果

REDIS_PORT=${1:-6399}
REDIS_CLI="./src/redis-cli -p $REDIS_PORT"

echo "========================================"
echo "CXL内存平衡测试"
echo "========================================"

# 启动Redis服务器
echo "[INFO] 启动Redis服务器..."
./src/redis-server --port $REDIS_PORT --daemonize yes
sleep 2

# 检查NUMA节点信息
echo "[INFO] NUMA节点信息:"
numactl --hardware

echo "[INFO] 系统内存信息:"
free -h

# 执行高压测试
echo "[INFO] 开始高压测试..."
echo "创建大量数据对象..."

# 使用redis-benchmark创建数据（启用轮询分配）
./src/redis-benchmark -p $REDIS_PORT -n 50000 -r 50000 -d 1024 -t set --csv > /dev/null 2>&1 &
PID1=$!

./src/redis-benchmark -p $REDIS_PORT -n 20000 -r 20000 -d 10240 -t set --csv > /dev/null 2>&1 &
PID2=$!

./src/redis-benchmark -p $REDIS_PORT -n 5000 -r 5000 -d 102400 -t set --csv > /dev/null 2>&1 &
PID3=$!

# 等待数据创建完成
echo "[INFO] 等待数据创建完成..."
wait $PID1 $PID2 $PID3

# 检查内存分布
echo "[INFO] 检查内存使用情况..."
$REDIS_CLI INFO memory | grep -E "used_memory|used_memory_rss"

# 检查各NUMA节点内存使用（如果numastat可用）
if command -v numastat >/dev/null 2>&1; then
    echo "[INFO] NUMA节点内存分布:"
    numastat -c redis-server
fi

# 执行热点访问测试
echo "[INFO] 执行热点访问测试..."
./src/redis-benchmark -p $REDIS_PORT -n 100000 -t get --csv > /dev/null 2>&1

# 检查最终状态
echo "[INFO] 最终状态检查..."
echo "数据库key数量: $($REDIS_CLI DBSIZE)"
$REDIS_CLI INFO memory | grep -E "used_memory_human|used_memory_rss_human"

# 清理测试数据
echo "[INFO] 清理测试数据..."
$REDIS_CLI FLUSHDB

# 停止Redis服务器
echo "[INFO] 停止Redis服务器..."
$REDIS_CLI SHUTDOWN NOSAVE

echo "[INFO] 测试完成！"
echo "如果内存分配均衡，各NUMA节点的使用应该相对平均"