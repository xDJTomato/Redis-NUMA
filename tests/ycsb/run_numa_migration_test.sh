#!/bin/bash
# ============================================================================
# NUMA 迁移触发测试脚本
# 
# 用途: 在双路 NUMA 服务器上触发 Redis Key 的跨节点迁移
# 环境: QEMU CXL 虚拟机或真实 NUMA 硬件
# 前提: 
#   1. 已编译 Redis (make -j$(nproc))
#   2. 已安装 YCSB (tests/ycsb/scripts/install_ycsb.sh)
#   3. NUMA 环境正常 (numactl --hardware 显示 2+ 节点)
#
# 用法: ./run_numa_migration_test.sh [选项]
# 选项:
#   --mode <quick|full>   测试模式 (默认: full)
#   --port PORT           Redis 端口 (默认: 6379)
#   --threshold N         迁移热度阈值 (1-7, 默认: 3)
#   --help                显示帮助
# ============================================================================

set -euo pipefail

# ── 路径定义 ────────────────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
YCSB_BIN="$SCRIPT_DIR/ycsb-0.17.0/bin/ycsb"
WORKLOADS_DIR="$SCRIPT_DIR/workloads"
REDIS_SERVER="$ROOT_DIR/src/redis-server"
REDIS_CLI="$ROOT_DIR/src/redis-cli"

# ── 默认参数 ────────────────────────────────────────────────────────────────
MODE="full"
REDIS_PORT=6379
MIGRATION_THRESHOLD=3
RECORD_COUNT=500000
OPERATION_COUNT=2000000
THREAD_COUNT=16

# ── 颜色输出 ────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()   { echo -e "${RED}[ERR]${NC}   $*"; }
log_step()  { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

# ── 帮助信息 ────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --mode <quick|full>   测试模式
                        quick: 10万条记录，快速验证
                        full:  50万条记录，完整测试 (默认)
  --port PORT           Redis 端口 (默认: 6379)
  --threshold N         迁移热度阈值 1-7 (默认: 3)
  --help                显示此帮助

示例:
  $(basename "$0") --mode quick
  $(basename "$0") --mode full --threshold 4
  $(basename "$0") --port 6380
EOF
    exit 0
}

# ── 参数解析 ────────────────────────────────────────────────────────────────
parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --mode)
                MODE="$2"
                shift 2
                ;;
            --port)
                REDIS_PORT="$2"
                shift 2
                ;;
            --threshold)
                MIGRATION_THRESHOLD="$2"
                shift 2
                ;;
            --help|-h)
                usage
                ;;
            *)
                log_err "未知参数: $1"
                usage
                ;;
        esac
    done

    if [[ "$MODE" == "quick" ]]; then
        RECORD_COUNT=100000
        OPERATION_COUNT=500000
        THREAD_COUNT=8
    fi
}

# ── 前置检查 ────────────────────────────────────────────────────────────────
check_deps() {
    log_step "前置检查"

    if [[ ! -x "$REDIS_SERVER" ]]; then
        log_err "redis-server 未找到: $REDIS_SERVER"
        log_info "请先编译: make -C $ROOT_DIR"
        exit 1
    fi
    log_ok "redis-server: $REDIS_SERVER"

    if [[ ! -x "$YCSB_BIN" ]]; then
        log_err "YCSB 未找到: $YCSB_BIN"
        log_info "请先安装: $SCRIPT_DIR/scripts/install_ycsb.sh"
        exit 1
    fi
    log_ok "YCSB: $YCSB_BIN"

    local workload="$WORKLOADS_DIR/workload_numa_migration"
    if [[ ! -f "$workload" ]]; then
        log_err "工作负载文件不存在: $workload"
        exit 1
    fi
    log_ok "工作负载: $workload"

    # 检查 NUMA 环境
    if command -v numactl &>/dev/null; then
        local num_nodes
        num_nodes=$(numactl --hardware | grep "available:" | awk '{print $2}')
        log_ok "NUMA 节点数: $num_nodes"
        
        if [[ "$num_nodes" -lt 2 ]]; then
            log_warn "NUMA 节点数 < 2，迁移效果可能不明显"
        fi
    else
        log_warn "numactl 未安装，无法绑定 NUMA 节点"
    fi
}

