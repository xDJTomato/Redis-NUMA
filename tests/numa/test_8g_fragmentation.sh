#!/bin/bash
#===============================================================================
# 8G内存碎片压力测试脚本
# 目标：填充8GB内存，评估NUMA内存池的碎片率
#===============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_ROOT="$SCRIPT_DIR"

REDIS_SERVER="$PROJECT_ROOT/src/redis-server"
REDIS_CLI="$PROJECT_ROOT/src/redis-cli"
REDIS_BENCH="$PROJECT_ROOT/src/redis-benchmark"

REDIS_PORT=6379
REDIS_HOST="127.0.0.1"

# 测试参数 - 目标8GB
TARGET_MEMORY_GB=8
TARGET_MEMORY_MB=$((TARGET_MEMORY_GB * 1024))

# 计算需要的key数量 (每个key约占用内存 = value_size + 开销)
# 假设平均每个key占用 ~300B (256B value + 44B 开销)
KEYS_FOR_8GB=$((TARGET_MEMORY_MB * 1024 * 1024 / 300))

# 实际测试参数
VALUE_SIZE=256
KEY_RANGE=$((KEYS_FOR_8GB / 2))  # 使用50%的range，允许重复和过期

# 报告目录
REPORT_DIR="$PROJECT_ROOT/tests/reports/fragmentation_8g_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$REPORT_DIR"

echo "=========================================="
echo "8G内存碎片压力测试"
echo "=========================================="
echo "目标内存: ${TARGET_MEMORY_GB}GB"
echo "估算key数: $KEYS_FOR_8GB"
echo "实际range: $KEY_RANGE"
echo "Value大小: ${VALUE_SIZE}B"
echo "报告目录: $REPORT_DIR"
echo ""

# 清理并启动Redis
echo "[1/5] 启动Redis服务器..."
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
echo "Redis启动成功"

# 获取内存统计函数
get_memory_info() {
    local info
    info=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null)
    
    local used_memory=$(echo "$info" | grep "^used_memory:" | cut -d: -f2 | tr -d '\r')
    local used_memory_rss=$(echo "$info" | grep "^used_memory_rss:" | cut -d: -f2 | tr -d '\r')
    local mem_fragmentation_ratio=$(echo "$info" | grep "^mem_fragmentation_ratio:" | cut -d: -f2 | tr -d '\r')
    local used_memory_human=$(echo "$info" | grep "^used_memory_human:" | cut -d: -f2 | tr -d '\r')
    local used_memory_rss_human=$(echo "$info" | grep "^used_memory_rss_human:" | cut -d: -f2 | tr -d '\r')
    
    echo "USED:$used_memory RSS:$used_memory_rss FRAG:$mem_fragmentation_ratio USED_H:$used_memory_human RSS_H:$used_memory_rss_human"
}

# 记录内存状态
record_memory() {
    local phase="$1"
    local info
    info=$(get_memory_info)
    
    local used=$(echo "$info" | grep -oP 'USED:\K[0-9]+')
    local rss=$(echo "$info" | grep -oP 'RSS:\K[0-9]+')
    local frag=$(echo "$info" | grep -oP 'FRAG:\K[0-9.]+')
    local used_h=$(echo "$info" | grep -oP 'USED_H:\K[^ ]+')
    local rss_h=$(echo "$info" | grep -oP 'RSS_H:\K[^ ]+')
    
    local used_mb=$((used / 1024 / 1024))
    local rss_mb=$((rss / 1024 / 1024))
    
    echo "$(date '+%H:%M:%S'),$phase,$used_mb,$rss_mb,$frag,$used_h,$rss_h" >> "$REPORT_DIR/memory_timeline.csv"
    
    echo "[$phase] 有效数据:${used_h} RSS:${rss_h} 碎片率:${frag}"
}

# 初始化CSV
echo "timestamp,phase,used_mb,rss_mb,fragmentation,used_human,rss_human" > "$REPORT_DIR/memory_timeline.csv"

# 初始状态
echo ""
echo "[2/5] 记录初始状态..."
record_memory "initial"

# 阶段1: 快速填充到4GB (50%)
echo ""
echo "[3/5] 阶段1: 填充到4GB (50%目标)..."
"$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
    -t set -n $((KEY_RANGE / 2)) -r $((KEY_RANGE / 2)) \
    -d "$VALUE_SIZE" -c 50 -P 100 -q 2>&1 | tail -5

sleep 2
record_memory "fill_50pct"

# 阶段2: 继续填充到6GB (75%)
echo ""
echo "[4/5] 阶段2: 填充到6GB (75%目标)..."
"$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
    -t set -n $((KEY_RANGE / 4)) -r $((KEY_RANGE / 4 * 3)) \
    -d "$VALUE_SIZE" -c 50 -P 100 -q 2>&1 | tail -5

sleep 2
record_memory "fill_75pct"

# 阶段3: 填充到8GB (100%目标)
echo ""
echo "[5/5] 阶段3: 填充到8GB (100%目标)..."
"$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
    -t set -n $((KEY_RANGE / 4)) -r "$KEY_RANGE" \
    -d "$VALUE_SIZE" -c 50 -P 100 -q 2>&1 | tail -5

sleep 2
record_memory "fill_100pct"

# 混合工作负载测试
echo ""
echo "[6/5] 混合工作负载测试 (SET/GET/DEL)..."

# SET操作
echo "执行SET操作..."
"$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
    -t set -n $((KEY_RANGE / 10)) -r "$KEY_RANGE" \
    -d "$VALUE_SIZE" -c 30 -P 50 -q 2>&1 | tail -3
record_memory "mixed_set"

# GET操作
echo "执行GET操作..."
"$REDIS_BENCH" -h "$REDIS_HOST" -p "$REDIS_PORT" \
    -t get -n $((KEY_RANGE / 10)) -r "$KEY_RANGE" \
    -c 30 -P 50 -q 2>&1 | tail -3
record_memory "mixed_get"

# 最终状态
echo ""
echo "=========================================="
echo "测试完成 - 最终内存状态"
echo "=========================================="

# 获取最终信息
final_info=$(get_memory_info)
echo "详细信息: $final_info"

# 生成摘要报告
{
    echo "8G内存碎片压力测试报告"
    echo "======================"
    echo "测试时间: $(date)"
    echo ""
    echo "测试配置:"
    echo "  目标内存: ${TARGET_MEMORY_GB}GB"
    echo "  Value大小: ${VALUE_SIZE}B"
    echo "  Key范围: $KEY_RANGE"
    echo ""
    echo "内存使用曲线:"
    cat "$REPORT_DIR/memory_timeline.csv"
    echo ""
    echo "Redis INFO memory:"
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null | grep -E "used_memory|mem_fragmentation"
} > "$REPORT_DIR/SUMMARY.txt"

cat "$REPORT_DIR/SUMMARY.txt"

# 停止Redis
"$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true

echo ""
echo "报告已保存到: $REPORT_DIR"
