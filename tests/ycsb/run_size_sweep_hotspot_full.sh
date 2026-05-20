#!/usr/bin/env bash
# ============================================================================
# Redis-NUMA / vanilla YCSB 固定 32 线程 value-size sweep 热点读测试
# ============================================================================

set -euo pipefail

REMOTE_HOST="192.168.12.204"
REMOTE_USER="dell"
REMOTE_PASS="Dell@123"
SSH_PORT=22
REMOTE_NUMA_ROOT="~/lx/Redis-NUMA-main"
REMOTE_VANILLA_ROOT="~/lx/redis-6.2.21"
REMOTE_YCSB="${REMOTE_NUMA_ROOT}/tests/ycsb"
TEST_PORT=6421

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
LOCAL_RESULTS="${SCRIPT_DIR}/results"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')
THREADS=32
RECORD_COUNT=10000
MAXMEM_NUMA="16gb"
MAXMEM_VANILLA="8gb"
NUMA_STRATEGY="interleaved"
SIZE_LIST="32,64,128,256,512,1024,2048,4096,8192,16384,32768,65536,131072,262144,524288,1048576,1310720"

BLUE='\033[0;34m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; RED='\033[0;31m'; BOLD='\033[1m'; CYAN='\033[0;36m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*" >&2; }
log_step() { echo -e "\n${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; echo -e "${BOLD}${CYAN}  $*${NC}"; echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════${NC}"; }

usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --port PORT          起始 Redis 端口 (默认: 6421)
  --records N          key 数量 (默认: 100000)
  --sizes LIST         value size 列表 (默认: ${SIZE_LIST})
  --host HOST          远程主机 (默认: 192.168.12.204)
  --user USER          SSH 用户 (默认: dell)
  --pass PASS          SSH 密码
  --remote-dir DIR     远程 NUMA 项目根
  --vanilla-dir DIR    远程 vanilla Redis 根
  --help               显示帮助
EOF
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --port) TEST_PORT="$2"; shift 2 ;;
        --records) RECORD_COUNT="$2"; shift 2 ;;
        --sizes) SIZE_LIST="$2"; shift 2 ;;
        --host) REMOTE_HOST="$2"; shift 2 ;;
        --user) REMOTE_USER="$2"; shift 2 ;;
        --pass) REMOTE_PASS="$2"; shift 2 ;;
        --remote-dir) REMOTE_NUMA_ROOT="$2"; REMOTE_YCSB="${REMOTE_NUMA_ROOT}/tests/ycsb"; shift 2 ;;
        --vanilla-dir) REMOTE_VANILLA_ROOT="$2"; shift 2 ;;
        --help|-h) usage ;;
        *) log_err "未知参数: $1"; usage ;;
    esac
done

_ssh() {
    sshpass -p "$REMOTE_PASS" ssh -o StrictHostKeyChecking=no -o ConnectTimeout=10 -p "$SSH_PORT" "${REMOTE_USER}@${REMOTE_HOST}" "$@"
}

_scp() {
    sshpass -p "$REMOTE_PASS" scp -P "$SSH_PORT" -o StrictHostKeyChecking=no "$@"
}

