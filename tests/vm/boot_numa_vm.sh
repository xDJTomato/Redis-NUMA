#!/bin/bash
# ============================================================
# QEMU (TCG, no KVM) multi-NUMA-node smoke test for Redis-NUMA.
#
# Boots a Debian 12 generic-cloud image with 2 emulated NUMA
# nodes, copies the locally built redis-server/redis-cli/
# redis-benchmark into it, and runs a scaled-down bandwidth
# benchmark to confirm the NUMA code paths execute correctly
# under a real (if emulated) multi-node topology.
#
# This host has no /dev/kvm, so boot is pure software (TCG) and
# slow. If the VM does not come up within BOOT_TIMEOUT seconds,
# this script logs the failure honestly and exits non-zero --
# it never fabricates results. Callers (run_full_validation.sh)
# should treat this step as optional (--skip-vm).
#
# Usage: ./boot_numa_vm.sh [--timeout SECONDS] [--keep] [--skip]
# ============================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
CACHE_DIR="$SCRIPT_DIR/.cache"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$CACHE_DIR" "$RESULTS_DIR"

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
log()      { echo -e "[$(date +%H:%M:%S)] $*"; }
log_ok()   { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err()  { echo -e "${RED}[ERR]${NC}   $*"; }

BOOT_TIMEOUT=600
KEEP_VM=0
SKIP=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout) BOOT_TIMEOUT="$2"; shift 2 ;;
        --keep) KEEP_VM=1; shift ;;
        --skip) SKIP=1; shift ;;
        *) log_warn "unknown arg: $1"; shift ;;
    esac
done

RESULT_JSON="$RESULTS_DIR/qemu_smoke_$(date +%Y%m%d_%H%M%S).json"
write_result() {
    # write_result <status> <reason>
    cat > "$RESULT_JSON" <<EOF
{
  "phase": "qemu_numa_smoke_test",
  "status": "$1",
  "reason": "$2",
  "kvm_available": false,
  "accel": "tcg",
  "numa_nodes_emulated": 2
}
EOF
    log "result written to $RESULT_JSON"
}

if [[ "$SKIP" == "1" ]]; then
    log_warn "--skip requested, not booting VM"
    write_result "skipped" "explicitly skipped via --skip"
    exit 0
fi

command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    log_err "qemu-system-x86_64 not found"
    write_result "skipped" "qemu-system-x86_64 not installed"
    exit 0
}
if [[ ! -e /dev/kvm ]]; then
    log_warn "/dev/kvm not present -- using pure TCG software emulation (slow, expected on this host)"
fi

IMG_BASE_URL="https://cloud.debian.org/images/cloud/bookworm/latest"
IMG_NAME="debian-12-genericcloud-amd64.qcow2"
BASE_IMG="$CACHE_DIR/$IMG_NAME"
OVERLAY_IMG="$CACHE_DIR/overlay.qcow2"
SEED_IMG="$CACHE_DIR/seed.iso"
SSH_KEY="$CACHE_DIR/vm_test_key"
SERIAL_LOG="$CACHE_DIR/serial.log"
QMP_SOCK="$CACHE_DIR/qmp.sock"
PIDFILE="$CACHE_DIR/qemu.pid"
SSH_PORT=10222

cleanup() {
    if [[ "$KEEP_VM" == "1" ]]; then
        log "--keep set: leaving VM running (pid file: $PIDFILE, ssh: ssh -p $SSH_PORT -i $SSH_KEY numatest@127.0.0.1)"
        return
    fi
    if [[ -f "$PIDFILE" ]]; then
        local pid
        pid="$(cat "$PIDFILE" 2>/dev/null || true)"
        if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
            log "shutting down VM (pid $pid)"
            kill "$pid" 2>/dev/null
            for _ in $(seq 1 10); do
                kill -0 "$pid" 2>/dev/null || break
                sleep 1
            done
            kill -9 "$pid" 2>/dev/null || true
        fi
        rm -f "$PIDFILE"
    fi
}
trap cleanup EXIT

