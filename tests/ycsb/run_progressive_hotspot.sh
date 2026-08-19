#!/usr/bin/env bash
# ============================================================================
# Redis-NUMA YCSB 渐进并发热点访问压力测试
#
# 基础负载：10000 * 512KB key
# 渐进并发：4,8,12,...,128 线程
# 输出：请求处理速度、应用层访问带宽、延迟、内存统计和图表
# ============================================================================

set -euo pipefail

_on_err() {
    local exit_code=$?
    local line_no=${BASH_LINENO[0]}
    echo -e "\033[0;31m[ERR-TRAP]\033[0m 脚本在第 ${line_no} 行以退出码 ${exit_code} 失败" >&2
    echo -e "\033[0;31m[ERR-TRAP]\033[0m 失败命令: ${BASH_COMMAND}" >&2
}
trap '_on_err' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REDIS_SERVER="$PROJECT_ROOT/src/redis-server"
REDIS_CLI="$PROJECT_ROOT/src/redis-cli"
YCSB_DIR="$SCRIPT_DIR/ycsb-0.17.0"
WORKLOAD="$SCRIPT_DIR/workloads/workload_progressive_hotspot"
VISUALIZE_SCRIPT="$SCRIPT_DIR/scripts/visualize_progressive_hotspot.py"

SAFE_LINK="/tmp/redis-progressive-bench-$$"
ln -sfn "$PROJECT_ROOT" "$SAFE_LINK"
if [[ -x "$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb.sh" ]]; then
    YCSB_BIN="$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb.sh"
else
    YCSB_BIN="$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb"
fi
WORKLOAD="$SAFE_LINK/tests/ycsb/workloads/workload_progressive_hotspot"
VISUALIZE_SCRIPT="$SAFE_LINK/tests/ycsb/scripts/visualize_progressive_hotspot.py"

REDIS_HOST="127.0.0.1"
REDIS_PORT=6379
REDIS_VARIANT="numa"
VANILLA_ROOT="${VANILLA_REDIS_ROOT:-$PROJECT_ROOT/../redis-6.2.21}"
MAX_MEMORY="16gb"
OUTPUT_DIR=""
NO_RESTART=false
SKIP_LOAD=false
PROCESS_NUMA_NODES="0,2"
THREAD_LIST="4,8,12,16,20,24,28,32,36,40,44,48,52,56,60,64,68,72,76,80,84,88,92,96,100,104,108,112,116,120,124,128"
RECORD_COUNT=1000000
FIELD_LENGTH=4096
FIELD_COUNT=1
OPERATION_COUNT=100000
READ_PROPORTION="0.5"
UPDATE_PROPORTION="0.5"
HOTSPOT_DATA_FRACTION="0.05"
HOTSPOT_OPN_FRACTION="0.95"
YCSB_TIMEOUT_MS=60000
ENABLE_LOCALITY_STATS=true
ENABLE_ACCESS_TRACKING=true
ENABLE_AUTO_MIGRATE=true
NUMA_STRATEGY="interleaved"
ENABLE_TINYLFU=false
ENABLE_AE_SCHEDULER=false
VANILLA_CPU_NODE="${VANILLA_CPU_NODE:-0}"
VANILLA_MEM_NODE="${VANILLA_MEM_NODE:-0}"
VANILLA_MEM_POLICY="${VANILLA_MEM_POLICY:-bind}"

