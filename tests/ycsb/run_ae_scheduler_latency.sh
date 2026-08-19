#!/usr/bin/env bash
# ============================================================================
# ARCHIVED (ADR-08, docs/new/09-architecture-decisions.md): this benchmark
# compares the per-slot AE-time-event vs serverCron scheduling modes that
# `numa_strategy_slots` (ADR-07) used to offer via `NUMA STRATEGY SLOT
# SCHEDULE`. That framework, and the AE/servercron toggle with it, has been
# removed - migration is now driven solely by NUMAflow's `numa_flow_cron()`
# on serverCron, with no AE time-event variant. This script will fail against
# current builds (`NUMA STRATEGY` no longer exists); kept for historical
# reference only. Do not port it without first deciding whether an
# AE-scheduled NUMAflow mode is worth building - that is a new feature, not
# a like-for-like migration of this one.
# ============================================================================
#
# Redis-NUMA AE Strategy Scheduler latency disturbance benchmark
#
# Runs a fixed-window YCSB hotspot workload while migration pressure is injected
# in the background, then compares read P99 latency across scheduler modes:
#   - serverCron
#   - AE time event
#
# Default pressure mode is "access": it repeatedly reads hot YCSB keys so the
# Composite LRU/TinyLFU access path enqueues migration candidates. This measures
# strategy scheduler behavior. The "scan" and "db" modes are available for
# manual blocking-command stress, but they should not be used as primary AE
# scheduler evidence.
# ============================================================================

set -euo pipefail

_on_err() {
    local exit_code=$?
    local line_no=${BASH_LINENO[0]}
    echo -e "\033[0;31m[ERR-TRAP]\033[0m line ${line_no}, exit ${exit_code}" >&2
    echo -e "\033[0;31m[ERR-TRAP]\033[0m command: ${BASH_COMMAND}" >&2
}
trap '_on_err' ERR

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
REDIS_SERVER="$PROJECT_ROOT/src/redis-server"
REDIS_CLI="$PROJECT_ROOT/src/redis-cli"
WORKLOAD="$SCRIPT_DIR/workloads/workload_progressive_hotspot"
VISUALIZE_SCRIPT="$SCRIPT_DIR/scripts/visualize_ae_scheduler_latency.py"
TRIGGER_SCRIPT="$SCRIPT_DIR/trigger_ae_migration_pressure.sh"

SAFE_LINK="/tmp/redis-ae-latency-bench-$$"
ln -sfn "$PROJECT_ROOT" "$SAFE_LINK"
if [[ -x "$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb.sh" ]]; then
    YCSB_BIN="$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb.sh"
else
    YCSB_BIN="$SAFE_LINK/tests/ycsb/ycsb-0.17.0/bin/ycsb"
fi
WORKLOAD="$SAFE_LINK/tests/ycsb/workloads/workload_progressive_hotspot"
VISUALIZE_SCRIPT="$SAFE_LINK/tests/ycsb/scripts/visualize_ae_scheduler_latency.py"
TRIGGER_SCRIPT="$SAFE_LINK/tests/ycsb/trigger_ae_migration_pressure.sh"

REDIS_HOST="127.0.0.1"
REDIS_PORT=6419
OUTPUT_DIR=""
MAX_MEMORY="16gb"
PROCESS_NUMA_NODES="0,2"
SCHEDULER_MODE="servercron"
ENABLE_TINYLFU=false
NOOP_MIGRATION=false
HIGH_PRESSURE=false
VANILLA_MODE=false
VANILLA_REDIS_ROOT="${VANILLA_REDIS_ROOT:-$PROJECT_ROOT/../redis-6.2.21}"
NO_RESTART=false
SKIP_LOAD=false
REMOTE_RESET=false
KEEP_REDIS=false

RECORD_COUNT=100000
FIELD_LENGTH=4096
FIELD_COUNT=1
THREADS=64
WINDOWS=12
WINDOW_OPS=100000
READ_PROPORTION="0.95"
UPDATE_PROPORTION="0.05"
HOTSPOT_DATA_FRACTION="0.05"
HOTSPOT_OPN_FRACTION="0.95"
YCSB_TIMEOUT_MS=60000
LOAD_THREADS=8

PRESSURE_MODE="access"
PRESSURE_INTERVAL_SEC=1
PRESSURE_KEY_COUNT=500
PRESSURE_REPEAT=2
PRESSURE_SCAN_COUNT=2500
PRESSURE_DB_TARGETS="2,0"