# ── 启动 Redis ─────────────────────────────────────────────────────────────
start_redis() {
    log_step "步骤 1: 启动 Redis"

    log_info "停止已有 Redis 实例..."
    pkill -9 -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
    sleep 2

    log_info "启动 Redis (port=$REDIS_PORT, maxmemory=16gb)..."
    "$REDIS_SERVER" \
        --daemonize yes \
        --port "$REDIS_PORT" \
        --bind 127.0.0.1 \
        --loglevel verbose \
        --save "" \
        --appendonly no \
        --maxmemory 16gb \
        --maxmemory-policy allkeys-lru

    # 等待就绪
    for i in {1..10}; do
        if "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" ping 2>/dev/null | grep -q "PONG"; then
            log_ok "Redis 启动成功"
            return 0
        fi
        sleep 1
    done

    log_err "Redis 启动超时"
    exit 1
}

# ── 配置 NUMA 策略 ─────────────────────────────────────────────────────────
config_numa() {
    log_step "步骤 2: 配置 NUMA 策略"

    # 设置为交错分配，确保数据初始分布在两个节点
    log_info "设置分配策略为 interleaved"
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA CONFIG SET strategy interleaved || {
        log_warn "NUMA CONFIG 命令失败，可能模块未加载"
    }
    sleep 1

    # 查看当前配置
    log_info "当前 NUMA 配置:"
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA CONFIG GET 2>/dev/null || true

    # 创建复合 LRU 配置文件 (降低阈值，更容易触发迁移)
    local config_file="/tmp/composite_lru_migration.json"
    cat > "$config_file" <<EOF
{
    "migrate_hotness_threshold": $MIGRATION_THRESHOLD,
    "hot_candidates_size": 512,
    "scan_batch_size": 500,
    "decay_threshold_sec": 5,
    "auto_migrate_enabled": 1,
    "overload_threshold": 0.8,
    "bandwidth_threshold": 0.9,
    "pressure_threshold": 0.7,
    "stability_count": 2
}
EOF

    log_info "加载复合 LRU 配置 (阈值=$MIGRATION_THRESHOLD)"
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA CONFIG LOAD "$config_file" 2>/dev/null || {
        log_warn "CONFIG LOAD 失败，尝试手动配置"
        "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA STRATEGY SLOT 1 composite-lru 2>/dev/null || true
    }
}

# ── 加载数据 ───────────────────────────────────────────────────────────────
load_data() {
    log_step "步骤 3: 加载数据 ($RECORD_COUNT 条，交错分布)"

    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" FLUSHALL
    log_ok "已清空数据"

    log_info "开始 YCSB Load 阶段..."
    "$YCSB_BIN" load redis \
        -P "$WORKLOADS_DIR/workload_numa_migration" \
        -p recordcount="$RECORD_COUNT" \
        -p threadcount="$THREAD_COUNT" \
        -p redis.host=127.0.0.1 \
        -p redis.port="$REDIS_PORT" \
        -s 2>&1 | tee /tmp/ycsb_load.txt

    log_ok "数据加载完成"

    # 查看初始内存分布
    log_info "初始 NUMA 分配统计:"
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA CONFIG STATS 2>/dev/null || true
}

# ── 热点访问测试 ───────────────────────────────────────────────────────────
run_hotspot_access() {
    log_step "步骤 4: 热点访问测试 (触发迁移)"

    # 重置迁移统计
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA MIGRATE RESET 2>/dev/null || true

    if command -v numactl &>/dev/null; then
        log_info "使用 numactl 绑定线程到不同 NUMA 节点"
        
        # 获取 NUMA 节点信息
        local num_nodes
        num_nodes=$(numactl --hardware | grep "available:" | awk '{print $2}')
        
        if [[ "$num_nodes" -ge 2 ]]; then
            # 双节点模式: 分别在两个节点上运行
            log_info "在 Node 0 上启动线程组 A (8 threads)..."
            numactl --cpunodebind=0 --membind=0 \
                "$YCSB_BIN" run redis \
                    -P "$WORKLOADS_DIR/workload_numa_migration" \
                    -p recordcount="$RECORD_COUNT" \
                    -p operationcount=$((OPERATION_COUNT / 2)) \
                    -p threadcount=8 \
                    -p redis.host=127.0.0.1 \
                    -p redis.port="$REDIS_PORT" \
                    -p requestdistribution=zipfian \
                    -p zipfianconstant=0.99 \
                    -s 2>&1 | tee /tmp/ycsb_node0.txt &
            local pid_a=$!

            log_info "在 Node 1 上启动线程组 B (8 threads)..."
            numactl --cpunodebind=1 --membind=1 \
                "$YCSB_BIN" run redis \
                    -P "$WORKLOADS_DIR/workload_numa_migration" \
                    -p recordcount="$RECORD_COUNT" \
                    -p operationcount=$((OPERATION_COUNT / 2)) \
                    -p threadcount=8 \
                    -p redis.host=127.0.0.1 \
                    -p redis.port="$REDIS_PORT" \
                    -p requestdistribution=zipfian \
                    -p zipfianconstant=0.99 \
                    -s 2>&1 | tee /tmp/ycsb_node1.txt &
            local pid_b=$!

            log_info "等待两个线程组完成..."
            wait $pid_a $pid_b
        else
            log_warn "NUMA 节点数不足，使用单节点模式"
            run_single_node_test
        fi
    else
        log_warn "numactl 未安装，使用普通模式"
        run_single_node_test
    fi

    log_ok "热点访问完成"
}

