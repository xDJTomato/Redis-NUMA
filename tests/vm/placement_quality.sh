#!/bin/bash
# ==============================================================================
# placement_quality.sh -- 在真实双 NUMA 节点环境里测"放置质量"
#
# 背景：开发主机只有 1 个 NUMA 节点，migrations 恒为 0，整条迁移执行路径至今
# 零覆盖。"迁移收益 = 放置质量 x 节点延迟差"，后者是硬件参数、本环境测不了
# （QEMU 两个 -numa node 都是同一块宿主机 DRAM），但前者可以真实测量、跨策略
# 可比，且不依赖任何延迟建模。
#
# 指标：
#   hot_local_ratio    热 key 驻留在 node0 的比例   越高越好（热数据该在快层）
#   cold_off_ratio     冷 key 驻留在 node1 的比例   越高越好（冷数据该被挪走）
#   migrations         实际成功迁移次数              成本代理
#
# 用法（guest 内）: ./placement_quality.sh <strategy> [port]
# ==============================================================================
set -uo pipefail

STRATEGY="${1:-caat}"
PORT="${2:-7799}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="$DIR/redis-cli -p $PORT"

TOTAL_KEYS=400
HOT_KEYS=40
VALUE_SIZE=4096
HOT_OPS=8000
SETTLE=8

pkill -9 -x redis-server 2>/dev/null; sleep 1
nohup "$DIR/redis-server" --port "$PORT" --daemonize no \
    --numa-flow-default-strategy "$STRATEGY" --numa-flow-interval-sec 1 \
    --save "" --logfile "$DIR/pq_$STRATEGY.log" >/dev/null 2>&1 &
sleep 4
$CLI ping >/dev/null 2>&1 || { echo "$STRATEGY: redis 起不来"; exit 1; }

# ---- 确定性建 key（redis-benchmark -r 是随机的，覆盖不全）----
$CLI eval "for i=0,$((TOTAL_KEYS-1)) do redis.call('SET', string.format('key:%012d',i), string.rep('x',$VALUE_SIZE)) end return redis.call('DBSIZE')" 0 >/dev/null

mig_before=$($CLI numa migrate stats | paste - - | awk '/^successful_migrations/{print $2}')

# ---- 制造热点：只打前 HOT_KEYS 个 ----
"$DIR/redis-benchmark" -p "$PORT" -q -t get -r "$HOT_KEYS" -n "$HOT_OPS" -P 10 >/dev/null 2>&1
sleep "$SETTLE"

mig_after=$($CLI numa migrate stats | paste - - | awk '/^successful_migrations/{print $2}')

# ---- 逐 key 采集驻留节点：用 ECHO 做标记，避免位置解析出错 ----
{
  for i in $(seq 0 $((TOTAL_KEYS-1))); do
    echo "echo MARK$i"
    echo "numa migrate info $(printf 'key:%012d' "$i")"
  done
} | $CLI 2>/dev/null | awk -v hot="$HOT_KEYS" '
  /^MARK[0-9]+$/ { idx = substr($0,5)+0; pending=1; seen=0; next }
  pending && /^current_node$/ { getline v; node[idx]=v+0; seen=1; pending=0; next }
  pending && /^ERR/ { pending=0; next }
  END {
    hot_n=0; hot_local=0; cold_n=0; cold_off=0
    # 注意：for(i in arr) 里 i 是字符串，必须 +0 强制数值比较，
    # 否则 "100" < "40" 按字典序成立，热/冷集合会被切错。
    for (i in node) {
      idx = i + 0
      if (idx < hot) { hot_n++;  if (node[i]==0) hot_local++ }
      else           { cold_n++; if (node[i]>0)  cold_off++ }
    }
    printf "%d %d %d %d\n", hot_n, hot_local, cold_n, cold_off
  }' > /tmp/pq_$STRATEGY.stat

read -r hot_n hot_local cold_n cold_off < /tmp/pq_$STRATEGY.stat
migrations=$(( ${mig_after:-0} - ${mig_before:-0} ))

awk -v s="$STRATEGY" -v hn="$hot_n" -v hl="$hot_local" -v cn="$cold_n" -v co="$cold_off" -v m="$migrations" 'BEGIN{
  printf "%-14s hot_local=%3d/%-3d (%5.1f%%)  cold_off=%3d/%-3d (%5.1f%%)  migrations=%d\n",
         s, hl, hn, (hn?100.0*hl/hn:0), co, cn, (cn?100.0*co/cn:0), m
}'

$CLI shutdown nosave 2>/dev/null || true
sleep 1