SUMMARY_CSV=""
PRESSURE_PID=""
HOT_KEYS_FILE=""

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*" >&2; }
log_step() { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

usage() {
    cat <<EOF
Usage: $(basename "$0") [options]

Benchmark:
  --scheduler servercron|ae   Strategy slot scheduler mode (default: servercron)
  --tinylfu                  Use Slot 2 TinyLFU instead of Slot 1 Composite LRU
  --port PORT                Redis port (default: 6419)
  --maxmem MEM               Redis maxmemory (default: 16gb)
  --output-dir DIR           Output directory
  --records N                YCSB recordcount (default: 100000)
  --fieldlength BYTES        YCSB value size (default: 4096)
  --threads N                YCSB client threads per window (default: 64)
  --windows N                Number of measurement windows (default: 12)
  --window-ops N             YCSB operationcount per window (default: 100000)
  --read-proportion P        Read ratio (default: 0.95)
  --update-proportion P      Update ratio (default: 0.05)
  --remote-reset             After load, synchronously move DB to remote node 2
  --skip-load                Reuse existing dataset
  --no-restart               Reuse existing Redis
  --keep-redis               Do not shut Redis down on exit

Migration pressure:
  --pressure-mode access|scan|db|none
                             access=inject hot reads to enqueue strategy candidates
                             scan=run NUMA MIGRATE SCAN; db=run NUMA MIGRATE DB
                             none=no background pressure
  --pressure-key-count N     Number of hot keys per access injection round (default: 500)
  --pressure-repeat N        Repeats over hot key set per round (default: 2)
  --pressure-interval SEC    Seconds between pressure rounds (default: 1)
  --pressure-scan-count N    COUNT for NUMA MIGRATE SCAN (default: 2500)

Other:
  --process-nodes NODES      numactl CPU/memory nodes for Redis-NUMA (default: 0,2)
  --host HOST                Redis host (default: 127.0.0.1)
  --help                     Show help
EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --scheduler) SCHEDULER_MODE="$2"; shift 2 ;;
            --tinylfu) ENABLE_TINYLFU=true; shift ;;
            --port) REDIS_PORT="$2"; shift 2 ;;
            --maxmem) MAX_MEMORY="$2"; shift 2 ;;
            --output-dir) OUTPUT_DIR="$2"; shift 2 ;;
            --records) RECORD_COUNT="$2"; shift 2 ;;
            --fieldlength) FIELD_LENGTH="$2"; shift 2 ;;
            --threads) THREADS="$2"; shift 2 ;;
            --windows) WINDOWS="$2"; shift 2 ;;
            --window-ops) WINDOW_OPS="$2"; shift 2 ;;
            --read-proportion) READ_PROPORTION="$2"; shift 2 ;;
            --update-proportion) UPDATE_PROPORTION="$2"; shift 2 ;;
            --remote-reset) REMOTE_RESET=true; shift ;;
            --skip-load) SKIP_LOAD=true; shift ;;
            --no-restart) NO_RESTART=true; shift ;;
            --keep-redis) KEEP_REDIS=true; shift ;;
            --noop-migration) NOOP_MIGRATION=true; shift ;;
            --high-pressure) HIGH_PRESSURE=true; shift ;;
            --vanilla) VANILLA_MODE=true; shift ;;
            --pressure-mode) PRESSURE_MODE="$2"; shift 2 ;;
            --pressure-key-count) PRESSURE_KEY_COUNT="$2"; shift 2 ;;
            --pressure-repeat) PRESSURE_REPEAT="$2"; shift 2 ;;
            --pressure-interval) PRESSURE_INTERVAL_SEC="$2"; shift 2 ;;
            --pressure-scan-count) PRESSURE_SCAN_COUNT="$2"; shift 2 ;;
            --process-nodes) PROCESS_NUMA_NODES="$2"; shift 2 ;;
            --host) REDIS_HOST="$2"; shift 2 ;;
            --help|-h) usage ;;
            *) log_err "Unknown argument: $1"; usage ;;
        esac
    done
}

slot_id() {
    if [[ "$ENABLE_TINYLFU" == true ]]; then
        echo 2
    else
        echo 1
    fi
}

check_prerequisites() {
    log_step "Prerequisites"
    if [[ "$VANILLA_MODE" == true ]]; then
        local vanilla_server="${VANILLA_REDIS_ROOT}/src/redis-server"
        local vanilla_cli="${VANILLA_REDIS_ROOT}/src/redis-cli"
        [[ -x "$vanilla_server" ]] || { log_err "vanilla redis-server not found: $vanilla_server"; exit 1; }
        [[ -x "$vanilla_cli" ]] || { log_err "vanilla redis-cli not found: $vanilla_cli"; exit 1; }
        REDIS_SERVER="$vanilla_server"
        REDIS_CLI="$vanilla_cli"
        log_ok "vanilla redis-server: $REDIS_SERVER"
    else
        [[ -x "$REDIS_SERVER" ]] || { log_err "redis-server not found: $REDIS_SERVER"; exit 1; }
        [[ -x "$REDIS_CLI" ]] || { log_err "redis-cli not found: $REDIS_CLI"; exit 1; }
        log_ok "redis-server: $REDIS_SERVER"
    fi
    [[ -x "$YCSB_BIN" ]] || { log_err "YCSB not found: $YCSB_BIN"; exit 1; }
    [[ -f "$WORKLOAD" ]] || { log_err "workload not found: $WORKLOAD"; exit 1; }
    [[ -f "$VISUALIZE_SCRIPT" ]] || log_warn "visualizer not found: $VISUALIZE_SCRIPT"
    command -v awk >/dev/null || { log_err "awk is required"; exit 1; }
    log_ok "YCSB: $YCSB_BIN"
}

