#!/usr/bin/env bash
# ============================================================================
# remote_pipeline.sh — Redis-NUMA 远端一键测试流水线（宿主机侧）
#
# 链路: [本地 one_shot.ps1] -> scp 上传代码/tar.gz
#       -> 本脚本在 64GB 云服务器上:
#           1. 安装宿主依赖 (qemu-system, cloud-image-utils)
#           2. 准备 Ubuntu cloud image + cloud-init seed (注入 ssh key)
#           3. 启动 QEMU guest: 双 NUMA 节点 (Node0=DRAM, Node1=模拟CXL)
#           4. 等待 guest ssh 就绪
#           5. 执行 guest_benchmark.sh (编译 Redis-NUMA + 完整压力测试)
#           6. 结果通过 9p 写回宿主机 $PROJECT/tests/ycsb/results
#
# 用法: bash remote_pipeline.sh [选项]
#   --bench-mode bw|stress|baseline   测试模式 (默认: bw)
#   --sim-mode numa|cxl               内存模拟方式 (默认: numa, cxl 需 qemu7.2+)
#   --node0-mem SIZE                  Node 0 DRAM 大小 (默认: 4g)
#   --node1-mem SIZE                  Node 1 模拟 CXL 大小 (默认: 8g)
#   --cpus N                          guest vCPU 数 (默认: 4)
#   --guest-port PORT                  guest ssh 转发端口 (默认: 2222)
#   --maxmem MEM                      redis maxmemory (默认: 6000mb)
#   --keep-vm 0|1                     测试后保留 VM 运行 (默认: 1)
#   --help
# ============================================================================
set -euo pipefail

# ── 默认参数 ────────────────────────────────────────────────────────────────
BENCH_MODE="bw"
SIM_MODE="numa"
NODE0_MEM="4g"
NODE1_MEM="8g"
GUEST_CPUS=4
GUEST_PORT=2222
MAX_MEM="6000mb"
KEEP_VM=1

PROJECT="/root/redis-numa"
TOOLS_DIR="/root/redis-numa-tools"
VM_DIR="/root/qemu-vm"
GUEST_USER="ubuntu"
SSH_KEY="$VM_DIR/id_ed25519"
CLOUD_IMG_URL="https://cloud-images.ubuntu.com/noble/current/noble-server-cloudimg-amd64.img"
QEMU_BIN="qemu-system-x86_64"

# ── 颜色/日志 ───────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BLUE='\033[0;34m'; CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'
log()      { echo -e "${BLUE}[$(date '+%H:%M:%S')]${NC} $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*"; }
log_step() { echo -e "\n${BOLD}${CYAN}══ $* ══${NC}"; }

usage() {
    cat <<'EOF'
用法: bash remote_pipeline.sh [选项]

  --bench-mode bw|stress|baseline   测试模式 (默认: bw)
  --sim-mode numa|cxl               内存模拟方式 (默认: numa; cxl 需 qemu7.2+)
  --node0-mem SIZE                  Node 0 DRAM 大小 (默认: 4g)
  --node1-mem SIZE                  Node 1 模拟 CXL 大小 (默认: 8g)
  --cpus N                          guest vCPU 数, >=2 (默认: 4)
  --guest-port PORT                 guest ssh 转发端口 (默认: 2222)
  --maxmem MEM                      redis maxmemory (默认: 6000mb)
  --keep-vm 0|1                     测试后保留 VM 运行 (默认: 1)
  --help                            显示此帮助
EOF
    exit 0
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --bench-mode) BENCH_MODE="$2"; shift 2 ;;
            --sim-mode)   SIM_MODE="$2";   shift 2 ;;
            --node0-mem)  NODE0_MEM="$2";  shift 2 ;;
            --node1-mem)  NODE1_MEM="$2";  shift 2 ;;
            --cpus)       GUEST_CPUS="$2"; shift 2 ;;
            --guest-port) GUEST_PORT="$2"; shift 2 ;;
            --maxmem)     MAX_MEM="$2";    shift 2 ;;
            --keep-vm)    KEEP_VM="$2";    shift 2 ;;
            --help|-h)    usage ;;
            *) log_err "未知参数: $1"; usage ;;
        esac
    done
    case "$BENCH_MODE" in bw|stress|baseline) ;; *) log_err "无效 --bench-mode: $BENCH_MODE"; usage ;; esac
    case "$SIM_MODE" in numa|cxl) ;; *) log_err "无效 --sim-mode: $SIM_MODE"; usage ;; esac
    if (( GUEST_CPUS < 2 )); then
        log_err "--cpus 必须 >= 2 (需要将 vCPU 分配到两个 NUMA 节点)"
        exit 1
    fi
}

# 将 "4g/8192M" 转成 MB 整数
to_mb() {
    local v="${1,,}"
    case "$v" in
        *g) echo $(( ${v%g} * 1024 )) ;;
        *m) echo $(( ${v%m} )) ;;
        *)  echo $(( v / 1048576 )) ;;
    esac
}

