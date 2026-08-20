#!/bin/bash
# ==============================================================================
# relative_perf_bench.sh -- 相对性能基准：把真实放置轨迹喂进 NUMAflow 的代价模型
#
# 背景（详见 docs/new/09-architecture-decisions.md 的 ADR-11/ADR-12）：
#   - tests/ycsb/run_algorithm_comparison.sh 在只有 1 个 NUMA 节点的开发机上
#     只能测到策略自身的记账开销，测不到任何迁移收益（这台机器上 migrations
#     恒为 0，配对 A/B 测试已经把表面上 ~11% 的 CAAT "劣势"还原成噪声主导下
#     的 ~3.4% 真实开销，且这个数字结构性地不可能显示出收益）。
#   - tests/vm/placement_quality.sh 在真实双节点 QEMU guest 里第一次测到了
#     真实的放置决策，但 QEMU 的两个 -numa node 背后是同一块宿主机 DRAM，
#     没有真实的跨节点延迟差——放置质量测得到，性能收益测不到。
#   - 本脚本把第二步的产物（真实放置决策：谁被放在哪、被访问了多少次）喂进
#     NUMAflow 已有的、可用真实 CXLMemSim 数字标定的纯函数代价模型
#     （`numaflow replay`，见 numaflow/src/nf_cli.c），算出"如果这份真实决策
#     发生在标定参数代表的硬件上，会是多少 ns"——这是建模投影，不是实测，
#     全程标注清楚，不与任何吞吐/延迟实测数字混在同一张图/同一份摘要里。
#
# 前置条件：先用
#   tests/vm/boot_numa_vm.sh --keep --timeout 600
# 起一个双节点 guest 并保持运行，再在开发机上跑本脚本。
#
# 用法: ./relative_perf_bench.sh [--only s1,s2,...] [--out-dir DIR]
# ==============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
NUMAFLOW_BIN="$PROJECT_ROOT/numaflow/build/numaflow"
CACHE_DIR="$SCRIPT_DIR/.cache"
SSH_KEY="$CACHE_DIR/vm_test_key"
SSH_PORT=10222
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o BatchMode=yes -i "$SSH_KEY" -p "$SSH_PORT")
SCP_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P "$SSH_PORT" -i "$SSH_KEY")

STRATEGIES="noop,composite_lru,tinylfu,caat"
OUT_DIR="$PROJECT_ROOT/results"
# 与 run_full_validation.sh 里 NUMAflow eval 标定用的是同一组数字：一次真实
# CXLMemSim device-link 检查测得的 ~100-150ns 延迟 / 25GB/s 带宽。
CXL_LATENCY_NS=125
CXL_BANDWIDTH_MBPS=25000

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; BLUE='\033[0;34m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }

usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --only NAMES     只采集指定策略，逗号分隔 (默认: $STRATEGIES)
  --out-dir DIR    结果目录 (默认: $OUT_DIR)
  --help           显示此帮助

前置条件: tests/vm/boot_numa_vm.sh --keep --timeout 600 已经起了一个双节点 guest。
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --only) STRATEGIES="$2"; shift 2 ;;
        --out-dir) OUT_DIR="$2"; shift 2 ;;
        --help|-h) usage ;;
        *) log_err "未知参数: $1"; usage ;;
    esac
done

[[ -x "$NUMAFLOW_BIN" ]] || { log_err "numaflow 未编译: cd numaflow && make"; exit 1; }
[[ -f "$SSH_KEY" ]] || { log_err "没找到 $SSH_KEY -- 先用 tests/vm/boot_numa_vm.sh --keep 起一个 VM"; exit 1; }
ssh "${SSH_OPTS[@]}" numatest@127.0.0.1 'echo ssh-ready' >/dev/null 2>&1 || {
    log_err "SSH 到 127.0.0.1:$SSH_PORT 失败 -- VM 是不是没在跑？先用 tests/vm/boot_numa_vm.sh --keep 起一个"
    exit 1
}

mkdir -p "$OUT_DIR"
TRACE_DIR="$(mktemp -d)"
trap 'rm -rf "$TRACE_DIR"' EXIT