save_system_info() {
    local sysinfo="$OUTPUT_DIR/system_info.txt"
    {
        echo "Redis-NUMA AE scheduler latency disturbance benchmark"
        echo "====================================================="
        echo "time: $(date)"
        echo "host: $(hostname)"
        echo "redis_port: $REDIS_PORT"
        echo "scheduler_mode: $SCHEDULER_MODE"
        echo "strategy: $([[ "$ENABLE_TINYLFU" == true ]] && echo TinyLFU || echo 'Composite LRU')"
        echo "slot: $(slot_id)"
        echo "pressure_mode: $PRESSURE_MODE"
        echo "maxmemory: $MAX_MEMORY"
        echo "record_count: $RECORD_COUNT"
        echo "field_length: $FIELD_LENGTH"
        echo "threads: $THREADS"
        echo "windows: $WINDOWS"
        echo "window_ops: $WINDOW_OPS"
        echo "read_proportion: $READ_PROPORTION"
        echo "update_proportion: $UPDATE_PROPORTION"
        echo "hotspot_data_fraction: $HOTSPOT_DATA_FRACTION"
        echo "hotspot_opn_fraction: $HOTSPOT_OPN_FRACTION"
        echo "remote_reset: $REMOTE_RESET"
        echo ""
        echo "=== Kernel ==="
        uname -a
        echo ""
        echo "=== Memory ==="
        free -h
        echo ""
        echo "=== NUMA topology ==="
        numactl --hardware 2>/dev/null || echo "numactl not available"
        echo ""
        echo "=== Redis ==="
        if [[ "$VANILLA_MODE" == true ]]; then
            echo "MODE: vanilla (numactl --membind=0, local memory only)"
        fi
        "$REDIS_SERVER" --version
        echo ""
        echo "=== YCSB ==="
        "$YCSB_BIN" --version 2>&1 | head -3 || true
    } > "$sysinfo"
    log_ok "system info: $sysinfo"
}

start_redis() {
    log_step "Start Redis"
    if [[ $EUID -eq 0 ]]; then
        echo 0 > /proc/sys/kernel/numa_balancing 2>/dev/null || true
        echo never > /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null || true
        echo never > /sys/kernel/mm/transparent_hugepage/defrag 2>/dev/null || true
    else
        log_warn "not root; cannot disable NUMA balancing or THP"
    fi

    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE >/dev/null 2>&1 || true
    sleep 1
    pkill -f "redis-server.*:${REDIS_PORT}" >/dev/null 2>&1 || true
    sleep 1

    local -a numa_cmd=()
    if [[ "$VANILLA_MODE" == true ]]; then
        if command -v numactl >/dev/null 2>&1; then
            numa_cmd=(numactl --membind=0)
            log "vanilla mode: binding memory to local NUMA node 0 only"
        else
            log_warn "numactl not available; vanilla Redis will use OS default memory allocation"
        fi
    else
        if command -v numactl >/dev/null 2>&1 && [[ "$PROCESS_NUMA_NODES" != "all" ]]; then
            numa_cmd=(numactl --cpunodebind="$PROCESS_NUMA_NODES" --membind="$PROCESS_NUMA_NODES")
            log "binding Redis-NUMA CPU/memory to NUMA nodes: $PROCESS_NUMA_NODES"
        fi
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
            log_ok "Redis is ready"
            return
        fi
        retries=$((retries - 1))
        sleep 1
    done
    log_err "Redis failed to start"
    exit 1
}

redis_cmd() {
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" "$@"
}

discover_hot_keys() {
    HOT_KEYS_FILE="$OUTPUT_DIR/hot_keys.txt"
    log "discovering YCSB keys for access pressure"
    "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" --scan 2>/dev/null \
        | head -n "$PRESSURE_KEY_COUNT" > "$HOT_KEYS_FILE" || true
    local count
    count="$(wc -l < "$HOT_KEYS_FILE" 2>/dev/null || echo 0)"
    if [[ "${count:-0}" -eq 0 ]]; then
        log_warn "redis-cli --scan found no keys; falling back to user0..userN"
        : > "$HOT_KEYS_FILE"
        for ((i = 0; i < PRESSURE_KEY_COUNT; i++)); do
            echo "user${i}" >> "$HOT_KEYS_FILE"
        done
        count="$PRESSURE_KEY_COUNT"
    fi
    log_ok "pressure key set: $HOT_KEYS_FILE (${count} keys)"
}