SUMMARY_CSV=""

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*" >&2; }
log_step() { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --port PORT              Redis 端口 (默认: 6379)
  --variant numa|vanilla   Redis 类型 (默认: numa)
  --vanilla-root DIR       原版 Redis 根目录 (默认: ../redis-6.2.21)
  --maxmem MEM             Redis maxmemory (默认: 16gb)
  --output-dir DIR         输出目录 (默认: results/progressive_hotspot_<timestamp>)
  --threads LIST           线程列表，逗号分隔 (默认: 4,8,12,...,128)
  --records N              key 数量 (默认: 10000)
  --fieldlength BYTES      value 大小 (默认: 4096，即 4KB)
  --ops N                  每个线程点的 YCSB 操作数 (默认: 100000)
  --read-only              只读热点访问 (read=1.0, update=0)，用于纯访问带宽测试
  --read-proportion P      读比例 (默认: 0.5)
  --update-proportion P    更新比例 (默认: 0.5)
  --numa-strategy NAME     NUMA 分配策略 (默认: cxl_optimized)
  --ae-scheduler           已失效（ADR-08 移除了 AE/servercron 逐槎位调度），仅保留参数解析
  --tinylfu                切换到 TinyLFU 策略 (NUMA FLOW DEFAULT tinylfu)
  --no-locality-stats      禁用 NUMA 本地/远端访问计数
  --no-access-tracking     禁用 Composite LRU 访问热路径统计
  --no-auto-migrate        禁用 Composite LRU 后台自动迁移
  --skip-load              跳过 10000*1MB 初始加载
  --no-restart             使用已有 Redis，不重启
  --process-nodes NODES    Redis-NUMA 进程 NUMA 节点 (默认: 0,2；传 all 禁用绑定)
  --vanilla-cpu-node NODE   Vanilla Redis CPU 绑定节点 (默认: 0)
  --vanilla-mem-node NODE   Vanilla Redis 内存绑定节点 (默认: 0)
  --vanilla-mem-policy P    bind 或 interleave (默认: bind)
  --host HOST              Redis host (默认: 127.0.0.1)
  --help                   显示帮助

示例:
  $(basename "$0")
  $(basename "$0") --threads 4,16,32,64,128 --ops 20000
  $(basename "$0") --no-restart --skip-load --port 6399
EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --port) REDIS_PORT="$2"; shift 2 ;;
            --variant) REDIS_VARIANT="$2"; shift 2 ;;
            --vanilla-root) VANILLA_ROOT="$2"; shift 2 ;;
            --maxmem) MAX_MEMORY="$2"; shift 2 ;;
            --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
            --threads) THREAD_LIST="$2"; shift 2 ;;
            --records) RECORD_COUNT="$2"; shift 2 ;;
            --fieldlength) FIELD_LENGTH="$2"; shift 2 ;;
            --ops) OPERATION_COUNT="$2"; shift 2 ;;
            --read-only) READ_PROPORTION="1.0"; UPDATE_PROPORTION="0"; shift ;;
            --read-proportion) READ_PROPORTION="$2"; shift 2 ;;
            --update-proportion) UPDATE_PROPORTION="$2"; shift 2 ;;
            --numa-strategy) NUMA_STRATEGY="$2"; shift 2 ;;
            --ae-scheduler) ENABLE_AE_SCHEDULER=true; shift ;;
            --tinylfu) ENABLE_TINYLFU=true; shift ;;
            --no-locality-stats) ENABLE_LOCALITY_STATS=false; shift ;;
            --no-access-tracking) ENABLE_ACCESS_TRACKING=false; shift ;;
            --no-auto-migrate) ENABLE_AUTO_MIGRATE=false; shift ;;
            --skip-load) SKIP_LOAD=true; shift ;;
            --no-restart) NO_RESTART=true; shift ;;
            --process-nodes) PROCESS_NUMA_NODES="$2"; shift 2 ;;
            --vanilla-cpu-node) VANILLA_CPU_NODE="$2"; shift 2 ;;
            --vanilla-mem-node) VANILLA_MEM_NODE="$2"; shift 2 ;;
            --vanilla-mem-policy) VANILLA_MEM_POLICY="$2"; shift 2 ;;
            --host) REDIS_HOST="$2"; shift 2 ;;
            --locality-stats) ENABLE_LOCALITY_STATS=true; shift ;;
            --help|-h) usage ;;
            *) log_err "未知参数: $1"; usage ;;
        esac
    done
}

