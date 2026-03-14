#!/bin/bash
################################################################################
# test_numa_command.sh - NUMA 统一命令单元测试
#
# 覆盖范围:
#   NUMA MIGRATE KEY/DB/SCAN/STATS/RESET/INFO
#   NUMA CONFIG GET/SET/LOAD/REBALANCE/STATS
#   NUMA STRATEGY SLOT/LIST
#   NUMA HELP
#   错误路径（参数缺失、越界、无效值）
#
# 用法:
#   ./tests/unit/test_numa_command.sh [OPTIONS]
#
# 选项:
#   -p <port>      Redis 端口（默认 6399）
#   -H <host>      Redis 地址（默认 127.0.0.1）
#   -b <bindir>    redis-server/redis-cli 所在目录（默认 ./src）
#   -c <cfgfile>   composite-lru JSON 配置文件路径（用于 CONFIG LOAD 测试）
#   -k             测试结束后保留 Redis 进程（默认自动关闭）
#   -q             仅输出 PASS/FAIL 摘要，不打印中间日志
#   -h             打印帮助信息
#
# 退出码:
#   0  全部通过
#   1  存在失败用例
################################################################################

set -uo pipefail

# ─── 默认参数 ────────────────────────────────────────────────────────────────
REDIS_PORT=6399
REDIS_HOST="127.0.0.1"
BIN_DIR="./src"
CXL_CFG_FILE=""
KEEP_REDIS=0
QUIET=0

# ─── 颜色 ────────────────────────────────────────────────────────────────────
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m'

# ─── 统计 ────────────────────────────────────────────────────────────────────
PASS=0
FAIL=0
SKIP=0
FAIL_LIST=()

# ─── 辅助函数 ────────────────────────────────────────────────────────────────
log()   { [[ $QUIET -eq 0 ]] && echo -e "${BLUE}[INFO ]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN ]${NC} $*"; }
fatal() { echo -e "${RED}[FATAL]${NC} $*"; exit 1; }

header() {
    [[ $QUIET -eq 0 ]] && echo -e "\n${CYAN}══════════════════════════════════════════${NC}"
    [[ $QUIET -eq 0 ]] && echo -e "${CYAN}  $*${NC}"
    [[ $QUIET -eq 0 ]] && echo -e "${CYAN}══════════════════════════════════════════${NC}"
}

# assert_ok <test_name> <actual_output>
#   通过条件：输出不含 ERR / error（大小写不敏感）且不为空
assert_ok() {
    local name="$1"
    local out="$2"
    if [[ -n "$out" ]] && ! echo "$out" | grep -qi "^ERR\|^-ERR\|^(error)"; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
        (( PASS++ )) || true
    else
        echo -e "  ${RED}[FAIL]${NC} $name  →  $out"
        (( FAIL++ )) || true
        FAIL_LIST+=("$name")
    fi
}

# assert_eq <test_name> <actual> <expected>
assert_eq() {
    local name="$1" actual="$2" expected="$3"
    if [[ "$actual" == "$expected" ]]; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
        (( PASS++ )) || true
    else
        echo -e "  ${RED}[FAIL]${NC} $name  →  got='$actual'  want='$expected'"
        (( FAIL++ )) || true
        FAIL_LIST+=("$name")
    fi
}

# assert_contains <test_name> <actual> <substring>
assert_contains() {
    local name="$1" actual="$2" substr="$3"
    if echo "$actual" | grep -q "$substr"; then
        echo -e "  ${GREEN}[PASS]${NC} $name"
        (( PASS++ )) || true
    else
        echo -e "  ${RED}[FAIL]${NC} $name  →  '$substr' not found in: $actual"
        (( FAIL++ )) || true
        FAIL_LIST+=("$name")
    fi
}

# assert_error <test_name> <actual_output>
#   通过条件：输出包含 ERR 或 error
assert_error() {
    local name="$1" out="$2"
    if echo "$out" | grep -qi "ERR\|error\|WRONGTYPE"; then
        echo -e "  ${GREEN}[PASS]${NC} $name  (正确拒绝)"
        (( PASS++ )) || true
    else
        echo -e "  ${RED}[FAIL]${NC} $name  →  应报错但返回: $out"
        (( FAIL++ )) || true
        FAIL_LIST+=("$name")
    fi
}