init_numa() {
    if [[ "$VANILLA_MODE" == true ]]; then
        log_step "Vanilla mode — skipping NUMA initialization"
        return
    fi
    log_step "Initialize NUMA strategy"
    local config_file="$PROJECT_ROOT/composite_lru.json"
    [[ -f "$config_file" ]] && redis_cmd NUMA CONFIG LOAD "$config_file" >/dev/null 2>&1 || true
    redis_cmd NUMA CONFIG SET locality_stats 1 >/dev/null 2>&1 || true
    redis_cmd NUMA CONFIG SET access_tracking 1 >/dev/null 2>&1 || true
    redis_cmd NUMA CONFIG SET enabled_nodes 0,2 >/dev/null 2>&1 || true
    redis_cmd NUMA CONFIG SET strategy local_first >/dev/null 2>&1 || true

    if [[ "$NOOP_MIGRATION" == true ]]; then
        log "noop migration mode: disabling all migration strategies"
        redis_cmd NUMA CONFIG SET auto_migrate_enabled 0 >/dev/null 2>&1 || true
        redis_cmd NUMA STRATEGY SLOT DISABLE 1 >/dev/null 2>&1 || true
        redis_cmd NUMA STRATEGY SLOT DISABLE 2 >/dev/null 2>&1 || true
    elif [[ "$ENABLE_TINYLFU" == true ]]; then
        redis_cmd NUMA CONFIG SET auto_migrate_enabled 1 >/dev/null 2>&1 || true
        redis_cmd NUMA STRATEGY SLOT DISABLE 1 >/dev/null 2>&1 || true
        redis_cmd NUMA STRATEGY SLOT ENABLE 2 >/dev/null 2>&1 || true
    else
        redis_cmd NUMA CONFIG SET auto_migrate_enabled 1 >/dev/null 2>&1 || true
        redis_cmd NUMA STRATEGY SLOT ENABLE 1 >/dev/null 2>&1 || true
        redis_cmd NUMA STRATEGY SLOT DISABLE 2 >/dev/null 2>&1 || true
    fi

    local slot
    if [[ "$NOOP_MIGRATION" == true ]]; then
        slot=0
    else
        slot="$(slot_id)"
    fi
    if [[ "$SCHEDULER_MODE" == "ae" ]]; then
        redis_cmd NUMA STRATEGY SLOT SCHEDULE "$slot" ae >/dev/null 2>&1 || true
    elif [[ "$SCHEDULER_MODE" == "servercron" ]]; then
        redis_cmd NUMA STRATEGY SLOT SCHEDULE "$slot" servercron >/dev/null 2>&1 || true
    else
        log_err "invalid scheduler mode: $SCHEDULER_MODE"
        exit 1
    fi

    redis_cmd NUMA STRATEGY LIST > "$OUTPUT_DIR/strategy_list_initial.txt" 2>&1 || true
    redis_cmd NUMA STRATEGY SLOT STATUS "$slot" > "$OUTPUT_DIR/slot_status_initial.txt" 2>&1 || true
    log_ok "slot $slot scheduled via $SCHEDULER_MODE"

    if [[ "$HIGH_PRESSURE" == true && "$NOOP_MIGRATION" != true ]]; then
        log "high pressure mode: boosting migration rate and sensitivity"
        redis_cmd NUMA CONFIG SET migration_rate_multiplier 20 >/dev/null 2>&1 || true
        redis_cmd NUMA CONFIG SET scan_batch_size 10000 >/dev/null 2>&1 || true
        redis_cmd NUMA CONFIG SET hot_candidates_size 2048 >/dev/null 2>&1 || true
        redis_cmd NUMA CONFIG SET migrate_hotness_threshold 1 >/dev/null 2>&1 || true
        PRESSURE_REPEAT=5
    fi
}

