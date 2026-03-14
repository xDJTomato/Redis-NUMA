#!/bin/bash
# 测试关键函数是否会卡住

REDIS_CLI="../src/redis-cli"
REDIS_HOST="127.0.0.1"
REDIS_PORT=6379

echo "=== 测试1: NUMACONFIG GET ==="
timeout 5 $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT NUMACONFIG GET 2>&1 | head -10
echo "返回码: $?"
echo ""

echo "=== 测试2: NUMACONFIG STATS ==="
timeout 5 $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT NUMACONFIG STATS 2>&1 | head -20
echo "返回码: $?"
echo ""

echo "=== 测试3: INFO memory ==="
timeout 5 $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT INFO memory 2>&1 | head -10
echo "返回码: $?"
echo ""

echo "=== 测试4: numastat ==="
redis_pid=$(pgrep -f "redis-server.*$REDIS_PORT" | head -1)
if [ -n "$redis_pid" ]; then
    echo "Redis PID: $redis_pid"
    timeout 5 numastat -p "$redis_pid" 2>&1 | head -15
    echo "返回码: $?"
else
    echo "Redis进程未找到"
fi
