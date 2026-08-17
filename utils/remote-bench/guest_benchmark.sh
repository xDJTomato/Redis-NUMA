#!/usr/bin/env bash
# ============================================================================
# guest_benchmark.sh — 在 QEMU guest 内执行的 Redis-NUMA 测试流水线
# 由 remote_pipeline.sh 上传到 guest 并执行，运行在 Ubuntu cloud image 上。
#
# 步骤: 挂载 9p -> 同步代码 -> 安装依赖 -> 编译 -> YCSB -> 压测 -> 回写结果
#
# 用法: sudo bash guest_benchmark.sh --bench-mode bw|stress|baseline [--maxmem MEM]
# ============================================================================
set -euo pipefail

BENCH_MODE="bw"
MAX_MEM="6000mb"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*"; }
log_step() { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --bench-mode) BENCH_MODE="$2"; shift 2 ;;
            --maxmem)     MAX_MEM="$2";    shift 2 ;;
            *) log_err "未知参数: $1"; exit 1 ;;
        esac
    done
    case "$BENCH_MODE" in bw|stress|baseline) ;; *) log_err "无效 --bench-mode: $BENCH_MODE"; exit 1 ;; esac
}

# ── A. 挂载 9p 共享目录 ─────────────────────────────────────────────────────
mount_host() {
    log_step "A. 挂载 9p 共享目录 (宿主机代码)"
    mkdir -p /mnt/host
    if mountpoint -q /mnt/host; then
        log_ok "/mnt/host 已挂载"
    else
        mount -t 9p -o trans=virtio,version=9p2000.L host0 /mnt/host 2>/dev/null && \
            log_ok "9p 挂载成功" || log_warn "9p 挂载失败，将使用已有代码"
    fi
}

# ── B. 同步代码 ─────────────────────────────────────────────────────────────
sync_code() {
    log_step "B. 同步最新代码到本地磁盘"
    if mountpoint -q /mnt/host; then
        mkdir -p /root/redis-numa
        rsync -a --delete --exclude='.git' /mnt/host/ /root/redis-numa/
        log_ok "代码已同步: $(du -sh /root/redis-numa 2>/dev/null | cut -f1)"
    else
        log_warn "9p 不可用，假设 /root/redis-numa 已存在"
        [[ -f /root/redis-numa/src/Makefile ]] || { log_err "项目源码缺失"; exit 1; }
    fi
}

# ── C. 安装依赖 ─────────────────────────────────────────────────────────────
install_deps() {
    log_step "C. 安装构建与测试依赖"
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        build-essential libnuma-dev pkg-config numactl rsync wget \
        openjdk-17-jdk-headless python3 python3-pip python3-matplotlib
    log_ok "依赖安装完成"
}

# ── D. 编译 Redis-NUMA ──────────────────────────────────────────────────────
build_redis() {
    log_step "D. 编译 Redis-NUMA (Linux 下自动启用 HAVE_NUMA + MALLOC=libc)"
    cd /root/redis-numa
    make -j"$(nproc)"
    [[ -x src/redis-server && -x src/redis-cli ]] || { log_err "编译产物缺失"; exit 1; }
    log_ok "编译完成: $(src/redis-server --version 2>/dev/null | head -1)"
}

# ── E. 准备 YCSB ────────────────────────────────────────────────────────────
prepare_ycsb() {
    log_step "E. 准备 YCSB"
    cd /root/redis-numa/tests/ycsb
    if [[ ! -f ycsb-0.17.0/bin/ycsb.sh ]]; then
        ./scripts/install_ycsb.sh
    fi
    [[ -f ycsb-0.17.0/bin/ycsb.sh ]] || { log_err "YCSB 安装失败"; exit 1; }
    log_ok "YCSB 就绪"
}

# ── F. 压力测试 ─────────────────────────────────────────────────────────────
run_benchmark() {
    log_step "F. 运行压力测试 (mode=$BENCH_MODE, maxmem=$MAX_MEM)"
    cd /root/redis-numa/tests/ycsb
    case "$BENCH_MODE" in
        bw)
            ./run_bw_benchmark.sh --process-nodes 0,1 --maxmem "$MAX_MEM"
            ;;
        stress)
            ./run_ycsb.sh --mode stress --maxmem "$MAX_MEM"
            ;;
        baseline)
            ./run_ycsb.sh --mode baseline
            ;;
    esac
    log_ok "压力测试完成"
}

# ── G. 回写结果 ─────────────────────────────────────────────────────────────
publish_results() {
    log_step "G. 回写结果到 9p 共享目录 (宿主机可见)"
    if mountpoint -q /mnt/host; then
        mkdir -p /mnt/host/tests/ycsb/results
        rsync -a /root/redis-numa/tests/ycsb/results/ /mnt/host/tests/ycsb/results/
        log_ok "结果已写回宿主机"
    fi
    log "最新结果目录:"
    ls -1t /root/redis-numa/tests/ycsb/results/ 2>/dev/null | head -3 || true
}

main() {
    parse_args "$@"
    log "guest 流水线启动: bench=$BENCH_MODE maxmem=$MAX_MEM"
    mount_host
    sync_code
    install_deps
    build_redis
    prepare_ycsb
    run_benchmark
    publish_results
    log_ok "guest 流水线全部完成"
}

main "$@"