run_load() {
    log_step "Load dataset: ${RECORD_COUNT} records x ${FIELD_LENGTH}B"
    if [[ "$VANILLA_MODE" != true ]]; then
        redis_cmd NUMA CONFIG SET enabled_nodes 2 >/dev/null 2>&1 || true
        redis_cmd NUMA CONFIG SET strategy cxl_optimized >/dev/null 2>&1 || true
    fi

    "$YCSB_BIN" load redis -s \
        -P "$WORKLOAD" \
        -p "recordcount=$RECORD_COUNT" \
        -p "fieldcount=$FIELD_COUNT" \
        -p "fieldlength=$FIELD_LENGTH" \
        -p "fieldlengthdistribution=constant" \
        -p "redis.host=$REDIS_HOST" \
        -p "redis.port=$REDIS_PORT" \
        -p "redis.timeout=$YCSB_TIMEOUT_MS" \
        -p "threadcount=$LOAD_THREADS" \
        2>&1 | tee "$OUTPUT_DIR/load.txt"

    if [[ "$VANILLA_MODE" != true ]]; then
        redis_cmd NUMA CONFIG SET enabled_nodes 0,2 >/dev/null 2>&1 || true
        redis_cmd NUMA CONFIG SET strategy local_first >/dev/null 2>&1 || true
    fi

    if [[ "$REMOTE_RESET" == true ]]; then
        log "forcing initial dataset to remote node 2 via NUMA MIGRATE DB 2"
        redis_cmd NUMA MIGRATE DB 2 > "$OUTPUT_DIR/remote_reset_migrate_db.txt" 2>&1 || true
    fi

    redis_cmd NUMA CONFIG STATS > "$OUTPUT_DIR/config_stats_after_load.txt" 2>&1 || true
    redis_cmd NUMA MIGRATE STATS > "$OUTPUT_DIR/migrate_stats_after_load.txt" 2>&1 || true
    redis_cmd INFO memory > "$OUTPUT_DIR/info_memory_after_load.txt" 2>&1 || true
    discover_hot_keys
    log_ok "load complete"
}

extract_metric() {
    local file="$1"
    local op="$2"
    local metric="$3"
    awk -F, -v op="[$op]" -v metric="$metric" '
        $1 == op && $2 ~ metric {gsub(/^[ \t]+|[ \t]+$/, "", $3); print $3; exit}
    ' "$file"
}

extract_inline_metric() {
    local file="$1"
    local op="$2"
    local key="$3"
    awk -v op="[$op]" -v key="$key" '
        $0 ~ op {
            match($0, key "=([0-9]+)", arr)
            if (arr[1] != "") {print arr[1]; exit}
        }
    ' "$file"
}

raw_stat() {
    local stats="$1"
    local key="$2"
    awk -v k="$key" '$0 == k {getline; print; exit}' <<< "$stats"
}

collect_migrate_total() {
    local stats total
    stats="$(redis_cmd --raw NUMA MIGRATE STATS 2>/dev/null || true)"
    total="$(raw_stat "$stats" "total_migrations")"
    echo "${total:-0}"
}

collect_strategy_counter() {
    local key="$1"
    local stats value
    stats="$(redis_cmd --raw NUMA CONFIG GET 2>/dev/null || true)"
    value="$(raw_stat "$stats" "$key")"
    if [[ -z "${value:-}" || "$value" == "unavailable" ]]; then
        echo "0"
    else
        echo "$value"
    fi
}

collect_strategy_metrics() {
    local stats heat triggered candidates scanned
    stats="$(redis_cmd --raw NUMA CONFIG GET 2>/dev/null || true)"
    heat="$(raw_stat "$stats" "composite_heat_updates")"
    triggered="$(raw_stat "$stats" "composite_migrations_triggered")"
    candidates="$(raw_stat "$stats" "composite_candidates_written")"
    scanned="$(raw_stat "$stats" "composite_scan_keys_checked")"
    for value_name in heat triggered candidates scanned; do
        if [[ -z "${!value_name:-}" || "${!value_name}" == "unavailable" ]]; then
            printf -v "$value_name" "0"
        fi
    done
    echo "${heat},${triggered},${candidates},${scanned}"
}

collect_remote_pct() {
    local stats tlfu_on acc_local acc_remote total
    stats="$(redis_cmd --raw NUMA MIGRATE STATS 2>/dev/null || true)"
    tlfu_on="$(raw_stat "$stats" "tinylfu_enabled")"
    if [[ "${tlfu_on:-0}" -eq 1 ]]; then
        acc_local="$(raw_stat "$stats" "tinylfu_accesses_local")"
        acc_remote="$(raw_stat "$stats" "tinylfu_accesses_remote")"
    else
        acc_local="$(raw_stat "$stats" "accesses_local")"
        acc_remote="$(raw_stat "$stats" "accesses_remote")"
    fi
    acc_local=${acc_local:-0}; acc_remote=${acc_remote:-0}
    total=$((acc_local + acc_remote))
    if [[ "$total" -gt 0 ]]; then
        awk -v r="$acc_remote" -v t="$total" 'BEGIN {printf "%.2f", r*100/t}'
    else
        echo "0"
    fi
}

collect_numa_live_memory_metrics() {
    local stats node0 node2 local_bytes remote_bytes
    stats="$(redis_cmd --raw NUMA CONFIG STATS 2>/dev/null || true)"
    node0="$(raw_stat "$stats" "node0_live")"
    node2="$(raw_stat "$stats" "node2_live")"
    node0=${node0:-0}; node2=${node2:-0}
    local_bytes="$node0"
    remote_bytes="$node2"
    awk -v l="$local_bytes" -v r="$remote_bytes" -v n0="$node0" -v n2="$node2" \
        'BEGIN {printf "%.1f,%.1f,%.1f,%.1f", l/1048576, r/1048576, n0/1048576, n2/1048576}'
}