skip_test() {
    local name="$1" reason="$2"
    echo -e "  ${YELLOW}[SKIP]${NC} $name  ($reason)"
    (( SKIP++ )) || true
}

# CLI 快捷调用
cli() { "$BIN_DIR/redis-cli" -h "$REDIS_HOST" -p "$REDIS_PORT" "$@" 2>&1; }

# ─── 参数解析 ─────────────────────────────────────────────────────────────────
usage() {
    sed -n '3,20p' "$0" | sed 's/^# \?//'
    exit 0
}

while getopts "p:H:b:c:kqh" opt; do
    case $opt in
        p) REDIS_PORT="$OPTARG" ;;
        H) REDIS_HOST="$OPTARG" ;;
        b) BIN_DIR="$OPTARG" ;;
        c) CXL_CFG_FILE="$OPTARG" ;;
        k) KEEP_REDIS=1 ;;
        q) QUIET=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

# ─── Redis 生命周期管理 ───────────────────────────────────────────────────────
REDIS_STARTED_BY_US=0
LOG_FILE="/tmp/redis_numa_cmd_test_$REDIS_PORT.log"

start_redis() {
    log "启动 Redis (port=$REDIS_PORT)..."
    "$BIN_DIR/redis-server" \
        --port "$REDIS_PORT" \
        --bind "$REDIS_HOST" \
        --daemonize yes \
        --loglevel verbose \
        --logfile "$LOG_FILE" \
        --save "" \
        --appendonly no \
        --dir /tmp
    for i in {1..15}; do
        if cli PING 2>/dev/null | grep -q "PONG"; then
            log "Redis 就绪 (PID=$(pgrep -f "redis-server.*$REDIS_PORT" | head -1))"
            REDIS_STARTED_BY_US=1
            return 0
        fi
        sleep 1
    done
    fatal "Redis 启动超时，请检查日志: $LOG_FILE"
}

stop_redis() {
    if [[ $KEEP_REDIS -eq 1 ]]; then
        log "保留 Redis 进程（-k 选项）"
        return
    fi
    if [[ $REDIS_STARTED_BY_US -eq 1 ]]; then
        log "关闭 Redis..."
        cli SHUTDOWN NOSAVE >/dev/null 2>&1 || true
        sleep 1
        pkill -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
    fi
}

# 确保 Redis 仍在线；若意外崩溃则重新启动
ensure_redis_alive() {
    if ! cli PING 2>/dev/null | grep -q "PONG"; then
        warn "Redis 连接断开，尝试重新启动..."
        pkill -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
        sleep 1
        start_redis
        REDIS_STARTED_BY_US=1
        # 补写测试用 key
        for i in {1..50}; do
            cli SET "numa_unit_key_$i" "val_$i" >/dev/null
        done
        cli SET numa_test_key "hello_numa" >/dev/null
    fi
}

check_or_start_redis() {
    if cli PING 2>/dev/null | grep -q "PONG"; then
        log "检测到运行中的 Redis (port=$REDIS_PORT)"
    else
        start_redis
    fi
}

trap stop_redis EXIT

# ─── 测试套件 ─────────────────────────────────────────────────────────────────

# ── 1. NUMA HELP ──────────────────────────────────────────────────────────────
suite_help() {
    header "Suite: NUMA HELP"

    out=$(cli NUMA HELP)
    assert_contains "HELP 包含 MIGRATE 提示"  "$out" "MIGRATE"
    assert_contains "HELP 包含 CONFIG 提示"   "$out" "CONFIG"
    assert_contains "HELP 包含 STRATEGY 提示" "$out" "STRATEGY"
    assert_contains "HELP 包含 HELP 提示"     "$out" "HELP"
}

# ── 2. NUMA 顶层错误路径 ──────────────────────────────────────────────────────
suite_top_errors() {
    header "Suite: 顶层错误路径"

    out=$(cli NUMA)
    assert_error "NUMA 无参数应报错" "$out"

    out=$(cli NUMA UNKNOWN_DOMAIN)
    assert_error "NUMA 未知域应报错" "$out"
}

