#!/bin/bash
# numa_diagnose.sh — NUMA 数据放置快速诊断
# 用法: bash numa_diagnose.sh [redis-port] [redis-host]
# 需要在双节点服务器上、benchmark 运行中或刚结束时执行

PORT=${1:-6379}
HOST=${2:-127.0.0.1}
CLI="redis-cli -h $HOST -p $PORT"

echo "=========================================="
echo " NUMA 数据放置诊断报告"
echo " $(date '+%Y-%m-%d %H:%M:%S')"
echo "=========================================="

# ── 0. 基本环境 ──────────────────────────────
echo ""
echo "── 0. NUMA 拓扑 ──"
numactl --hardware 2>/dev/null | grep -E 'available|node [0-9]+ cpus|node [0-9]+ size|distances'
echo ""
echo "Redis PID: $(pgrep -f "redis-server.*:$PORT" | head -1)"
PID=$(pgrep -f "redis-server.*:$PORT" | head -1)
if [[ -z "$PID" ]]; then
    echo "ERROR: 找不到 redis-server 进程 (port=$PORT)"
    exit 1
fi
echo "Redis CPU affinity: $(taskset -p $PID 2>/dev/null)"
echo "Redis 当前 CPU: $(cat /proc/$PID/stat | awk '{print $39}' 2>/dev/null)"
CURRENT_CPU=$(cat /proc/$PID/stat | awk '{print $39}' 2>/dev/null)
if [[ -n "$CURRENT_CPU" ]]; then
    echo "该 CPU 所在 NUMA 节点: $(numactl --hardware 2>/dev/null | grep "node.*cpus.*\b${CURRENT_CPU}\b" | awk '{print $2}')"
fi

# ── 1. dbOverwrite 修复触发统计 ──────────────
echo ""
echo "── 1. dbOverwrite 修复触发情况 ──"
STATS=$($CLI --raw NUMA MIGRATE STATS 2>/dev/null)
if [[ -z "$STATS" ]]; then
    echo "ERROR: 无法连接 Redis 或 NUMA MIGRATE STATS 不可用"
    exit 1
fi

checks=$(awk '/^dboverwrite_checks$/ {found=1; next} found {print; exit}' <<< "$STATS")
reallocs=$(awk '/^dboverwrite_reallocs$/ {found=1; next} found {print; exit}' <<< "$STATS")
acc_local=$(awk '/^accesses_local$/ {found=1; next} found {print; exit}' <<< "$STATS")
acc_remote=$(awk '/^accesses_remote$/ {found=1; next} found {print; exit}' <<< "$STATS")
migrations=$(awk '/^successful_migrations$/ {found=1; next} found {print; exit}' <<< "$STATS")

echo "  dboverwrite_checks:   ${checks:-N/A}"
echo "  dboverwrite_reallocs: ${reallocs:-N/A}"
echo "  accesses_local:       ${acc_local:-N/A}"
echo "  accesses_remote:      ${acc_remote:-N/A}"
echo "  successful_migrations:${migrations:-N/A}"

if [[ "${checks:-0}" -gt 0 && "${reallocs:-0}" -eq 0 ]]; then
    echo "  ⚠  条件进入 ${checks} 次但 0 次重分配 — old_node==0 && new_node!=0 从未为真"
elif [[ "${reallocs:-0}" -gt 0 ]]; then
    pct=$((reallocs * 100 / checks))
    echo "  ✓  重分配触发率: ${reallocs}/${checks} = ${pct}%"
else
    echo "  ⚠  计数器均为 0 — 可能 HAVE_NUMA 未编译或未执行 UPDATE"
fi

total_acc=$(( ${acc_local:-0} + ${acc_remote:-0} ))
if [[ "$total_acc" -gt 0 ]]; then
    local_pct=$((acc_local * 100 / total_acc))
    echo "  累计 local%: ${local_pct}%  (${acc_local} / ${total_acc})"
fi

# ── 2. 采样热点键的 NUMA 位置 ──────────────
echo ""
echo "── 2. 热点键 NUMA 位置采样 ──"
echo "  (随机采样 20 个键，检查 prefix 中记录的 node_id)"

DBSIZE=$($CLI DBSIZE 2>/dev/null | grep -o '[0-9]*')
echo "  当前 DBSIZE: ${DBSIZE:-unknown}"

node0_count=0
node1_count=0
other_count=0
sample_count=0