# 单节点模式 (无 numactl 或单节点环境)
run_single_node_test() {
    log_info "运行单节点热点访问 (无法触发跨节点迁移)"
    
    "$YCSB_BIN" run redis \
        -P "$WORKLOADS_DIR/workload_numa_migration" \
        -p recordcount="$RECORD_COUNT" \
        -p operationcount="$OPERATION_COUNT" \
        -p threadcount="$THREAD_COUNT" \
        -p redis.host=127.0.0.1 \
        -p redis.port="$REDIS_PORT" \
        -p requestdistribution=zipfian \
        -p zipfianconstant=0.99 \
        -s 2>&1 | tee /tmp/ycsb_run.txt
}

# ── 查看迁移结果 ───────────────────────────────────────────────────────────
check_migration() {
    log_step "步骤 5: 查看迁移结果"

    echo -e "${BOLD}${CYAN}迁移统计:${NC}"
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA MIGRATE STATS 2>/dev/null || \
        echo "无法获取迁移统计"

    echo -e "\n${BOLD}${CYAN}策略统计:${NC}"
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA CONFIG STATS 2>/dev/null || \
        echo "无法获取策略统计"

    echo -e "\n${BOLD}${CYAN}策略列表:${NC}"
    "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA STRATEGY LIST 2>/dev/null || \
        echo "无法获取策略列表"

    echo -e "\n${BOLD}${CYAN}热点 Key 示例:${NC}"
    for key in user:100 user:200 user:300 user:500 user:1000; do
        echo -e "${YELLOW}Key: $key${NC}"
        "$REDIS_CLI" -h 127.0.0.1 -p "$REDIS_PORT" NUMA MIGRATE INFO "$key" 2>/dev/null || \
            echo "  (无元数据)"
    done

    # 从日志中提取关键信息
    echo -e "\n${BOLD}${CYAN}迁移相关日志 (最近20条):${NC}"
    grep -i "migrate\|composite.*lru\|candidate" /tmp/redis_migration_test.log 2>/dev/null | tail -20 || \
        echo "无迁移日志 (可能未启用 verbose 模式)"
}

# ── 清理 ───────────────────────────────────────────────────────────────────
cleanup() {
    log_info "测试完成，停止 Redis..."
    pkill -9 -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
}

# ── 主流程 ─────────────────────────────────────────────────────────────────
main() {
    parse_args "$@"

    echo -e "${BOLD}${CYAN}"
    echo "╔═══════════════════════════════════════════════════╗"
    echo "║   NUMA 迁移触发测试                                ║"
    echo "║   模式: $MODE                                     "
    echo "║   记录数: $RECORD_COUNT                           "
    echo "║   操作数: $OPERATION_COUNT                        "
    echo "║   线程数: $THREAD_COUNT                           "
    echo "║   迁移阈值: $MIGRATION_THRESHOLD                   "
    echo "╚═══════════════════════════════════════════════════╝"
    echo -e "${NC}"

    check_deps
    start_redis
    config_numa
    load_data
    run_hotspot_access
    check_migration

    log_ok "全部完成"
}

# 捕获退出信号
trap cleanup EXIT

main "$@"