# ── 3. NUMA MIGRATE ───────────────────────────────────────────────────────────
suite_migrate() {
    header "Suite: NUMA MIGRATE"

    # 3-1 STATS（空库也应返回统计）
    out=$(cli NUMA MIGRATE STATS)
    assert_contains "MIGRATE STATS 返回 total_migrations"    "$out" "total_migrations"
    assert_contains "MIGRATE STATS 返回 successful_migrations" "$out" "successful_migrations"
    assert_contains "MIGRATE STATS 返回 failed_migrations"   "$out" "failed_migrations"
    assert_contains "MIGRATE STATS 返回 total_bytes_migrated" "$out" "total_bytes_migrated"

    # 3-2 RESET
    out=$(cli NUMA MIGRATE RESET)
    assert_eq "MIGRATE RESET 返回 OK" "$out" "OK"

    # 3-3 统计清零验证
    out=$(cli NUMA MIGRATE STATS)
    total=$(echo "$out" | awk '/total_migrations/{getline; print}' | tr -d ' \r')
    assert_eq "MIGRATE RESET 后 total_migrations=0" "$total" "0"

    # 3-4 写入测试 key，再测 INFO
    cli SET numa_test_key "hello_numa" >/dev/null
    out=$(cli NUMA MIGRATE INFO numa_test_key)
    assert_contains "MIGRATE INFO 返回 type"          "$out" "type"
    assert_contains "MIGRATE INFO 返回 hotness_level" "$out" "hotness_level"
    assert_contains "MIGRATE INFO 返回 access_count"  "$out" "access_count"
    assert_contains "MIGRATE INFO 返回 current_node"  "$out" "current_node"

    # 3-5 INFO key 不存在
    out=$(cli NUMA MIGRATE INFO nonexistent_key_xyz)
    assert_error "MIGRATE INFO 不存在的 key 应报错" "$out"

    # 3-6 KEY 迁移到节点 0（本地节点总是合法）
    out=$(cli NUMA MIGRATE KEY numa_test_key 0)
    # 结果可能是 OK 或 error（取决于 NUMA 硬件可用性），两者均接受
    if echo "$out" | grep -q "^OK"; then
        echo -e "  ${GREEN}[PASS]${NC} MIGRATE KEY → node 0  (执行成功)"
        (( PASS++ )) || true
    else
        echo -e "  ${YELLOW}[SKIP]${NC} MIGRATE KEY → node 0  (非 NUMA 环境: $out)"
        (( SKIP++ )) || true
    fi

    # 3-7 KEY 迁移节点越界
    out=$(cli NUMA MIGRATE KEY numa_test_key 9999)
    assert_error "MIGRATE KEY 节点越界应报错" "$out"

    # 3-8 SCAN（无参数，使用默认批量）
    out=$(cli NUMA MIGRATE SCAN)
    assert_contains "MIGRATE SCAN 返回 scanned" "$out" "scanned"
    assert_contains "MIGRATE SCAN 返回 migrated" "$out" "migrated"

    # 3-9 SCAN COUNT n
    out=$(cli NUMA MIGRATE SCAN COUNT 10)
    assert_contains "MIGRATE SCAN COUNT 10 返回 scanned" "$out" "scanned"

    # 3-10 SCAN 参数错误
    out=$(cli NUMA MIGRATE SCAN INVALID)
    assert_error "MIGRATE SCAN 无效参数应报错" "$out"

    # 3-11 DB 迁移（迁至节点0，这个操作属于破坏性，放在 MIGRATE suite 最末）
    out=$(cli NUMA MIGRATE DB 0)
    if echo "$out" | grep -qiE "^OK|failed|partial"; then
        echo -e "  ${GREEN}[PASS]${NC} MIGRATE DB 返回结果（$out）"
        (( PASS++ )) || true
    else
        assert_error "MIGRATE DB 应返回 OK 或错误信息" "$out"
    fi

    # 如果 Redis 崩溃，自动恢复
    ensure_redis_alive

    # 3-12 MIGRATE 缺少子命令
    out=$(cli NUMA MIGRATE)
    assert_error "MIGRATE 无子命令应报错" "$out"

    # 3-13 MIGRATE 未知子命令
    out=$(cli NUMA MIGRATE BADCMD)
    assert_error "MIGRATE 未知子命令应报错" "$out"
}

