#!/bin/bash
# ==============================================================================
# collect_relative_trace.sh -- 在真实双 NUMA 节点 guest 内采集一份"真实放置
# 轨迹"：每个 key 真正的起始节点（fill 阶段落点，不是假设的 0）、最终节点、
# 被访问了多少次。
#
# 这份轨迹是 relative_perf_bench.sh / `numaflow replay` 的输入 -- 它们把这份
# 真实决策喂进 NUMAflow 的纯函数代价模型，投影出标定硬件参数下的建模 ns 级
# 相对性能。本脚本本身只负责采集决策，不产生任何性能数字（QEMU 的两个
# -numa node 背后是同一块宿主机 DRAM，这里量出来的时间没有意义）。
#
# 为什么要单独采集"起始节点"而不是假设都从 node0 开始：这台 guest 上默认的
# 分配策略是 local_first——但 "local" 是相对于*当前执行分配调用的 CPU*而言的，
# 而 Redis 的单线程主线程会被 guest 内核在 4 个 vCPU（分属两个节点）之间调度，
# 所以哪怕一个 key 从未被迁移过，它落在哪个节点也不保证是 0。用假设的 0 当
# origin 会把"本来就分配在别处"和"被迁移过去"混为一谈，把 noop（这条路径的
# DAG 是空的，不可能迁移任何东西）也算出一堆"migrations"——这正是本脚本第一版
# 的 bug，被 noop 组的结果直接测出来。
#
# 起始快照本身需要遍历全部 key（对这台机器要几十秒），如果
# 后台 cron 在这几十秒里跑起来，会在我们还没采完起始快照之前就动手迁移，
# 起始快照就不干净了。启动时把 interval 设成配置允许的最大值
# （numa-flow-interval-sec 的上限是 3600，见 src/config.c），让它在整个测试
# 窗口内基本不会自己触发，改为在采完起始快照、灌完访问负载之后手动调用几次
# `NUMA FLOW RUN default`，让迁移的时机完全在我们控制之内。
#
# 用法（guest 内）: ./collect_relative_trace.sh <strategy> [port] [out_file]
# ==============================================================================
set -uo pipefail

STRATEGY="${1:-caat}"
PORT="${2:-7799}"
OUT="${3:-/home/numatest/trace_${STRATEGY}.json}"
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CLI="$DIR/redis-cli -p $PORT"

TOTAL_KEYS=2000
HOT_KEYS=100     # 最热 5%
WARM_KEYS=400    # 次热 15%（与 HOT_KEYS 窗口重叠，重叠部分自然拿到双重访问）
VALUE_SIZE=4096
HOT_OPS=20000
WARM_OPS=12000
MANUAL_RUNS=3    # 手动触发几次 `NUMA FLOW RUN default` 模拟多个决策窗口
SETTLE=3

ORIGIN_FILE="$(mktemp)"
FINAL_FILE="$(mktemp)"
trap 'rm -f "$ORIGIN_FILE" "$FINAL_FILE"' EXIT

pkill -9 -x redis-server 2>/dev/null; sleep 1
nohup "$DIR/redis-server" --port "$PORT" --daemonize no \
    --numa-flow-default-strategy "$STRATEGY" --numa-flow-interval-sec 3600 \
    --save "" --logfile "$DIR/rpb_$STRATEGY.log" >/dev/null 2>&1 &
sleep 4
$CLI ping >/dev/null 2>&1 || { echo "$STRATEGY: redis 起不来" >&2; exit 1; }

# 确定性建 key（避免 redis-benchmark -r 的随机覆盖不全问题）。
$CLI eval "for i=0,$((TOTAL_KEYS-1)) do redis.call('SET', string.format('key:%012d',i), string.rep('x',$VALUE_SIZE)) end return redis.call('DBSIZE')" 0 >/dev/null

# ---- 起始快照：只在 fill 之后、任何迁移决策跑之前采集 current_node ----
{
  for i in $(seq 0 $((TOTAL_KEYS-1))); do
    echo "echo MARK$i"
    echo "numa migrate info $(printf 'key:%012d' "$i")"
  done
} | $CLI 2>/dev/null | awk '
  /^MARK[0-9]+$/ { idx = substr($0,5)+0; next }
  /^current_node$/ { getline v; print idx, v+0; next }
' > "$ORIGIN_FILE"

# 这台机器上的 redis-benchmark 没有 --distribution=zipf，用两层重叠的随机窗口
# 近似冷热分层：窄窗口+多次数制造最热 5%，宽窗口+少次数叠加出次热的 15%，
# 剩余 80% 在这两轮里保持冷（fill 阶段的 SET 已经让它们有过一次访问）。
"$DIR/redis-benchmark" -p "$PORT" -q -t get -r "$HOT_KEYS" -n "$HOT_OPS" -P 10 >/dev/null 2>&1
"$DIR/redis-benchmark" -p "$PORT" -q -t get -r "$WARM_KEYS" -n "$WARM_OPS" -P 10 >/dev/null 2>&1

# 手动触发迁移决策（noop 策略下 DAG 是空图，这几次调用什么都不做）。
for _ in $(seq 1 "$MANUAL_RUNS"); do
    $CLI numa flow run default >/dev/null 2>&1
    sleep 1
done
sleep "$SETTLE"

mig_stats=$($CLI numa migrate stats | paste - - | awk '/^successful_migrations/{print $2}')

# ---- 最终快照：落点 + 访问次数 ----
{
  for i in $(seq 0 $((TOTAL_KEYS-1))); do
    echo "echo MARK$i"
    echo "numa migrate info $(printf 'key:%012d' "$i")"
  done
} | $CLI 2>/dev/null | awk '
  /^MARK[0-9]+$/ { idx = substr($0,5)+0; next }
  /^current_node$/ { getline v; node=v+0; next }
  /^access_count$/ { getline v; print idx, node, v+0; next }
' > "$FINAL_FILE"

# ---- 合并成轨迹 JSON：按数值下标（不是关联数组遍历）输出，保证即使某个 key
# 的某次查询意外失败也不会在轨迹里留洞；final 缺失时退化为"没动"（origin）。
awk -v total="$TOTAL_KEYS" -v size="$VALUE_SIZE" '
  NR == FNR { orig[$1] = $2; next }
  { fin[$1] = $2; acc[$1] = $3 }
  END {
    printf "["
    for (i = 0; i < total+0; i++) {
      oi = (i in orig) ? orig[i] : 0
      fi = (i in fin)  ? fin[i]  : oi
      ai = (i in acc)  ? acc[i]  : 0
      printf "%s{\"key\":\"key:%012d\",\"size\":%d,\"access_count\":%d,\"origin_node\":%d,\"final_node\":%d}", (i>0?",":""), i, size, ai, oi, fi
    }
    printf "]\n"
  }' "$ORIGIN_FILE" "$FINAL_FILE" > "$OUT"

echo "$STRATEGY: 轨迹已写入 $OUT (successful_migrations=${mig_stats:-0})"

$CLI shutdown nosave 2>/dev/null || true
sleep 1