for i in $(seq 1 20); do
    key=$($CLI --raw RANDOMKEY 2>/dev/null)
    [[ -z "$key" ]] && continue

    info=$($CLI --raw NUMA MIGRATE INFO "$key" 2>/dev/null)
    [[ -z "$info" ]] && continue

    node=$(awk '/^current_node$/ {found=1; next} found {print; exit}' <<< "$info")
    hotness=$(awk '/^hotness_level$/ {found=1; next} found {print; exit}' <<< "$info")
    encoding=$(awk '/^type$/ {found=1; next} found {print; exit}' <<< "$info")

    sample_count=$((sample_count + 1))

    case "$node" in
        0) node0_count=$((node0_count + 1)) ;;
        1) node1_count=$((node1_count + 1)) ;;
        *) other_count=$((other_count + 1)) ;;
    esac

    printf "    %-40s  node=%s  hotness=%s  type=%s\n" "$key" "${node:--1}" "${hotness:-?}" "${encoding:-?}"
done

if [[ "$sample_count" -gt 0 ]]; then
    echo ""
    echo "  采样 ${sample_count} 个键: Node0=${node0_count}($(( node0_count*100/sample_count ))%)  Node1=${node1_count}($(( node1_count*100/sample_count ))%)  其他=${other_count}"
fi

# ── 3. /proc/numa_maps 物理页分布 ──────────────
echo ""
echo "── 3. 进程物理页 NUMA 分布 (numa_maps) ──"

NUMA_MAPS="/proc/$PID/numa_maps"
if [[ ! -r "$NUMA_MAPS" ]]; then
    echo "  需要 root 权限读取 $NUMA_MAPS"
    echo "  请用: sudo bash $0 $PORT $HOST"
else
    total_n0=0
    total_n1=0
    total_pages=0

    while IFS= read -r line; do
        n0=$(echo "$line" | grep -oP 'N0=\K[0-9]+' || echo 0)
        n1=$(echo "$line" | grep -oP 'N1=\K[0-9]+' || echo 0)
        total_n0=$((total_n0 + n0))
        total_n1=$((total_n1 + n1))
    done < "$NUMA_MAPS"

    total_pages=$((total_n0 + total_n1))
    if [[ "$total_pages" -gt 0 ]]; then
        n0_pct=$((total_n0 * 100 / total_pages))
        n1_pct=$((total_n1 * 100 / total_pages))
        echo "  Node0 物理页: ${total_n0} (${n0_pct}%) ≈ $((total_n0 * 4 / 1024)) MB"
        echo "  Node1 物理页: ${total_n1} (${n1_pct}%) ≈ $((total_n1 * 4 / 1024)) MB"
        echo "  总计: ${total_pages} 页 ≈ $((total_pages * 4 / 1024)) MB"
    else
        echo "  无法解析 numa_maps 数据"
    fi

    echo ""
    echo "  按映射类型的大块分布 (top 10):"
    awk '{
        n0=0; n1=0;
        if (match($0, /N0=([0-9]+)/, a)) n0=a[1];
        if (match($0, /N1=([0-9]+)/, a)) n1=a[1];
        total=n0+n1;
        if (total > 100) printf "    pages=%6d  N0=%6d  N1=%6d  N0%%=%3d%%  %s\n", total, n0, n1, (total>0 ? n0*100/total : 0), $1
    }' "$NUMA_MAPS" | sort -t= -k1 -rn | head -10
fi

# ── 4. 验证 prefix node_id vs 实际物理位置 ──────
echo ""
echo "── 4. prefix node_id 与物理页对账 ──"

if [[ "$sample_count" -gt 0 && -r "$NUMA_MAPS" ]]; then
    prefix_n0_pct=$((node0_count * 100 / sample_count))
    echo "  prefix 报告 Node0: ${prefix_n0_pct}%"
    echo "  numa_maps 实际 Node0: ${n0_pct:-?}%"

    if [[ -n "$n0_pct" ]]; then
        diff=$((prefix_n0_pct - n0_pct))
        abs_diff=${diff#-}
        if [[ "$abs_diff" -gt 15 ]]; then
            echo "  ⚠  差异 ${abs_diff}% — prefix node_id 可能与物理放置不一致!"
        else
            echo "  ✓  差异 ${abs_diff}% — 基本一致"
        fi
    fi
else
    echo "  (需要采样数据 + numa_maps 权限才能对账)"
fi

# ── 4b. 主动写入测试: dbOverwrite 修复验证 ─────
echo ""
echo "── 4b. 主动写入测试 (dbOverwrite 验证) ──"

VAL_1800=$(python3 -c 'print("A"*1800)' 2>/dev/null || printf '%0.sA' $(seq 1 1800))
TEST_PREFIX="__numa_diag_"

# 记录修复前的计数器
pre_checks=$(awk '/^dboverwrite_checks$/ {f=1;next} f{print;exit}' <<< "$STATS")
pre_reallocs=$(awk '/^dboverwrite_reallocs$/ {f=1;next} f{print;exit}' <<< "$STATS")

# 4b-1: 插入 50 个测试键
echo "  [插入] 写入 50 个 1800B 测试键..."
for i in $(seq 1 50); do
    $CLI SET "${TEST_PREFIX}${i}" "$VAL_1800" > /dev/null 2>&1
done

# 检查初始节点分布
init_n0=0; init_n1=0
for i in $(seq 1 50); do
    info=$($CLI --raw NUMA MIGRATE INFO "${TEST_PREFIX}${i}" 2>/dev/null)
    node=$(awk '/^current_node$/ {f=1;next} f{print;exit}' <<< "$info")
    case "$node" in
        0) init_n0=$((init_n0+1)) ;;
        *) init_n1=$((init_n1+1)) ;;
    esac