log "把二进制和采集脚本部署到 guest..."
scp -q "${SCP_OPTS[@]}" \
    "$PROJECT_ROOT/src/redis-server" "$PROJECT_ROOT/src/redis-cli" "$PROJECT_ROOT/src/redis-benchmark" \
    "$SCRIPT_DIR/collect_relative_trace.sh" \
    numatest@127.0.0.1:/home/numatest/ || { log_err "scp 部署失败"; exit 1; }
ssh "${SSH_OPTS[@]}" numatest@127.0.0.1 \
    'chmod +x /home/numatest/collect_relative_trace.sh /home/numatest/redis-server /home/numatest/redis-cli /home/numatest/redis-benchmark'

IFS=',' read -ra STRAT_ARR <<< "$STRATEGIES"
declare -A TRACE_PATH=()
FAILED=0
for s in "${STRAT_ARR[@]}"; do
    log "guest 内采集策略: $s"
    if timeout 180 ssh "${SSH_OPTS[@]}" numatest@127.0.0.1 \
        "/home/numatest/collect_relative_trace.sh $s 7799 /home/numatest/trace_$s.json"; then
        if scp -q "${SCP_OPTS[@]}" \
            "numatest@127.0.0.1:/home/numatest/trace_$s.json" "$TRACE_DIR/trace_$s.json"; then
            TRACE_PATH[$s]="$TRACE_DIR/trace_$s.json"
            log_ok "$s: 轨迹采集完成"
        else
            log_err "$s: 轨迹文件取回失败"; FAILED=1
        fi
    else
        log_err "$s: guest 内采集失败"; FAILED=1
    fi
done

if [[ ${#TRACE_PATH[@]} -eq 0 ]]; then
    log_err "没有任何策略成功采集，无法生成建模结果"
    exit 1
fi

TRACE_FLAGS=()
for s in "${!TRACE_PATH[@]}"; do
    TRACE_FLAGS+=(--trace "$s=${TRACE_PATH[$s]}")
done

log "喂进标定过的代价模型 (tier-1 覆盖为 CXLMemSim 实测数字: ${CXL_LATENCY_NS}ns / ${CXL_BANDWIDTH_MBPS}MB/s)..."
if "$NUMAFLOW_BIN" replay "${TRACE_FLAGS[@]}" --nodes 2 \
    --cxl-latency-ns "$CXL_LATENCY_NS" --cxl-bandwidth-mbps "$CXL_BANDWIDTH_MBPS" \
    --out "$OUT_DIR/bench_relative_perf_cxlcal.json"; then
    log_ok "写入 $OUT_DIR/bench_relative_perf_cxlcal.json"
else
    log_err "numaflow replay (标定版) 失败"; FAILED=1
fi

log "喂进 numa_shim.c 的合成 tier-1 默认值（未标定，作对照）..."
if "$NUMAFLOW_BIN" replay "${TRACE_FLAGS[@]}" --nodes 2 \
    --out "$OUT_DIR/bench_relative_perf.json"; then
    log_ok "写入 $OUT_DIR/bench_relative_perf.json"
else
    log_err "numaflow replay (默认版) 失败"; FAILED=1
fi

echo
echo -e "${YELLOW}重要提示 / IMPORTANT:${NC}"
echo "以上两份 bench_relative_perf*.json 里的 net_cost 是"
echo "\"真实放置决策 x 标定/合成硬件参数\" 算出来的建模投影，不是在真实硬件上"
echo "实测到的延迟——标定数字本身来自 CXLMemSim 的简化设备模型，不是硅片实测。"
echo "net_cost above is a MODELED projection (real placement decisions x"
echo "calibrated/synthetic hardware cost-model parameters), NOT a measured"
echo "latency. Do not present it next to real throughput/latency numbers"
echo "without this caveat."

if [[ ${#TRACE_PATH[@]} -lt 4 ]]; then
    log_warn "只采集到 ${#TRACE_PATH[@]}/4 个策略——numaflow/eval/report.py 和"
    log_warn "tests/report/generate_full_report.py 要求 noop/composite_lru/tinylfu/caat"
    log_warn "四个都在场才会画出这个 workload 的图，其它已有 workload 的图不受影响。"
fi

log "跑 'cd numaflow && python3 eval/report.py' 或 run_full_validation.sh 的聚合报告即可看到这张新面板。"

[[ "$FAILED" == "1" ]] && exit 1
exit 0