check_prerequisites() {
    if [[ "$REDIS_VARIANT" == "vanilla" ]]; then
        REDIS_SERVER="$VANILLA_ROOT/src/redis-server"
        REDIS_CLI="$VANILLA_ROOT/src/redis-cli"
        ENABLE_LOCALITY_STATS=false
    elif [[ "$REDIS_VARIANT" != "numa" ]]; then
        log_err "无效 Redis 类型: $REDIS_VARIANT"
        exit 1
    fi

    log_step "前置检查"
    [[ -x "$REDIS_SERVER" ]] || { log_err "redis-server 未找到: $REDIS_SERVER"; exit 1; }
    [[ -x "$REDIS_CLI" ]] || { log_err "redis-cli 未找到: $REDIS_CLI"; exit 1; }
    [[ -x "$YCSB_BIN" ]] || { log_err "YCSB 未找到: $YCSB_BIN"; exit 1; }
    [[ -f "$WORKLOAD" ]] || { log_err "工作负载不存在: $WORKLOAD"; exit 1; }
    command -v python3 &>/dev/null && log_ok "python3 可用" || log_warn "python3 不可用，将跳过绘图"
    log_ok "redis-server: $REDIS_SERVER"
    log_ok "YCSB: $YCSB_BIN"
    log_ok "workload: $WORKLOAD"
}

save_system_info() {
    local sysinfo="$OUTPUT_DIR/system_info.txt"
    {
        echo "Redis-NUMA YCSB 渐进并发热点访问压力测试"
        echo "=============================================="
        echo "测试时间: $(date)"
        echo "主机名: $(hostname)"
        echo "Redis 类型: $REDIS_VARIANT"
        echo "Redis 端口: $REDIS_PORT"
        echo "Redis maxmemory: $MAX_MEMORY"
        echo "记录数: $RECORD_COUNT"
        echo "字段大小: $FIELD_LENGTH bytes"
        echo "每个线程点操作数: $OPERATION_COUNT"
        echo "线程列表: $THREAD_LIST"
        echo "读写比例: read=$READ_PROPORTION update=$UPDATE_PROPORTION"
        echo "NUMA 策略: $NUMA_STRATEGY"
        if [[ "$REDIS_VARIANT" == "vanilla" ]]; then
            echo "Vanilla CPU node: $VANILLA_CPU_NODE"
            echo "Vanilla memory node: $VANILLA_MEM_NODE"
        fi
        echo "NUMA 消融开关: locality_stats=$ENABLE_LOCALITY_STATS access_tracking=$ENABLE_ACCESS_TRACKING auto_migrate=$ENABLE_AUTO_MIGRATE tinylfu=$ENABLE_TINYLFU ae_scheduler=$ENABLE_AE_SCHEDULER"
        echo "热点分布: data=$HOTSPOT_DATA_FRACTION operations=$HOTSPOT_OPN_FRACTION"
        echo ""
        echo "=== 内核 ==="
        uname -a
        echo ""
        echo "=== CPU ==="
        awk -F': ' '/^model name/{print $2; exit}' /proc/cpuinfo
        echo "核心数: $(nproc)"
        echo ""
        echo "=== 内存 ==="
        free -h
        echo ""
        echo "=== NUMA 拓扑 ==="
        numactl --hardware 2>/dev/null || echo "numactl 不可用"
        echo ""
        echo "=== Redis ==="
        "$REDIS_SERVER" --version
        echo ""
        echo "=== YCSB ==="
        "$YCSB_BIN" --version 2>&1 | head -3 || true
    } > "$sysinfo"
    log_ok "系统信息: $sysinfo"
}