done
echo "  [初始分布] Node0=${init_n0}  Node1=${init_n1}  (预期 ~69%/31%)"

# 4b-2: 覆写全部键 (触发 dbOverwrite)
VAL_1800B=$(python3 -c 'print("B"*1800)' 2>/dev/null || printf '%0.sB' $(seq 1 1800))
echo "  [覆写] 覆写 50 个键 (触发 dbOverwrite)..."
for i in $(seq 1 50); do
    $CLI SET "${TEST_PREFIX}${i}" "$VAL_1800B" > /dev/null 2>&1
done

# 读取覆写后的计数器
STATS_POST=$($CLI --raw NUMA MIGRATE STATS 2>/dev/null)
post_checks=$(awk '/^dboverwrite_checks$/ {f=1;next} f{print;exit}' <<< "$STATS_POST")
post_reallocs=$(awk '/^dboverwrite_reallocs$/ {f=1;next} f{print;exit}' <<< "$STATS_POST")

delta_checks=$(( ${post_checks:-0} - ${pre_checks:-0} ))
delta_reallocs=$(( ${post_reallocs:-0} - ${pre_reallocs:-0} ))
echo "  [计数器增量] checks=+${delta_checks}  reallocs=+${delta_reallocs}"

# 检查覆写后的节点分布
post_n0=0; post_n1=0
for i in $(seq 1 50); do
    info=$($CLI --raw NUMA MIGRATE INFO "${TEST_PREFIX}${i}" 2>/dev/null)
    node=$(awk '/^current_node$/ {f=1;next} f{print;exit}' <<< "$info")
    case "$node" in
        0) post_n0=$((post_n0+1)) ;;
        *) post_n1=$((post_n1+1)) ;;
    esac
done
echo "  [覆写后分布] Node0=${post_n0}  Node1=${post_n1}"