# ── 1. 宿主依赖 ─────────────────────────────────────────────────────────────
ensure_host_deps() {
    log_step "1/6 检查宿主依赖"
    local missing=()
    command -v "$QEMU_BIN" >/dev/null 2>&1 || missing+=(qemu-system-x86)
    command -v qemu-img   >/dev/null 2>&1 || missing+=(qemu-utils)
    command -v cloud-localds >/dev/null 2>&1 || missing+=(cloud-image-utils)
    command -v wget >/dev/null 2>&1 || command -v curl >/dev/null 2>&1 || missing+=(wget)
    if [[ ${#missing[@]} -gt 0 ]]; then
        log "安装缺失依赖: ${missing[*]}"
        export DEBIAN_FRONTEND=noninteractive
        apt-get update -qq
        apt-get install -y "${missing[@]}" genisoimage
    fi
    # KVM 检测
    if [[ -e /dev/kvm ]]; then
        log_ok "KVM 可用 (/dev/kvm)"
    else
        log_warn "/dev/kvm 不存在：将使用 TCG 软件模拟，压测速度会慢 10-50 倍。"
        log_warn "若这是云服务器，请在控制台开启嵌套虚拟化 (Nested Virtualization)。"
    fi
}

# ── 2. 准备 guest 镜像 ──────────────────────────────────────────────────────
prepare_guest_image() {
    log_step "2/6 准备 guest 镜像"
    mkdir -p "$VM_DIR"
    # ssh key
    if [[ ! -f "$SSH_KEY" ]]; then
        ssh-keygen -t ed25519 -N "" -f "$SSH_KEY" -C "redis-numa-guest" >/dev/null
        log_ok "已生成 guest ssh key: $SSH_KEY"
    fi
    local base="$VM_DIR/noble-server-cloudimg-amd64.img"
    if [[ ! -f "$base" ]]; then
        log "下载 Ubuntu 24.04 cloud image (约 600MB)..."
        if command -v wget >/dev/null 2>&1; then
            wget -q --show-progress -O "$base" "$CLOUD_IMG_URL"
        else
            curl -fSL -o "$base" "$CLOUD_IMG_URL"
        fi
        log_ok "基础镜像下载完成"
    fi
    if [[ ! -f "$VM_DIR/guest.qcow2" ]]; then
        qemu-img create -f qcow2 -F qcow2 -b "$base" "$VM_DIR/guest.qcow2" 30G >/dev/null
        log_ok "已创建 overlay 磁盘 guest.qcow2"
    fi
    # cloud-init seed
    local pubkey
    pubkey="$(cat "$SSH_KEY.pub")"
    cat > "$VM_DIR/user-data" <<EOF
#cloud-config
hostname: redis-numa-guest
users:
  - name: $GUEST_USER
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $pubkey
ssh_pwauth: false
EOF
    cloud-localds "$VM_DIR/seed.iso" "$VM_DIR/user-data" 2>/dev/null || \
        genisoimage -quiet -output "$VM_DIR/seed.iso" -volid cidata -joliet -rock "$VM_DIR/user-data"
    log_ok "cloud-init seed 已生成"
}

# ── 3. 启动 VM ──────────────────────────────────────────────────────────────
total_mem() {
    echo $(( $(to_mb "$NODE0_MEM") + $(to_mb "$NODE1_MEM") ))
}

vm_running() {
    [[ -f "$VM_DIR/vm.pid" ]] && kill -0 "$(cat "$VM_DIR/vm.pid")" 2>/dev/null
}

launch_vm() {
    log_step "3/6 启动 QEMU guest (sim=$SIM_MODE, node0=$NODE0_MEM, node1=$NODE1_MEM, cpu=$GUEST_CPUS)"
    if vm_running; then
        log_ok "检测到 VM 已在运行 (pid=$(cat "$VM_DIR/vm.pid"))，复用现有 VM"
        return 0
    fi
    local qemu_args=()
    local half=$(( GUEST_CPUS / 2 ))
    if [[ "$SIM_MODE" == "cxl" ]]; then
        if ! "$QEMU_BIN" -machine help 2>&1 | grep -qi cxl; then
            log_warn "当前 QEMU 不支持 CXL 设备仿真，回退到 NUMA 双节点模式"
            SIM_MODE="numa"
        fi
    fi
    if [[ "$SIM_MODE" == "cxl" ]]; then
        qemu_args=(
            -machine q35,cxl=on
            -m "$NODE0_MEM",maxmem=32G
            -object memory-backend-file,id=cxl-mem1,size="$NODE1_MEM",mem-path="$VM_DIR/cxl_mem.raw",share=on
            -object memory-backend-ram,id=cxl-mem2,size=1G,share=on
            -device pxb-cxl,bus_nr=52,uid=0,id=cxl0
            -device cxl-rp,port=0,bus=dev0,chassis=0,slot=0,id=root0
            -device cxl-type3,bus=root0,volatile-memdev=cxl-mem1,persistent-memdev=cxl-mem2,id=cxl1
        )
        log_warn "CXL 仿真模式下 CXL 内存需 guest 内核在线，若 numactl 仍显示 1 节点请改用 --sim-mode numa"
    else
        qemu_args=(
            -machine q35
            -m "$(total_mem)M"
            -object memory-backend-ram,size="$NODE0_MEM",id=mem0
            -object memory-backend-ram,size="$NODE1_MEM",id=mem1
            -numa node,nodeid=0,cpus=0-$((half-1)),memdev=mem0
            -numa node,nodeid=1,cpus=$half-$((GUEST_CPUS-1)),memdev=mem1
        )
    fi
    local accel=()
    [[ -e /dev/kvm ]] && accel=(-enable-kvm -cpu host) || accel=(-accel tcg)
    "$QEMU_BIN" \
        -name redis-numa-guest \
        "${accel[@]}" \
        -smp "$GUEST_CPUS" \
        "${qemu_args[@]}" \
        -drive file="$VM_DIR/guest.qcow2",if=virtio,format=qcow2 \
        -drive file="$VM_DIR/seed.iso",if=virtio,format=raw \
        -netdev user,id=net0,hostfwd=tcp::"$GUEST_PORT"-:22 \
        -device virtio-net-pci,netdev=net0 \
        -virtfs local,path="$PROJECT",mount_tag=host0,security_model=none,readonly=off \
        -display none \
        -serial file:"$VM_DIR/guest-console.log" \
        -pidfile "$VM_DIR/vm.pid" \
        -daemonize
    log_ok "QEMU 已启动 (pid=$(cat "$VM_DIR/vm.pid"), ssh: localhost:$GUEST_PORT)"
}

# ── 4. 等待 guest ssh ───────────────────────────────────────────────────────
wait_for_guest() {
    log_step "4/6 等待 guest SSH 就绪 (最多 600s)"
    local ssh_opts=(-i "$SSH_KEY" -p "$GUEST_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5)
    local i=0
    while (( i < 120 )); do
        if ssh "${ssh_opts[@]}" "$GUEST_USER@localhost" true 2>/dev/null; then
            log_ok "guest SSH 就绪 (耗时约 $((i*5))s)"
            ssh "${ssh_opts[@]}" "$GUEST_USER@localhost" \
                "echo '--- numactl ---'; numactl --hardware 2>/dev/null | grep -E 'available|node [0-9]+ size' || echo 'numactl 不可用'" || true
            return 0
        fi
        i=$((i+1)); sleep 5
    done
    log_err "guest SSH 等待超时。请检查: tail -50 $VM_DIR/guest-console.log"
    return 1
}

# ── 5. guest 内压测 ─────────────────────────────────────────────────────────
run_guest_benchmark() {
    log_step "5/6 在 guest 内执行完整压力测试 (bench=$BENCH_MODE, maxmem=$MAX_MEM)"
    local ssh_opts=(-i "$SSH_KEY" -p "$GUEST_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10)
    # 上传 guest 流水线脚本
    scp -q "${ssh_opts[@]}" "$TOOLS_DIR/guest_benchmark.sh" "$GUEST_USER@localhost:/tmp/guest_benchmark.sh"
    ssh "${ssh_opts[@]}" "$GUEST_USER@localhost" \
        "sudo bash /tmp/guest_benchmark.sh --bench-mode '$BENCH_MODE' --maxmem '$MAX_MEM'"
}

# ── 6. 收尾 ─────────────────────────────────────────────────────────────────
finish() {
    log_step "6/6 收尾"
    if [[ "$KEEP_VM" == "1" ]]; then
        log_ok "VM 保持运行，可手动连接: ssh -i $SSH_KEY -p $GUEST_PORT $GUEST_USER@localhost"
    else
        log "关闭 VM..."
        ssh -i "$SSH_KEY" -p "$GUEST_PORT" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
            "$GUEST_USER@localhost" "sudo poweroff" 2>/dev/null || true
        sleep 10
        [[ -f "$VM_DIR/vm.pid" ]] && kill "$(cat "$VM_DIR/vm.pid")" 2>/dev/null || true
        log_ok "VM 已关闭"
    fi
    log "结果目录 (宿主机): $PROJECT/tests/ycsb/results/"
    log "最新结果:"
    ls -1t "$PROJECT/tests/ycsb/results/" 2>/dev/null | head -3 || true
}

main() {
    parse_args "$@"
    log "Redis-NUMA 远端流水线启动: bench=$BENCH_MODE sim=$SIM_MODE node0=$NODE0_MEM node1=$NODE1_MEM"
    [[ -d "$PROJECT" ]] || { log_err "项目目录不存在: $PROJECT (请先由 one_shot.ps1 上传代码)"; exit 1; }
    ensure_host_deps
    prepare_guest_image
    launch_vm
    wait_for_guest
    run_guest_benchmark
    finish
}

main "$@"