start_redis() {
    log_step "启动 Redis"
    if [[ "$REDIS_VARIANT" == "numa" && $EUID -eq 0 ]]; then
        echo 0 > /proc/sys/kernel/numa_balancing 2>/dev/null || true
        echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
        echo never > /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || true
    elif [[ "$REDIS_VARIANT" == "numa" ]]; then
        log_warn "非 root 运行，无法关闭 NUMA Balancing/THP"
    fi

    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    sleep 1
    pkill -f "redis-server.*:${REDIS_PORT}" 2>/dev/null || true
    sleep 1

    local -a numa_cmd=()
    if command -v numactl &>/dev/null; then
        if [[ "$REDIS_VARIANT" == "numa" ]]; then
            if [[ "$PROCESS_NUMA_NODES" == "all" ]]; then
                log "未限制 Redis-NUMA 进程 NUMA 节点"
            else
                numa_cmd=(numactl --cpunodebind="$PROCESS_NUMA_NODES" --membind="$PROCESS_NUMA_NODES")
                log "绑定 Redis-NUMA 进程 CPU 和内存到 NUMA Node $PROCESS_NUMA_NODES"
            fi
        elif [[ "$REDIS_VARIANT" == "vanilla" ]]; then
            if [[ "$VANILLA_MEM_POLICY" == "interleave" ]]; then
                numa_cmd=(numactl --cpunodebind="$VANILLA_CPU_NODE" --interleave="$VANILLA_MEM_NODE")
                log "绑定 Vanilla Redis CPU 到 Node $VANILLA_CPU_NODE，内存 interleave 到 Node $VANILLA_MEM_NODE"
            else
                numa_cmd=(numactl --cpunodebind="$VANILLA_CPU_NODE" --membind="$VANILLA_MEM_NODE")
                log "绑定 Vanilla Redis 进程 CPU 到 NUMA Node $VANILLA_CPU_NODE，内存到 NUMA Node $VANILLA_MEM_NODE"
            fi
        fi
    elif [[ "$REDIS_VARIANT" == "vanilla" ]]; then
        log_warn "numactl 未安装，Vanilla Redis 将不进行本地内存绑定"
    fi

    "${numa_cmd[@]}" "$REDIS_SERVER" \
        --port "$REDIS_PORT" \
        --bind "$REDIS_HOST" \
        --maxmemory "$MAX_MEMORY" \
        --maxmemory-policy noeviction \
        --save "" \
        --appendonly no \
        --loglevel verbose \
        --logfile "$OUTPUT_DIR/redis.log" \
        --daemonize yes

    local retries=30
    while [[ $retries -gt 0 ]]; do
        if [[ "$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING 2>/dev/null || true)" == *PONG* ]]; then
            "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" FLUSHALL >/dev/null 2>&1 || true
            log_ok "Redis 已就绪"
            return 0
        fi
        retries=$((retries - 1))
        sleep 1
    done
    log_err "Redis 启动失败"
    return 1
}

init_numa() {
    [[ "$REDIS_VARIANT" == "numa" ]] || return 0
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA CONFIG SET enabled_nodes 0,2 >/dev/null 2>&1 || true
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA CONFIG SET strategy "$NUMA_STRATEGY" >/dev/null 2>&1 || true

    # ADR-08 之后迁移策略统一收敛到 NUMAflow：composite_lru.json/NUMA CONFIG
    # LOAD 和 access_tracking/locality_stats/auto_migrate_enabled 这几个
    # composite-lru 私有开关已随原生模块一起移除（NUMAflow 的
    # build_composite_lru 预设参数固定，不支持这几个开关）。--no-locality-stats
    # /--no-access-tracking/--no-auto-migrate 三个消融开关目前是 no-op，仅保留
    # 参数解析以兼容旧调用方，不再实际影响 Redis 行为。
    if [[ "$ENABLE_LOCALITY_STATS" == false || "$ENABLE_ACCESS_TRACKING" == false || "$ENABLE_AUTO_MIGRATE" == false ]]; then
        log_warn "--no-locality-stats/--no-access-tracking/--no-auto-migrate 已失效（ADR-08），composite-lru 私有开关随原生模块一起移除"
    fi
    if [[ "$ENABLE_TINYLFU" == true ]]; then
        log "切换到 TinyLFU 策略 (NUMA FLOW DEFAULT tinylfu)"
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA FLOW DEFAULT tinylfu >/dev/null 2>&1 || true
    else
        log "切换到 Composite LRU 策略 (NUMA FLOW DEFAULT composite_lru)"
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA FLOW DEFAULT composite_lru >/dev/null 2>&1 || true
    fi

    if [[ "$ENABLE_AE_SCHEDULER" == true ]]; then
        # ADR-08 之后 numa_strategy_slots（连同 ADR-07 的逐槎位 AE/servercron
        # 调度切换）已整体退役，NUMAflow 只有 serverCron 一种调度路径。这个
        # 开关现在是无效的 no-op，保留仅为了不破坏调用方的参数解析。
        log_warn "--ae-scheduler 已失效：AE/servercron 调度切换随 numa_strategy_slots 一起被移除（ADR-08），本次运行仍走 serverCron"
    fi
}