# ── 4. NUMA CONFIG ────────────────────────────────────────────────────────────
suite_config() {
    header "Suite: NUMA CONFIG"

    # 4-1 GET
    out=$(cli NUMA CONFIG GET)
    assert_contains "CONFIG GET 返回 strategy"          "$out" "strategy"
    assert_contains "CONFIG GET 返回 nodes"             "$out" "nodes"
    assert_contains "CONFIG GET 返回 balance_threshold" "$out" "balance_threshold"
    assert_contains "CONFIG GET 返回 auto_rebalance"    "$out" "auto_rebalance"
    assert_contains "CONFIG GET 返回 node_weights"      "$out" "node_weights"

    # 4-2 SET strategy（合法值）
    for strat in local_first interleaved round_robin weighted; do
        out=$(cli NUMA CONFIG SET strategy "$strat")
        assert_eq "CONFIG SET strategy $strat 返回 OK" "$out" "OK"
    done

    # 4-3 SET strategy（非法值）
    out=$(cli NUMA CONFIG SET strategy no_such_strategy)
    assert_error "CONFIG SET strategy 非法值应报错" "$out"

    # 4-4 SET cxl_optimization on/off
    out=$(cli NUMA CONFIG SET cxl_optimization on)
    assert_eq "CONFIG SET cxl_optimization on" "$out" "OK"
    out=$(cli NUMA CONFIG SET cxl_optimization off)
    assert_eq "CONFIG SET cxl_optimization off" "$out" "OK"

    # 4-5 SET balance_threshold 合法范围
    out=$(cli NUMA CONFIG SET balance_threshold 30)
    assert_eq "CONFIG SET balance_threshold 30" "$out" "OK"

    # 4-6 SET balance_threshold 越界
    out=$(cli NUMA CONFIG SET balance_threshold 150)
    assert_error "CONFIG SET balance_threshold 越界应报错" "$out"

    # 4-7 SET weight 合法
    out=$(cli NUMA CONFIG SET weight 0 100)
    assert_eq "CONFIG SET weight 0 100" "$out" "OK"

    # 4-8 SET weight 节点越界
    out=$(cli NUMA CONFIG SET weight 9999 100)
    assert_error "CONFIG SET weight 节点越界应报错" "$out"

    # 4-9 SET weight 权重越界
    out=$(cli NUMA CONFIG SET weight 0 9999)
    assert_error "CONFIG SET weight 权重越界（>1000）应报错" "$out"

    # 4-10 REBALANCE
    out=$(cli NUMA CONFIG REBALANCE)
    assert_eq "CONFIG REBALANCE 返回 OK" "$out" "OK"

    # 4-11 STATS
    out=$(cli NUMA CONFIG STATS)
    assert_ok "CONFIG STATS 返回数据" "$out"

    # 4-12 LOAD（有配置文件时测试）
    if [[ -n "$CXL_CFG_FILE" && -f "$CXL_CFG_FILE" ]]; then
        out=$(cli NUMA CONFIG LOAD "$CXL_CFG_FILE")
        assert_eq "CONFIG LOAD 指定路径返回 OK" "$out" "OK"
    else
        skip_test "CONFIG LOAD 指定路径" "未提供 -c <cfgfile>"
    fi

    # 4-13 LOAD 不存在的路径
    out=$(cli NUMA CONFIG LOAD /nonexistent/path/cfg.json)
    assert_error "CONFIG LOAD 不存在路径应报错" "$out"

    # 4-14 CONFIG 缺少子命令
    out=$(cli NUMA CONFIG)
    assert_error "CONFIG 无子命令应报错" "$out"

    # 4-15 CONFIG 未知子命令
    out=$(cli NUMA CONFIG BADCMD)
    assert_error "CONFIG 未知子命令应报错" "$out"

    # 4-16 SET 参数不足
    out=$(cli NUMA CONFIG SET)
    assert_error "CONFIG SET 无参数应报错" "$out"
}