sync_remote() {
    log_step "同步 size sweep 脚本和源码"
    _ssh "mkdir -p ${REMOTE_YCSB}/scripts ${REMOTE_YCSB}/workloads ${REMOTE_YCSB}/results"
    _scp "${SCRIPT_DIR}/run_progressive_hotspot.sh" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/run_progressive_hotspot.sh"
    _scp "${SCRIPT_DIR}/workloads/workload_progressive_hotspot" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/workloads/workload_progressive_hotspot"
    _scp "${SCRIPT_DIR}/scripts/visualize_progressive_hotspot.py" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/scripts/visualize_progressive_hotspot.py"
    _scp "${SCRIPT_DIR}/scripts/visualize_size_sweep.py" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/scripts/visualize_size_sweep.py"
    _ssh "chmod +x ${REMOTE_YCSB}/run_progressive_hotspot.sh ${REMOTE_YCSB}/scripts/visualize_progressive_hotspot.py ${REMOTE_YCSB}/scripts/visualize_size_sweep.py"

    local src_dir="${PROJECT_ROOT}/src"
    for f in "$src_dir"/*.c "$src_dir"/*.h "$src_dir"/Makefile; do
        _scp "$f" "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_NUMA_ROOT}/src/" 2>/dev/null
    done
    _ssh "cd ${REMOTE_NUMA_ROOT}/src && make -j\$(nproc) >/tmp/redis_numa_size_sweep_build.log 2>&1"
    log_ok "同步和编译完成"
}

ops_for_size() {
    local size="$1"
    if (( size < 16384 )); then
        echo 500000
    else
        echo 100000
    fi
}

records_for_size() {
    local size="$1"
    if (( size == 1048576 )); then
        echo 7000
        return
    fi
    if (( size == 1310720 )); then
        echo 5000
        return
    fi
    echo "$RECORD_COUNT"
}

size_label() {
    local size="$1"
    if (( size >= 1048576 )); then
        if (( size % 1048576 == 0 )); then
            echo "$((size / 1048576))MiB"
        else
            awk -v s="$size" 'BEGIN {printf "%.2fMiB", s/1048576}'
        fi
    elif (( size >= 1024 )); then
        if (( size % 1024 == 0 )); then
            echo "$((size / 1024))KiB"
        else
            awk -v s="$size" 'BEGIN {printf "%.1fKiB", s/1024}'
        fi
    else
        echo "${size}B"
    fi
}

run_variant() {
    local variant="$1"
    local port="$2"
    local maxmem="$3"
    local sweep_tag="$4"
    local extra_numa_args="${5:-}"
    local root_arg=""
    [[ "$variant" == "vanilla" ]] && root_arg="VANILLA_REDIS_ROOT=${REMOTE_VANILLA_ROOT}"

    local remote_out="${REMOTE_YCSB}/results/size_sweep_${sweep_tag}_${TIMESTAMP}"
    _ssh "mkdir -p ${remote_out}"
    log_step "运行 ${sweep_tag} size sweep"

    IFS=',' read -ra sizes <<< "$SIZE_LIST"
    for raw_size in "${sizes[@]}"; do
        local size
        size="$(xargs <<< "$raw_size")"
        [[ -n "$size" ]] || continue
        local ops records label case_dir extra_args
        ops="$(ops_for_size "$size")"
        records="$(records_for_size "$size")"
        label="$(size_label "$size")"
        case_dir="results/size_sweep_${sweep_tag}_${TIMESTAMP}/size_${size}"
        extra_args=""
        if [[ "$variant" == "numa" ]]; then
            extra_args="--numa-strategy ${NUMA_STRATEGY}"
        fi
        if [[ -n "$extra_numa_args" ]]; then
            extra_args="$extra_args $extra_numa_args"
        fi
        log "${sweep_tag}: size=${label}, records=${records}, ops=${ops}"
        _ssh "cd ${REMOTE_YCSB} && ${root_arg} bash run_progressive_hotspot.sh --variant ${variant} --port ${port} --output-dir ${case_dir} --records ${records} --fieldlength ${size} --threads ${THREADS} --ops ${ops} --read-proportion 0.5 --update-proportion 0.5 --maxmem ${maxmem} ${extra_args}" || {
            log_warn "${sweep_tag} size=${label} 返回非零，继续"
        }
    done

    _ssh "cd ${remote_out} && printf 'value_size_bytes,value_size_label,record_count,operation_count,throughput_ops_sec,bandwidth_mib_sec,read_avg_us,read_p95_us,read_p99_us,read_ok,read_error,read_error_pct,used_mem_mb,rss_mb,frag_ratio,numa_local_live_mb,numa_remote_live_mb,numa_node0_live_mb,numa_node2_live_mb,remote_pct\n' > size_sweep_summary.csv && for d in size_*; do size=\
\${d#size_}; csv=\"\$d/progressive_summary.csv\"; run=\"\$d/run_t${THREADS}.txt\"; [ -f \"\$csv\" ] || continue; ops=\$(awk -F': ' '/每个线程点操作数/ {print \$2; exit}' \"\$d/system_info.txt\"); records=\$(awk -F': ' '/Record count/ {print \$2; exit}' \"\$d/system_info.txt\"); label=\$(awk -v s=\"\$size\" 'BEGIN {if (s>=1048576) print \"1MiB\"; else if (s>=1024 && s%1024==0) printf \"%dKiB\\n\", s/1024; else if (s>=1024) printf \"%.1fKiB\\n\", s/1024; else printf \"%dB\\n\", s}'); ok=\$(awk -F, '\$1==\"[READ]\" && \$2 ~ /Operations/ {gsub(/^[ \\t]+|[ \\t]+$/, \"\", \$3); print \$3; exit}' \"\$run\" 2>/dev/null); err=\$(awk -F, '\$1==\"[READ]\" && \$2 ~ /Return=ERROR/ {gsub(/^[ \\t]+|[ \\t]+$/, \"\", \$3); print \$3; exit}' \"\$run\" 2>/dev/null); ok=\${ok:-0}; err=\${err:-0}; errpct=\$(awk -v ok=\"\$ok\" -v err=\"\$err\" 'BEGIN {t=ok+err; if (t>0) printf \"%.2f\", err*100/t; else printf \"0\"}'); awk -F, -v size=\"\$size\" -v label=\"\$label\" -v records=\"\$records\" -v ops=\"\$ops\" -v ok=\"\$ok\" -v err=\"\$err\" -v errpct=\"\$errpct\" 'NR==2 {print size \",\" label \",\" records \",\" ops \",\" \$2 \",\" \$3 \",\" \$4 \",\" \$5 \",\" \$6 \",\" ok \",\" err \",\" errpct \",\" \$8 \",\" \$9 \",\" \$10 \",\" \$11 \",\" \$12 \",\" \$13 \",\" \$14 \",\" \$15}' \"\$csv\"; done | sort -t, -n -k1,1 >> size_sweep_summary.csv"
}

fetch_and_plot() {
    log_step "下载并绘制 size sweep 结果"
    local fetch_dir="${LOCAL_RESULTS}/size_sweep_full_${TIMESTAMP}"
    mkdir -p "$fetch_dir/numa" "$fetch_dir/vanilla_local" "$fetch_dir/vanilla_interleaved" "$fetch_dir/numa_tinylfu"
    _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/size_sweep_numa_${TIMESTAMP}/." "$fetch_dir/numa/"
    _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/size_sweep_vanilla_local_${TIMESTAMP}/." "$fetch_dir/vanilla_local/"
    _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/size_sweep_vanilla_interleaved_${TIMESTAMP}/." "$fetch_dir/vanilla_interleaved/"
    _scp -r "${REMOTE_USER}@${REMOTE_HOST}:${REMOTE_YCSB}/results/size_sweep_numa_tinylfu_${TIMESTAMP}/." "$fetch_dir/numa_tinylfu/"

    local numa_csv="$fetch_dir/numa/size_sweep_summary.csv"
    local local_csv="$fetch_dir/vanilla_local/size_sweep_summary.csv"
    local interleaved_csv="$fetch_dir/vanilla_interleaved/size_sweep_summary.csv"
    local tinylfu_csv="$fetch_dir/numa_tinylfu/size_sweep_summary.csv"
    local output="$fetch_dir/size_sweep_compare.png"
    if python3 -c "import matplotlib" 2>/dev/null; then
        local plot_args=(
            "${SCRIPT_DIR}/scripts/visualize_size_sweep.py"
            --input "$numa_csv"
            --label "Redis-NUMA (Composite LRU)"
            --output "$output"
            --title "YCSB Hotspot Read Size Sweep: 32 threads"
        )
        [[ -f "$local_csv" ]] && plot_args+=(--compare-input "$local_csv" --compare-label "Vanilla Redis (local)")
        [[ -f "$interleaved_csv" ]] && plot_args+=(--compare-input2 "$interleaved_csv" --compare-label2 "Vanilla Redis (interleaved)")
        [[ -f "$tinylfu_csv" ]] && plot_args+=(--compare-input3 "$tinylfu_csv" --compare-label3 "Redis-NUMA (TinyLFU)")
        python3 "${plot_args[@]}" || log_warn "本地绘图失败"
    else
        log_warn "本地 matplotlib 不可用，跳过绘图"
    fi
    generate_summary "$fetch_dir"
    log_ok "结果目录: $fetch_dir"
}

generate_summary() {
    local dir="$1"
    local summary="$dir/SUMMARY.txt"
    {
        echo "YCSB 32线程热点读 value-size sweep 摘要"
        echo "========================================"
        echo "时间: $(date '+%Y-%m-%d %H:%M:%S')"
        echo "负载: up to ${RECORD_COUNT} keys, 32 threads, hotspot read/update 5:5"
        echo "Key 数量: ${RECORD_COUNT} keys (1MiB 7000 keys, 1.25MiB 5000 keys)"
        echo "操作数: <16KiB 200000 ops, >=16KiB 40000 ops"
        echo "NUMA 策略: ${NUMA_STRATEGY}"
        echo ""
        for variant in numa vanilla_local vanilla_interleaved numa_tinylfu; do
            local csv="$dir/$variant/size_sweep_summary.csv"
            if [[ -f "$csv" ]]; then
                echo "── ${variant} ──"
                column -s, -t "$csv" 2>/dev/null || cat "$csv"
                echo ""
            fi
        done
    } > "$summary"
    log_ok "摘要已生成: $summary"
}

main() {
    command -v sshpass &>/dev/null || { log_err "sshpass 未安装，请执行: sudo apt install -y sshpass"; exit 1; }
    log "测试 SSH 连接..."
    _ssh "echo ok" &>/dev/null || { log_err "SSH 连接失败: ${REMOTE_USER}@${REMOTE_HOST}"; exit 1; }
    log_ok "SSH 连接正常"

    sync_remote
    run_variant numa "$TEST_PORT" "$MAXMEM_NUMA" "numa"
    run_variant vanilla "$((TEST_PORT+1))" "$MAXMEM_VANILLA" "vanilla_local" "--vanilla-cpu-node 0 --vanilla-mem-node 0"
    run_variant vanilla "$((TEST_PORT+2))" "$MAXMEM_VANILLA" "vanilla_interleaved" "--vanilla-cpu-node 0 --vanilla-mem-node 0,2 --vanilla-mem-policy interleave"
    run_variant numa "$((TEST_PORT+3))" "$MAXMEM_NUMA" "numa_tinylfu" "--tinylfu"
    fetch_and_plot
    log_ok "size sweep 测试完成"
}

main "$@"