run_load() {
    log_step "加载基础负载: ${RECORD_COUNT} * ${FIELD_LENGTH}B"
    local total_gib
    total_gib=$(awk -v r="$RECORD_COUNT" -v f="$FIELD_LENGTH" 'BEGIN {printf "%.2f", r*f/1024/1024/1024}')
    log "预计用户数据量: ${total_gib} GiB"

    "$YCSB_BIN" load redis -s \
        -P "$WORKLOAD" \
        -p "recordcount=$RECORD_COUNT" \
        -p "fieldcount=$FIELD_COUNT" \
        -p "fieldlength=$FIELD_LENGTH" \
        -p "fieldlengthdistribution=constant" \
        -p "redis.host=$REDIS_HOST" \
        -p "redis.port=$REDIS_PORT" \
        -p "redis.timeout=$YCSB_TIMEOUT_MS" \
        -p "threadcount=8" \
        2>&1 | tee "$OUTPUT_DIR/load.txt"

    if [[ "$REDIS_VARIANT" == "numa" ]]; then
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA CONFIG STATS > "$OUTPUT_DIR/config_stats_after_load.txt" 2>&1 || true
    fi
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory > "$OUTPUT_DIR/info_memory_after_load.txt" 2>&1 || true
    log_ok "基础负载加载完成"
}

extract_metric() {
    local file="$1"
    local op="$2"
    local metric="$3"
    awk -F, -v op="[$op]" -v metric="$metric" '
        $1 == op && $2 ~ metric {gsub(/^[ \t]+|[ \t]+$/, "", $3); print $3; exit}
    ' "$file"
}

collect_memory_metrics() {
    local meminfo used_mem rss_mem frag
    meminfo=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory 2>/dev/null || echo "")
    used_mem=$(grep "used_memory:" <<< "$meminfo" | head -1 | cut -d: -f2 | tr -d '\r')
    rss_mem=$(grep "used_memory_rss:" <<< "$meminfo" | head -1 | cut -d: -f2 | tr -d '\r')
    frag=$(grep "mem_fragmentation_ratio:" <<< "$meminfo" | cut -d: -f2 | tr -d '\r')
    awk -v u="${used_mem:-0}" -v r="${rss_mem:-0}" -v f="${frag:-0}" 'BEGIN {printf "%.1f,%.1f,%s", u/1048576, r/1048576, f}'
}

collect_numa_live_memory_metrics() {
    [[ "$REDIS_VARIANT" == "numa" ]] || { echo "0.0,0.0,0.0,0.0"; return; }
    local stats node0 node1 node2 node3 local_bytes remote_bytes
    stats=$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" --raw NUMA CONFIG STATS 2>/dev/null || echo "")
    node0=$(awk '/^node0_live$/ {getline; print; exit}' <<< "$stats")
    node1=$(awk '/^node1_live$/ {getline; print; exit}' <<< "$stats")
    node2=$(awk '/^node2_live$/ {getline; print; exit}' <<< "$stats")
    node3=$(awk '/^node3_live$/ {getline; print; exit}' <<< "$stats")
    node0=${node0:-0}; node1=${node1:-0}; node2=${node2:-0}; node3=${node3:-0}
    local_bytes=$((node0 + node1))
    remote_bytes=$((node2 + node3))
    awk -v l="$local_bytes" -v r="$remote_bytes" -v n0="$node0" -v n2="$node2" 'BEGIN {printf "%.1f,%.1f,%.1f,%.1f", l/1048576, r/1048576, n0/1048576, n2/1048576}'
}