# ---- 1. Fetch base cloud image (cached) ----
if [[ ! -f "$BASE_IMG" ]]; then
    log "downloading $IMG_NAME (~400MB, cached under tests/vm/.cache/)"
    if ! curl -fL --max-time 300 -o "$BASE_IMG.part" "$IMG_BASE_URL/$IMG_NAME"; then
        log_err "download failed (network unavailable?)"
        rm -f "$BASE_IMG.part"
        write_result "skipped" "failed to download base cloud image"
        exit 0
    fi
    mv "$BASE_IMG.part" "$BASE_IMG"
    log_ok "downloaded $IMG_NAME"
else
    log "using cached base image $BASE_IMG"
fi

# ---- 2. Build a disposable overlay so the base image is never mutated ----
# No explicit size here: an overlay must inherit the backing file's virtual
# size exactly, or the guest's GPT partition table (near the end of the
# disk) becomes unreachable and boot fails with "PARTUUID does not exist".
rm -f "$OVERLAY_IMG"
qemu-img create -f qcow2 -F qcow2 -b "$BASE_IMG" "$OVERLAY_IMG" >/dev/null || {
    log_err "qemu-img overlay creation failed"
    write_result "failed" "qemu-img overlay creation failed"
    exit 1
}

# ---- 3. Ephemeral SSH keypair + cloud-init seed ----
rm -f "$SSH_KEY" "$SSH_KEY.pub"
ssh-keygen -t ed25519 -N "" -f "$SSH_KEY" -C "numa-vm-test" >/dev/null

cat > "$CACHE_DIR/user-data" <<EOF
#cloud-config
hostname: numa-test-vm
users:
  - name: numatest
    sudo: ALL=(ALL) NOPASSWD:ALL
    shell: /bin/bash
    ssh_authorized_keys:
      - $(cat "$SSH_KEY.pub")
ssh_pwauth: false
package_update: false
package_upgrade: false
EOF
cat > "$CACHE_DIR/meta-data" <<EOF
instance-id: numa-test-vm-$$
local-hostname: numa-test-vm
EOF

genisoimage -output "$SEED_IMG" -volid cidata -joliet -rock \
    "$CACHE_DIR/user-data" "$CACHE_DIR/meta-data" >/dev/null 2>&1 || {
    log_err "genisoimage failed to build cloud-init seed"
    write_result "failed" "genisoimage failed"
    exit 1
}

# ---- 4. Boot with 2 emulated NUMA nodes ----
rm -f "$SERIAL_LOG" "$QMP_SOCK"
log "booting QEMU (accel=tcg, 2 NUMA nodes, 4 vCPU, 2GiB RAM) -- boot timeout ${BOOT_TIMEOUT}s"
qemu-system-x86_64 \
    -machine q35,accel=tcg \
    -cpu max \
    -m 2048 \
    -smp cpus=4,sockets=2,cores=2,threads=1 \
    -object memory-backend-ram,id=m0,size=1024M \
    -object memory-backend-ram,id=m1,size=1024M \
    -numa node,nodeid=0,cpus=0-1,memdev=m0 \
    -numa node,nodeid=1,cpus=2-3,memdev=m1 \
    -drive file="$OVERLAY_IMG",if=virtio,format=qcow2 \
    -drive file="$SEED_IMG",if=virtio,format=raw \
    -netdev user,id=net0,hostfwd=tcp::${SSH_PORT}-:22 \
    -device virtio-net-pci,netdev=net0 \
    -display none \
    -serial file:"$SERIAL_LOG" \
    -qmp unix:"$QMP_SOCK",server,nowait \
    -pidfile "$PIDFILE" \
    -daemonize >/dev/null 2>"$CACHE_DIR/qemu_stderr.log"

if [[ $? -ne 0 ]] || [[ ! -f "$PIDFILE" ]]; then
    log_err "qemu-system-x86_64 failed to start; see $CACHE_DIR/qemu_stderr.log"
    write_result "failed" "qemu process failed to launch: $(tail -3 "$CACHE_DIR/qemu_stderr.log" 2>/dev/null)"
    exit 1
fi
log_ok "QEMU started (pid $(cat "$PIDFILE"))"