collect_slot_metrics() {
    local status max_time timeouts
    status="$(redis_cmd NUMA STRATEGY SLOT STATUS "$(slot_id)" 2>/dev/null || true)"
    max_time="$(awk -F': ' '/Max time:/ {gsub(/ us/, "", $2); print $2; exit}' <<< "$status")"
    timeouts="$(awk -F': ' '/Timeouts:/ {print $2; exit}' <<< "$status")"
    echo "${max_time:-0},${timeouts:-0}"
}

run_pressure_access() {
    local stop_file="$1"
    local log_file="$OUTPUT_DIR/pressure_access.log"
    [[ -n "$HOT_KEYS_FILE" && -f "$HOT_KEYS_FILE" ]] || discover_hot_keys
    mapfile -t pressure_keys < "$HOT_KEYS_FILE"
    if [[ "${#pressure_keys[@]}" -eq 0 ]]; then
        log_warn "empty pressure key set; access pressure disabled"
        return
    fi
    log "starting access pressure injector: keys=$PRESSURE_KEY_COUNT repeat=$PRESSURE_REPEAT interval=${PRESSURE_INTERVAL_SEC}s"
    while [[ ! -f "$stop_file" ]]; do
        local round_start
        round_start="$(date +%s)"
        for ((r = 0; r < PRESSURE_REPEAT; r++)); do
            for key in "${pressure_keys[@]}"; do
                redis_cmd HGETALL "$key" >/dev/null 2>&1 || true
            done
        done
        echo "$(date '+%F %T') access keys=${#pressure_keys[@]} repeat=$PRESSURE_REPEAT" >> "$log_file"
        local elapsed=$(( $(date +%s) - round_start ))
        local sleep_for=$(( PRESSURE_INTERVAL_SEC - elapsed ))
        [[ "$sleep_for" -gt 0 ]] && sleep "$sleep_for" || sleep 0.1
    done
}

run_pressure_scan() {
    local stop_file="$1"
    local log_file="$OUTPUT_DIR/pressure_scan.log"
    log_warn "pressure-mode=scan uses synchronous NUMA MIGRATE SCAN; treat as blocking-command stress, not pure AE scheduler evidence"
    while [[ ! -f "$stop_file" ]]; do
        redis_cmd NUMA MIGRATE SCAN COUNT "$PRESSURE_SCAN_COUNT" >> "$log_file" 2>&1 || true
        sleep "$PRESSURE_INTERVAL_SEC"
    done
}

run_pressure_db() {
    local stop_file="$1"
    local log_file="$OUTPUT_DIR/pressure_db.log"
    log_warn "pressure-mode=db uses synchronous NUMA MIGRATE DB; treat as blocking-command stress, not pure AE scheduler evidence"
    IFS=',' read -ra targets <<< "$PRESSURE_DB_TARGETS"
    local idx=0
    while [[ ! -f "$stop_file" ]]; do
        local target="${targets[$((idx % ${#targets[@]}))]}"
        redis_cmd NUMA MIGRATE DB "$target" >> "$log_file" 2>&1 || true
        idx=$((idx + 1))
        sleep "$PRESSURE_INTERVAL_SEC"
    done
}

start_pressure() {
    local stop_file="$OUTPUT_DIR/.pressure_stop"
    rm -f "$stop_file"
    if [[ "$PRESSURE_MODE" != "none" && -x "$TRIGGER_SCRIPT" ]]; then
        [[ -n "$HOT_KEYS_FILE" && -f "$HOT_KEYS_FILE" ]] || discover_hot_keys
        "$TRIGGER_SCRIPT" \
            --host "$REDIS_HOST" \
            --port "$REDIS_PORT" \
            --mode "$PRESSURE_MODE" \
            --key-file "$HOT_KEYS_FILE" \
            --key-count "$PRESSURE_KEY_COUNT" \
            --repeat "$PRESSURE_REPEAT" \
            --interval "$PRESSURE_INTERVAL_SEC" \
            --scan-count "$PRESSURE_SCAN_COUNT" \
            --db-targets "$PRESSURE_DB_TARGETS" \
            --stop-file "$stop_file" \
            --log-file "$OUTPUT_DIR/pressure_${PRESSURE_MODE}.log" \
            --redis-cli "$REDIS_CLI" &
        PRESSURE_PID=$!
        log "external pressure trigger PID: $PRESSURE_PID"
        return
    fi
    case "$PRESSURE_MODE" in
        access) run_pressure_access "$stop_file" & PRESSURE_PID=$! ;;
        scan) run_pressure_scan "$stop_file" & PRESSURE_PID=$! ;;
        db) run_pressure_db "$stop_file" & PRESSURE_PID=$! ;;
        none) PRESSURE_PID="" ;;
        *) log_err "invalid pressure mode: $PRESSURE_MODE"; exit 1 ;;
    esac
    [[ -n "$PRESSURE_PID" ]] && log "pressure injector PID: $PRESSURE_PID"
}