collect_remote_pct() {
    [[ "$REDIS_VARIANT" == "numa" ]] || { echo "0"; return; }
    local acc_local acc_remote total
    # NOTE: ADR-08 之后 per-strategy 的 local/remote 访问分布计数器（原
    # composite_lru/tinylfu 私有统计）已随原生模块一起移除，NUMAflow 的桥接
    # 不追踪这个细分，这个指标恒为 0，仅保留字段以兼容下游 CSV 列结构。
    acc_local=0
    acc_remote=0
    total=$((acc_local + acc_remote))
    if [[ "$total" -gt 0 ]]; then
        awk -v r="$acc_remote" -v t="$total" 'BEGIN {printf "%.2f", r*100/t}'
    else
        echo "0"
    fi
}

run_thread_point() {
    local threads="$1"
    local out="$OUTPUT_DIR/run_t${threads}.txt"
    log_step "热点访问: ${threads} threads"

    "$YCSB_BIN" run redis -s \
        -P "$WORKLOAD" \
        -p "recordcount=$RECORD_COUNT" \
        -p "operationcount=$OPERATION_COUNT" \
        -p "threadcount=$threads" \
        -p "fieldcount=$FIELD_COUNT" \
        -p "fieldlength=$FIELD_LENGTH" \
        -p "fieldlengthdistribution=constant" \
        -p "readproportion=$READ_PROPORTION" \
        -p "updateproportion=$UPDATE_PROPORTION" \
        -p "insertproportion=0" \
        -p "scanproportion=0" \
        -p "requestdistribution=hotspot" \
        -p "hotspotdatafraction=$HOTSPOT_DATA_FRACTION" \
        -p "hotspotopnfraction=$HOTSPOT_OPN_FRACTION" \
        -p "redis.host=$REDIS_HOST" \
        -p "redis.port=$REDIS_PORT" \
        -p "redis.timeout=$YCSB_TIMEOUT_MS" \
        2>&1 | tee "$out"

    local throughput read_avg read_p95 read_p99 update_avg bandwidth mem numa_mem remote_pct
    throughput=$(extract_metric "$out" "OVERALL" "Throughput" || true)
    read_avg=$(extract_metric "$out" "READ" "AverageLatency" || true)
    read_p95=$(extract_metric "$out" "READ" "95thPercentileLatency" || true)
    read_p99=$(extract_metric "$out" "READ" "99thPercentileLatency" || true)
    update_avg=$(extract_metric "$out" "UPDATE" "AverageLatency" || true)
    [[ -z "$throughput" ]] && throughput=0
    [[ -z "$read_avg" ]] && read_avg=0
    [[ -z "$read_p95" ]] && read_p95=0
    [[ -z "$read_p99" ]] && read_p99=0
    [[ -z "$update_avg" ]] && update_avg=0

    bandwidth=$(awk -v ops="$throughput" -v bytes="$FIELD_LENGTH" 'BEGIN {printf "%.2f", ops*bytes/1024/1024}')
    mem=$(collect_memory_metrics)
    numa_mem=$(collect_numa_live_memory_metrics)
    remote_pct=$(collect_remote_pct)
    echo "${threads},${throughput},${bandwidth},${read_avg},${read_p95},${read_p99},${update_avg},${mem},${numa_mem},${remote_pct}" >> "$SUMMARY_CSV"
    log_ok "threads=${threads}, throughput=${throughput} ops/s, bandwidth=${bandwidth} MiB/s"
}