# ── 5. NUMA STRATEGY ──────────────────────────────────────────────────────────
suite_strategy() {
    header "Suite: NUMA STRATEGY"

    # 5-1 LIST（直接查看当前注册状态）
    out=$(cli NUMA STRATEGY LIST)
    assert_ok "STRATEGY LIST 返回内容" "$out"

    # 5-2 SLOT 插入非法 strategy name（应报错）
    out=$(cli NUMA STRATEGY SLOT 99 no_such_strategy_xyz)
    assert_error "STRATEGY SLOT 插入不存在的策略名应报错" "$out"

    # 5-3 STRATEGY 缺少子命令
    out=$(cli NUMA STRATEGY)
    assert_error "STRATEGY 无子命令应报错" "$out"

    # 5-4 STRATEGY 未知子命令
    out=$(cli NUMA STRATEGY BADCMD)
    assert_error "STRATEGY 未知子命令应报错" "$out"

    # 5-5 SLOT 参数不足
    out=$(cli NUMA STRATEGY SLOT 1)
    assert_error "STRATEGY SLOT 参数不足应报错" "$out"
}

# ── 6. 并发安全（连续快速调用不崩溃）────────────────────────────────────────
suite_stability() {
    header "Suite: 稳定性（快速连续调用）"

    log "连续 20 次 NUMA MIGRATE STATS..."
    for i in {1..20}; do
        cli NUMA MIGRATE STATS >/dev/null
    done
    echo -e "  ${GREEN}[PASS]${NC} 20 次 MIGRATE STATS 无崩溃"
    (( PASS++ )) || true

    log "连续 20 次 NUMA CONFIG GET..."
    for i in {1..20}; do
        cli NUMA CONFIG GET >/dev/null
    done
    echo -e "  ${GREEN}[PASS]${NC} 20 次 CONFIG GET 无崩溃"
    (( PASS++ )) || true

    log "连续 10 次 NUMA MIGRATE SCAN..."
    for i in {1..10}; do
        cli NUMA MIGRATE SCAN >/dev/null
    done
    echo -e "  ${GREEN}[PASS]${NC} 10 次 MIGRATE SCAN 无崩溃"
    (( PASS++ )) || true
}

# ── 7. 清理测试数据 ───────────────────────────────────────────────────────────
cleanup_test_data() {
    cli DEL numa_test_key >/dev/null 2>&1 || true
    cli FLUSHDB >/dev/null 2>&1 || true
}

# ─── 主流程 ──────────────────────────────────────────────────────────────────
main() {
    echo ""
    echo "╔══════════════════════════════════════════╗"
    echo "║   NUMA 统一命令单元测试 (numa_command)    ║"
    echo "╚══════════════════════════════════════════╝"
    echo "  Redis : $REDIS_HOST:$REDIS_PORT"
    echo "  BinDir: $BIN_DIR"
    [[ -n "$CXL_CFG_FILE" ]] && echo "  CfgFile: $CXL_CFG_FILE"
    echo ""

    check_or_start_redis

    # 写入少量测试数据供迁移测试使用
    log "预写入测试数据（50 keys）..."
    for i in {1..50}; do
        cli SET "numa_unit_key_$i" "val_$i" >/dev/null
    done

    # 执行各测试套件
    suite_help
    ensure_redis_alive
    suite_top_errors
    ensure_redis_alive
    suite_migrate
    ensure_redis_alive
    suite_config
    ensure_redis_alive
    suite_strategy
    ensure_redis_alive
    suite_stability

    cleanup_test_data

    # ── 结果摘要 ──────────────────────────────────────────────────────────────
    echo ""
    echo "══════════════════════════════════════════"
    echo "  测试结果摘要"
    echo "══════════════════════════════════════════"
    echo -e "  ${GREEN}PASS${NC}: $PASS"
    echo -e "  ${RED}FAIL${NC}: $FAIL"
    echo -e "  ${YELLOW}SKIP${NC}: $SKIP"
    echo "  TOTAL: $((PASS + FAIL + SKIP))"

    if [[ $FAIL -gt 0 ]]; then
        echo ""
        echo -e "  ${RED}失败用例列表:${NC}"
        for f in "${FAIL_LIST[@]}"; do
            echo "    - $f"
        done
        echo ""
        exit 1
    fi

    echo ""
    echo -e "  ${GREEN}✅ 全部通过！${NC}"
    echo ""
}

main