stop_pressure() {
    touch "$OUTPUT_DIR/.pressure_stop" 2>/dev/null || true
    if [[ -n "${PRESSURE_PID:-}" ]]; then
        wait "$PRESSURE_PID" 2>/dev/null || true
        PRESSURE_PID=""
    fi
}

run_window() {
    local window="$1"
    local out="$OUTPUT_DIR/window_${window}.txt"
    local before after delta throughput read_avg read_p95 read_p99 update_avg update_p99 remote_pct numa_mem slot_metrics
    local strategy_before strategy_after strategy_delta strategy_rate strategy_metrics
    local start_ms end_ms elapsed_sec

    if [[ "$VANILLA_MODE" != true ]]; then
        before="$(collect_migrate_total)"
        strategy_before="$(collect_strategy_counter composite_migrations_triggered)"
    else
        before=0; strategy_before=0
    fi
    start_ms="$(date +%s%3N)"
    "$YCSB_BIN" run redis -s \
        -P "$WORKLOAD" \
        -p "recordcount=$RECORD_COUNT" \
        -p "operationcount=$WINDOW_OPS" \
        -p "threadcount=$THREADS" \
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
    end_ms="$(date +%s%3N)"
    if [[ "$VANILLA_MODE" != true ]]; then
        after="$(collect_migrate_total)"
        strategy_after="$(collect_strategy_counter composite_migrations_triggered)"
    else
        after=0; strategy_after=0
    fi

    elapsed_sec="$(awk -v s="$start_ms" -v e="$end_ms" 'BEGIN {printf "%.3f", (e-s)/1000.0}')"
    delta=$(( ${after:-0} - ${before:-0} ))
    [[ "$delta" -lt 0 ]] 2>/dev/null && delta=0
    strategy_delta=$(( ${strategy_after:-0} - ${strategy_before:-0} ))
    [[ "$strategy_delta" -lt 0 ]] 2>/dev/null && strategy_delta=0

    throughput="$(extract_metric "$out" "OVERALL" "Throughput" || true)"
    read_avg="$(extract_metric "$out" "READ" "AverageLatency" || true)"
    read_p95="$(extract_metric "$out" "READ" "95thPercentileLatency" || true)"
    read_p99="$(extract_metric "$out" "READ" "99thPercentileLatency" || true)"
    read_p999="$(extract_inline_metric "$out" "READ" "99.9" || true)"
    read_p9999="$(extract_inline_metric "$out" "READ" "99.99" || true)"
    update_avg="$(extract_metric "$out" "UPDATE" "AverageLatency" || true)"
    update_p99="$(extract_metric "$out" "UPDATE" "99thPercentileLatency" || true)"
    update_p999="$(extract_inline_metric "$out" "UPDATE" "99.9" || true)"
    throughput=${throughput:-0}; read_avg=${read_avg:-0}; read_p95=${read_p95:-0}; read_p99=${read_p99:-0}
    read_p999=${read_p999:-0}; read_p9999=${read_p9999:-0}
    update_avg=${update_avg:-0}; update_p99=${update_p99:-0}; update_p999=${update_p999:-0}

    if [[ "$VANILLA_MODE" != true ]]; then
        remote_pct="$(collect_remote_pct)"
        numa_mem="$(collect_numa_live_memory_metrics)"
        slot_metrics="$(collect_slot_metrics)"
        local migrate_per_sec
        migrate_per_sec="$(awk -v d="$delta" -v e="$elapsed_sec" 'BEGIN {if (e>0) printf "%.2f", d/e; else print "0"}')"
        strategy_rate="$(awk -v d="$strategy_delta" -v e="$elapsed_sec" 'BEGIN {if (e>0) printf "%.2f", d/e; else print "0"}')"
        strategy_metrics="$(collect_strategy_metrics)"
    else
        remote_pct="0"
        numa_mem="0,0,0,0"
        slot_metrics="0,0"
        migrate_per_sec="0"
        strategy_rate="0"
        strategy_metrics="0,0,0,0"
    fi

    echo "${window},${elapsed_sec},${throughput},${read_avg},${read_p95},${read_p99},${read_p999},${read_p9999},${update_avg},${update_p99},${update_p999},${before:-0},${after:-0},${delta},${migrate_per_sec},${strategy_before:-0},${strategy_after:-0},${strategy_delta},${strategy_rate},${remote_pct},${numa_mem},${slot_metrics},${strategy_metrics}" >> "$SUMMARY_CSV"
    if [[ "$VANILLA_MODE" != true ]]; then
        redis_cmd NUMA STRATEGY SLOT STATUS "$(slot_id)" > "$OUTPUT_DIR/slot_status_window_${window}.txt" 2>&1 || true
    fi
    log_ok "window=${window}, p99=${read_p99} us, throughput=${throughput} ops/s, migrations=${delta}, strategy_migrations=${strategy_delta}"
}