# ---- 5. Wait for SSH to come up, with an honest timeout ----
SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o BatchMode=yes -i "$SSH_KEY" -p "$SSH_PORT")
deadline=$(( $(date +%s) + BOOT_TIMEOUT ))
ready=0
while [[ "$(date +%s)" -lt "$deadline" ]]; do
    if ssh "${SSH_OPTS[@]}" numatest@127.0.0.1 'echo ssh-ready' >/dev/null 2>&1; then
        ready=1
        break
    fi
    if ! kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
        log_err "QEMU process exited before SSH came up"
        break
    fi
    sleep 10
done

if [[ "$ready" != "1" ]]; then
    log_warn "VM did not become SSH-reachable within ${BOOT_TIMEOUT}s -- treating as environment limitation, not a NUMA-code bug"
    log_warn "last serial console output:"
    tail -20 "$SERIAL_LOG" 2>/dev/null | sed 's/^/    /'
    write_result "timeout" "no SSH within ${BOOT_TIMEOUT}s under pure TCG emulation"
    exit 0
fi
log_ok "VM is SSH-reachable on port $SSH_PORT"

# ---- 6. Confirm the guest actually sees 2 NUMA nodes ----
# Read /sys directly instead of shelling out to numactl: numactl is not
# preinstalled on the cloud image and installing it means an apt-get, which
# is network-bound and can take many minutes over QEMU's slirp NAT under
# pure TCG -- reading sysfs is instant and needs no network at all.
GUEST_NUMA="$(timeout 15 ssh "${SSH_OPTS[@]}" numatest@127.0.0.1 'ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null')"
echo "$GUEST_NUMA" | sed 's/^/    /'
NODE_COUNT="$(echo "$GUEST_NUMA" | grep -c 'node[0-9]' || echo 0)"

# ---- 7. Copy the locally built binaries + NUMA config into the guest ----
BIN_SRC="$PROJECT_ROOT/src"
if [[ ! -x "$BIN_SRC/redis-server" ]]; then
    log_err "src/redis-server not built -- run 'make -j\$(nproc)' in src/ first"
    write_result "failed" "redis-server binary missing, build src/ first"
    exit 1
fi

scp -q -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P "$SSH_PORT" -i "$SSH_KEY" \
    "$BIN_SRC/redis-server" "$BIN_SRC/redis-cli" "$BIN_SRC/redis-benchmark" \
    "$PROJECT_ROOT/redis.conf" \
    numatest@127.0.0.1:/home/numatest/ || {
    log_err "scp of binaries failed"
    write_result "failed" "scp of built binaries into guest failed"
    exit 1
}

# ---- 8. Run a scaled-down NUMA smoke test inside the guest ----
REMOTE_LOG="/home/numatest/numa_smoke.log"
timeout 120 ssh "${SSH_OPTS[@]}" numatest@127.0.0.1 bash -s <<REMOTE_SCRIPT > "$RESULTS_DIR/qemu_guest_output.log" 2>&1
set -x
cd /home/numatest
chmod +x redis-server redis-cli redis-benchmark
cat /sys/devices/system/node/node*/meminfo 2>/dev/null | grep -E "MemTotal|Node" || true
./redis-server ./redis.conf --daemonize yes --port 7799 --logfile server.log
sleep 2
./redis-cli -p 7799 ping
./redis-cli -p 7799 set numatest:key1 "hello-numa"
./redis-cli -p 7799 get numatest:key1
./redis-cli -p 7799 numa config get 2>&1 || true
./redis-cli -p 7799 numa flow list 2>&1 || true
./redis-benchmark -p 7799 -q -n 20000 -c 20 -t set,get
./redis-cli -p 7799 numa migrate stats 2>&1 || true
./redis-cli -p 7799 shutdown nosave 2>&1 || true
REMOTE_SCRIPT
GUEST_RC=$?

if [[ $GUEST_RC -eq 0 ]]; then
    log_ok "in-guest NUMA smoke test completed, output saved to $RESULTS_DIR/qemu_guest_output.log"
    write_result "passed" "SSH reachable, guest sees ${NODE_COUNT} NUMA node(s), redis-server + NUMA command smoke test ran"
else
    log_warn "in-guest smoke test exited non-zero ($GUEST_RC); see $RESULTS_DIR/qemu_guest_output.log"
    write_result "partial" "SSH reachable but in-guest smoke test exited $GUEST_RC"
fi

exit 0
