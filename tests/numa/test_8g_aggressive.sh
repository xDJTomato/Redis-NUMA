#!/bin/bash
#===============================================================================
# 8G内存压力测试 - 激进版本
# 目标：实际RSS占用达到8GB
#===============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"

REDIS_SERVER="$PROJECT_ROOT/src/redis-server"
REDIS_CLI="$PROJECT_ROOT/src/redis-cli"
REDIS_BENCH="$PROJECT_ROOT/src/redis-benchmark"

REDIS_PORT=6379
REDIS_HOST="127.0.0.1"

# 目标8GB RSS
TARGET_RSS_GB=8
TARGET_RSS_MB=$((TARGET_RSS_GB * 1024))

# 使用更大的value来更快达到目标
# 1KB value，每个key约占用1.2KB
VALUE_SIZE=1024
KEYS_NEEDED=$((TARGET_RSS_MB * 1024 / 1200))

# 报告目录
REPORT_DIR="$PROJECT_ROOT/tests/reports/fragmentation_8g_aggressive_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$REPORT_DIR"

echo "=========================================="
echo "8G内存压力测试 (激进版)"
echo "=========================================="
echo "目标RSS: ${TARGET_RSS_GB}GB"
echo "Value大小: ${VALUE_SIZE}B"
echo "需要key数: ~$KEYS_NEEDED"
echo "报告目录: $REPORT_DIR"
echo ""

# 清理并启动Redis
pkill -f "redis-server" 2>/dev/null || true
sleep 2

rm -f "$PROJECT_ROOT/dump.rdb" "$PROJECT_ROOT/appendonly.aof"

"$REDIS_SERVER" \
    --daemonize yes \
    --port "$REDIS_PORT" \
    --bind "$REDIS_HOST" \
    --loglevel warning \
    --save "" \
    --appendonly no \
    --maxmemory-policy allkeys-lru 2>&1 | grep -v "DEBUG:" || true

sleep 2
if ! "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING &>/dev/null; then
    echo "ERROR: Redis启动失败"
    exit 1
fi

# 获取内存统计
get_rss_mb() {
    local rss
    rss=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | grep "used_memory_rss:" | cut -d: -f2 | tr -d '\r')
    echo $((rss / 1024 / 1024))
}

get_frag() {
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | grep "mem_fragmentation_ratio:" | cut -d: -f2 | tr -d '\r'
}

# 记录函数
record_state() {
    local phase="$1"
    local rss=$(get_rss_mb)
    local frag=$(get_frag)
    local used=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | grep "used_memory:" | cut -d: -f2 | tr -d '\r')
    local used_mb=$((used / 1024 / 1024))
    
    echo "$(date '+%H:%M:%S'),$phase,$used_mb,$rss,$frag" >> "$REPORT_DIR/timeline.csv"
    echo "[$phase] 有效:${used_mb}MB RSS:${rss}MB 碎片率:$frag"
}

echo "timestamp,phase,used_mb,rss_mb,fragmentation" > "$REPORT_DIR/timeline.csv"

# 初始状态
echo ""
echo "[初始状态]"
record_state "initial"

# 分批填充，直到达到8GB RSS
echo ""
echo "[开始填充到8GB RSS...]"

BATCH_SIZE=1000000
BATCH_NUM=1
TOTAL_KEYS=0

while true; do
    CURRENT_RSS=$(get_rss_mb)
    echo ""
    echo "批次 $BATCH_NUM: 当前RSS=${CURRENT_RSS}MB, 目标=${TARGET_RSS_MB}MB"
    
    if [ "$CURRENT_RSS" -ge "$TARGET_RSS_MB" ]; then
        echo "已达到目标RSS!"
        break
    fi
    
    # 计算还需要多少key
    REMAINING_MB=$((TARGET_RSS_MB - CURRENT_RSS))
    if [ "$REMAINING_MB" -lt 100 ]; then
        echo "接近目标，微调中..."
    fi
    
    # 执行填充
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set -n "$BATCH_SIZE" -r $((BATCH_SIZE * BATCH_NUM * 2)) \
        -d "$VALUE_SIZE" -c 50 -P 100 -q 2>&1 | tail -3
    
    TOTAL_KEYS=$((TOTAL_KEYS + BATCH_SIZE))
    record_state "batch_$BATCH_NUM"
    
    BATCH_NUM=$((BATCH_NUM + 1))
    
    # 安全检查：最多10批次
    if [ "$BATCH_NUM" -gt 10 ]; then
        echo "警告：已达到最大批次限制"
        break
    fi
    
    sleep 1
done

# 最终状态
echo ""
echo "=========================================="
echo "填充完成 - 最终状态"
echo "=========================================="
record_state "final_fill"

# 混合工作负载测试
echo ""
echo "[混合工作负载测试]"

for i in 1 2 3; do
    echo ""
    echo "混合测试轮次 $i..."
    
    # SET
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t set -n 100000 -r "$TOTAL_KEYS" \
        -d "$VALUE_SIZE" -c 20 -P 50 -q 2>&1 | tail -2
    record_state "mixed_${i}_set"
    
    # GET
    "$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
        -t get -n 100000 -r "$TOTAL_KEYS" \
        -c 20 -P 50 -q 2>&1 | tail -2
    record_state "mixed_${i}_get"
done

# 生成报告
echo ""
echo "=========================================="
echo "测试完成"
echo "=========================================="

{
    echo "8G内存压力测试报告 (激进版)"
    echo "==========================="
    echo "测试时间: $(date)"
    echo ""
    echo "测试配置:"
    echo "  目标RSS: ${TARGET_RSS_GB}GB"
    echo "  Value大小: ${VALUE_SIZE}B"
    echo "  总key数: $TOTAL_KEYS"
    echo ""
    echo "内存使用曲线:"
    cat "$REPORT_DIR/timeline.csv"
    echo ""
    echo "最终Redis内存信息:"
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null
} > "$REPORT_DIR/SUMMARY.txt"

cat "$REPORT_DIR/SUMMARY.txt"

# 停止Redis
"$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true

echo ""
echo "报告已保存到: $REPORT_DIR"