# 4b-3: 手动迁移几个 CXL 键到 Node 0，再覆写
echo ""
echo "  [迁移+覆写测试] 找 CXL 键 → 迁移到 Node0 → 覆写 → 检查是否留在 Node0"
migrated_keys=()
for i in $(seq 1 50); do
    info=$($CLI --raw NUMA MIGRATE INFO "${TEST_PREFIX}${i}" 2>/dev/null)
    node=$(awk '/^current_node$/ {f=1;next} f{print;exit}' <<< "$info")
    if [[ "$node" != "0" && ${#migrated_keys[@]} -lt 10 ]]; then
        $CLI NUMA MIGRATE KEY "${TEST_PREFIX}${i}" 0 > /dev/null 2>&1
        migrated_keys+=("${TEST_PREFIX}${i}")
    fi
done
echo "  迁移了 ${#migrated_keys[@]} 个键到 Node 0"

# 确认迁移成功
if [[ ${#migrated_keys[@]} -gt 0 ]]; then
    mig_ok=0
    for key in "${migrated_keys[@]}"; do
        info=$($CLI --raw NUMA MIGRATE INFO "$key" 2>/dev/null)
        node=$(awk '/^current_node$/ {f=1;next} f{print;exit}' <<< "$info")
        [[ "$node" == "0" ]] && mig_ok=$((mig_ok+1))
    done
    echo "  迁移后确认 Node0: ${mig_ok}/${#migrated_keys[@]}"

    # 记录覆写前计数器
    STATS_PRE2=$($CLI --raw NUMA MIGRATE STATS 2>/dev/null)
    pre2_reallocs=$(awk '/^dboverwrite_reallocs$/ {f=1;next} f{print;exit}' <<< "$STATS_PRE2")

    # 覆写这些刚迁移的键
    VAL_1800C=$(python3 -c 'print("C"*1800)' 2>/dev/null || printf '%0.sC' $(seq 1 1800))
    for key in "${migrated_keys[@]}"; do
        $CLI SET "$key" "$VAL_1800C" > /dev/null 2>&1
    done

    STATS_POST2=$($CLI --raw NUMA MIGRATE STATS 2>/dev/null)
    post2_reallocs=$(awk '/^dboverwrite_reallocs$/ {f=1;next} f{print;exit}' <<< "$STATS_POST2")
    delta2_reallocs=$(( ${post2_reallocs:-0} - ${pre2_reallocs:-0} ))

    # 检查覆写后是否还在 Node 0
    stay_n0=0
    for key in "${migrated_keys[@]}"; do
        info=$($CLI --raw NUMA MIGRATE INFO "$key" 2>/dev/null)
        node=$(awk '/^current_node$/ {f=1;next} f{print;exit}' <<< "$info")
        [[ "$node" == "0" ]] && stay_n0=$((stay_n0+1))
    done
    echo "  覆写后仍在 Node0: ${stay_n0}/${#migrated_keys[@]}"
    echo "  本轮 reallocs 增量: +${delta2_reallocs}"

    if [[ "$stay_n0" -eq "${#migrated_keys[@]}" && "$delta2_reallocs" -gt 0 ]]; then
        echo "  ✓  dbOverwrite 修复生效: 迁移后的键覆写仍留在 Node 0"
    elif [[ "$stay_n0" -lt "${#migrated_keys[@]}" ]]; then
        echo "  ✗  修复未生效: ${migrated_keys[*]} 中有键回流到 CXL"
    else
        echo "  ?  reallocs 未增加但键仍在 Node0 — 新值可能恰好分配在 Node0"
    fi
else
    echo "  (所有测试键已在 Node0, 无法测试迁移+覆写场景)"
fi

# 清理测试键
echo ""
echo "  [清理] 删除测试键..."
for i in $(seq 1 50); do
    $CLI DEL "${TEST_PREFIX}${i}" > /dev/null 2>&1
done
echo "  已清理 50 个测试键"

# ── 5. weighted_interleave 权重检查 ──────────
echo ""
echo "── 5. 当前分配策略权重 ──"
CONFIG=$($CLI --raw NUMA CONFIG STATUS 2>/dev/null)
if [[ -n "$CONFIG" ]]; then
    echo "$CONFIG" | head -30
else
    CONFIG=$($CLI --raw NUMA CONFIG GET strategy 2>/dev/null)
    if [[ -n "$CONFIG" ]]; then
        echo "  策略: $CONFIG"
    else
        echo "  无法获取策略信息"
    fi
fi

# ── 6. 实时 1 秒采样对比 ──────────────────────
echo ""
echo "── 6. 实时 1 秒 local/remote 增量 ──"

STATS1=$($CLI --raw NUMA MIGRATE STATS 2>/dev/null)
l1=$(awk '/^accesses_local$/ {f=1; next} f {print; exit}' <<< "$STATS1")
r1=$(awk '/^accesses_remote$/ {f=1; next} f {print; exit}' <<< "$STATS1")
c1=$(awk '/^dboverwrite_checks$/ {f=1; next} f {print; exit}' <<< "$STATS1")
a1=$(awk '/^dboverwrite_reallocs$/ {f=1; next} f {print; exit}' <<< "$STATS1")

sleep 1

STATS2=$($CLI --raw NUMA MIGRATE STATS 2>/dev/null)
l2=$(awk '/^accesses_local$/ {f=1; next} f {print; exit}' <<< "$STATS2")
r2=$(awk '/^accesses_remote$/ {f=1; next} f {print; exit}' <<< "$STATS2")
c2=$(awk '/^dboverwrite_checks$/ {f=1; next} f {print; exit}' <<< "$STATS2")
a2=$(awk '/^dboverwrite_reallocs$/ {f=1; next} f {print; exit}' <<< "$STATS2")

dl=$(( ${l2:-0} - ${l1:-0} ))
dr=$(( ${r2:-0} - ${r1:-0} ))
dc=$(( ${c2:-0} - ${c1:-0} ))
da=$(( ${a2:-0} - ${a1:-0} ))
dt=$((dl + dr))

echo "  accesses_local/sec:   $dl"
echo "  accesses_remote/sec:  $dr"
if [[ "$dt" -gt 0 ]]; then
    echo "  local%/sec:           $((dl * 100 / dt))%"
fi
echo "  dboverwrite_checks/sec:   $dc"
echo "  dboverwrite_reallocs/sec: $da"

if [[ "$dt" -eq 0 ]]; then
    echo "  (无流量 — benchmark 可能已结束)"
fi

echo ""
echo "=========================================="
echo " 诊断完成"
echo "=========================================="