generate_report() {
    log_step "Generate plots"
    if ! command -v python3 >/dev/null 2>&1; then
        log_warn "python3 not available; skip plotting"
        return
    fi
    local venv_dir="$SCRIPT_DIR/scripts/.venv"
    local python="python3"
    if [[ -x "$venv_dir/bin/python" ]]; then
        python="$venv_dir/bin/python"
    fi
    "$python" "$VISUALIZE_SCRIPT" \
        --input "$SUMMARY_CSV" \
        --label "Redis-NUMA ($([[ "$ENABLE_TINYLFU" == true ]] && echo TinyLFU || echo 'Composite LRU'), $SCHEDULER_MODE)" \
        --output "$OUTPUT_DIR/ae_scheduler_latency_compare.png" \
        2>&1 || log_warn "plotting failed"
}

print_summary() {
    log_step "Summary"
    column -s, -t "$SUMMARY_CSV" 2>/dev/null || cat "$SUMMARY_CSV"
    echo ""
    echo "Output directory: $OUTPUT_DIR"
}

cleanup() {
    local exit_code=$?
    stop_pressure
    rm -f "$SAFE_LINK" "$OUTPUT_DIR/.pressure_stop" 2>/dev/null || true
    if [[ "$KEEP_REDIS" == false && "$NO_RESTART" == false ]]; then
        "$REDIS_CLI" -h "$REDIS_HOST" -p "$REDIS_PORT" SHUTDOWN NOSAVE >/dev/null 2>&1 || true
    fi
    if [[ "$VANILLA_MODE" != true && $EUID -eq 0 ]]; then
        echo 1 > /proc/sys/kernel/numa_balancing 2>/dev/null || true
    fi
    exit "$exit_code"
}

main() {
    parse_args "$@"
    if [[ -z "$OUTPUT_DIR" ]]; then
        local tag="$([[ "$ENABLE_TINYLFU" == true ]] && echo tinylfu || echo composite)_${SCHEDULER_MODE}"
        OUTPUT_DIR="$SCRIPT_DIR/results/ae_scheduler_latency_${tag}_$(date +%Y%m%d_%H%M%S)"
    fi
    mkdir -p "$OUTPUT_DIR"
    SUMMARY_CSV="$OUTPUT_DIR/ae_scheduler_latency_summary.csv"

    echo -e "${BOLD}${CYAN}"
    echo "╔════════════════════════════════════════════════════╗"
    echo "║      AE Scheduler Latency Disturbance Benchmark    ║"
    echo "╚════════════════════════════════════════════════════╝"
    echo -e "${NC}"
    log "output: $OUTPUT_DIR"

    trap cleanup EXIT
    check_prerequisites
    save_system_info
    if [[ "$NO_RESTART" == false ]]; then
        start_redis
    else
        [[ "$(redis_cmd PING 2>/dev/null || true)" == *PONG* ]] || { log_err "Redis not responding"; exit 1; }
        log_ok "Redis connection ok"
    fi
    init_numa
    if [[ "$SKIP_LOAD" == false ]]; then
        run_load
    else
        log_warn "skip load"
        discover_hot_keys
    fi

    echo "window,elapsed_sec,throughput_ops_sec,read_avg_us,read_p95_us,read_p99_us,read_p999_us,read_p9999_us,update_avg_us,update_p99_us,update_p999_us,migrate_before,migrate_after,migrate_delta,migrate_per_sec,strategy_migrate_before,strategy_migrate_after,strategy_migrate_delta,strategy_migrate_per_sec,remote_pct,numa_local_live_mb,numa_remote_live_mb,numa_node0_live_mb,numa_node2_live_mb,slot_max_time_us,slot_timeouts,composite_heat_updates,composite_migrations_triggered,composite_candidates_written,composite_scan_keys_checked" > "$SUMMARY_CSV"

    start_pressure
    for ((w = 1; w <= WINDOWS; w++)); do
        log_step "Window $w/$WINDOWS"
        run_window "$w"
    done
    stop_pressure

    if [[ "$VANILLA_MODE" != true ]]; then
        redis_cmd NUMA MIGRATE STATS > "$OUTPUT_DIR/migrate_stats_final.txt" 2>&1 || true
        redis_cmd NUMA CONFIG STATS > "$OUTPUT_DIR/config_stats_final.txt" 2>&1 || true
        redis_cmd NUMA STRATEGY SLOT STATUS "$(slot_id)" > "$OUTPUT_DIR/slot_status_final.txt" 2>&1 || true
    fi
    redis_cmd INFO memory > "$OUTPUT_DIR/info_memory_final.txt" 2>&1 || true

    generate_report
    print_summary
    log_ok "AE scheduler latency benchmark complete"
}

main "$@"