generate_report() {
    log_step "生成图表"
    if ! command -v python3 &>/dev/null; then
        log_warn "python3 不可用，跳过绘图"
        return
    fi
    local venv_dir="$SCRIPT_DIR/scripts/.venv"
    local python="$venv_dir/bin/python"
    if [[ ! -x "$python" ]]; then
        python3 -m venv "$venv_dir" || { log_warn "创建 venv 失败"; return; }
        "$venv_dir/bin/pip" install --quiet matplotlib pandas || { log_warn "安装绘图依赖失败"; return; }
    else
        python="python3"
    fi
    "$python" "$VISUALIZE_SCRIPT" \
        --input "$SUMMARY_CSV" \
        --output "$OUTPUT_DIR/progressive_hotspot_report.png" \
        --title "YCSB Progressive Hotspot Benchmark" \
        2>&1 || log_warn "绘图失败，请查看 $SUMMARY_CSV"
    [[ -f "$OUTPUT_DIR/progressive_hotspot_report.png" ]] && log_ok "图表: $OUTPUT_DIR/progressive_hotspot_report.png"
}

print_summary() {
    log_step "测试摘要"
    column -s, -t "$SUMMARY_CSV" 2>/dev/null || cat "$SUMMARY_CSV"
    echo ""
    echo "输出目录: $OUTPUT_DIR"
    [[ -f "$OUTPUT_DIR/progressive_hotspot_report.png" ]] && echo "图表: $OUTPUT_DIR/progressive_hotspot_report.png"
}

cleanup() {
    local exit_code=$?
    log "清理中... (退出码: ${exit_code})"
    rm -f "$SAFE_LINK" 2>/dev/null || true
    if [[ "$NO_RESTART" = false ]]; then
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE 2>/dev/null || true
    fi
    if [[ $EUID -eq 0 ]]; then
        echo 1 > /proc/sys/kernel/numa_balancing 2>/dev/null || true
    fi
}

main() {
    parse_args "$@"
    if [[ -z "$OUTPUT_DIR" ]]; then
        OUTPUT_DIR="$SCRIPT_DIR/results/progressive_hotspot_${REDIS_VARIANT}_$(date +%Y%m%d_%H%M%S)"
    fi
    mkdir -p "$OUTPUT_DIR"
    SUMMARY_CSV="$OUTPUT_DIR/progressive_summary.csv"

    echo -e "${BOLD}${CYAN}"
    echo "╔════════════════════════════════════════════════════╗"
    echo "║      YCSB 渐进并发热点访问压力测试                  ║"
    echo "╚════════════════════════════════════════════════════╝"
    echo -e "${NC}"
    log "输出目录: $OUTPUT_DIR"
    log "线程列表: $THREAD_LIST"

    trap cleanup EXIT
    check_prerequisites
    save_system_info

    if [[ "$NO_RESTART" = false ]]; then
        start_redis
    else
        [[ "$("$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" PING 2>/dev/null || true)" == *PONG* ]] || { log_err "Redis 未响应"; exit 1; }
        log_ok "Redis 连接正常"
    fi
    init_numa

    if [[ "$SKIP_LOAD" = false ]]; then
        run_load
        if [[ "$REDIS_VARIANT" == "numa" ]]; then
            "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA CONFIG SET strategy "$NUMA_STRATEGY" >/dev/null 2>&1 || true
        fi
    else
        log_warn "跳过加载阶段 (--skip-load)"
    fi

    echo "threads,throughput_ops_sec,bandwidth_mib_sec,read_avg_us,read_p95_us,read_p99_us,update_avg_us,used_mem_mb,rss_mb,frag_ratio,numa_local_live_mb,numa_remote_live_mb,numa_node0_live_mb,numa_node2_live_mb,remote_pct" > "$SUMMARY_CSV"
    IFS=',' read -ra thread_points <<< "$THREAD_LIST"
    for t in "${thread_points[@]}"; do
        t="$(xargs <<< "$t")"
        [[ -n "$t" ]] || continue
        run_thread_point "$t"
    done

    if [[ "$REDIS_VARIANT" == "numa" ]]; then
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA MIGRATE STATS > "$OUTPUT_DIR/migrate_stats_final.txt" 2>&1 || true
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" NUMA CONFIG STATS > "$OUTPUT_DIR/config_stats_final.txt" 2>&1 || true
    fi
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" INFO memory > "$OUTPUT_DIR/info_memory_final.txt" 2>&1 || true

    generate_report
    print_summary
    log_ok "渐进压力测试完成"
}

main "$@"
