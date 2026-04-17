#!/bin/bash
# ============================================================
# CXL 虚拟机一键环境安装脚本
# 用途：在 QEMU CXL 虚拟机内安装运行 NUMA 带宽基准测试
#       所需的全部 Python 环境和依赖
# 使用：bash setup_vm_env.sh
# ============================================================
set -euo pipefail

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()    { echo -e "[$(date +%H:%M:%S)] $*"; }
log_ok() { echo -e "${GREEN}[OK]${NC}    $*"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_err(){ echo -e "${RED}[ERR]${NC}   $*"; }

# 自动检测项目根目录（脚本所在位置的上两级）
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"

echo "╔════════════════════════════════════════════════════╗"
echo "║    CXL 虚拟机环境一键安装                           ║"
echo "╚════════════════════════════════════════════════════╝"
echo ""
log "项目根目录: $PROJECT_ROOT"

# ── 1. 检测 NUMA 环境 ──
echo ""
echo "══ 1. 检测 NUMA 环境 ══"
if command -v numactl &>/dev/null; then
    NUMA_NODES=$(numactl --hardware 2>/dev/null | grep "available:" | awk '{print $2}' || echo "0")
    if [[ "$NUMA_NODES" -ge 2 ]]; then
        log_ok "NUMA 多节点环境 ($NUMA_NODES 个节点)"
        numactl --hardware 2>/dev/null | grep -E "available|node [0-9]+ size|node distances" || true
    else
        log_warn "仅检测到 $NUMA_NODES 个 NUMA 节点，测试结果可能不包含跨节点迁移数据"
    fi
else
    log_warn "numactl 未安装，稍后将安装"
fi

# ── 2. 更新包管理器 ──
echo ""
echo "══ 2. 更新包管理器 ══"
log "运行 apt-get update..."
sudo apt-get update -qq 2>/dev/null || log_warn "apt-get update 失败，继续安装..."

# ── 3. 安装 Python3 + matplotlib ──
echo ""
echo "══ 3. 安装 Python3 + matplotlib ══"
sudo apt-get install -y python3 python3-pip python3-matplotlib 2>/dev/null || {
    log_warn "apt 安装 matplotlib 失败，尝试 pip 安装..."
    pip3 install matplotlib 2>/dev/null || python3 -m pip install matplotlib 2>/dev/null || {
        log_err "matplotlib 安装失败，可视化报告将不可用"
    }
}

# ── 4. 安装 Java（YCSB 依赖）──
echo ""
echo "══ 4. 安装 Java (YCSB 依赖) ══"
if command -v java &>/dev/null; then
    log_ok "Java 已安装: $(java -version 2>&1 | head -1)"
else
    log "安装 default-jdk..."
    sudo apt-get install -y default-jdk 2>/dev/null || {
        log_warn "default-jdk 安装失败，尝试 openjdk-17-jdk..."
        sudo apt-get install -y openjdk-17-jdk 2>/dev/null || \
        sudo apt-get install -y openjdk-11-jdk 2>/dev/null || \
        log_err "Java 安装失败，YCSB 将无法运行"
    }
fi

# ── 5. 安装其他工具 ──
echo ""
echo "══ 5. 安装工具依赖 (numactl, bc) ══"
sudo apt-get install -y numactl bc 2>/dev/null || log_warn "部分工具安装失败"

# ── 6. 编译 Redis ──
echo ""
echo "══ 6. 编译 Redis (MALLOC=libc) ══"
if [[ -f "$PROJECT_ROOT/src/redis-server" ]]; then
    log_ok "redis-server 已存在，跳过编译"
    log "如需重新编译，请运行: cd \"$PROJECT_ROOT\" && make clean && make MALLOC=libc -j\$(nproc)"
else
    log "开始编译 Redis..."
    cd "$PROJECT_ROOT"
    make MALLOC=libc -j"$(nproc)" 2>&1 | tail -5
    if [[ -f "$PROJECT_ROOT/src/redis-server" ]]; then
        log_ok "Redis 编译成功"
    else
        log_err "Redis 编译失败，请手动检查"
    fi
fi

# ── 7. 验证所有依赖 ──
echo ""
echo "══ 环境验证 ══"
echo "┌──────────────────┬─────────────────────────────────┐"

# Python3
if command -v python3 &>/dev/null; then
    PY_VER=$(python3 --version 2>&1)
    printf "│ %-16s │ ✅ %-29s │\n" "Python3" "$PY_VER"
else
    printf "│ %-16s │ ❌ %-29s │\n" "Python3" "未安装"
fi

# matplotlib
if python3 -c "import matplotlib; print(matplotlib.__version__)" 2>/dev/null; then
    MPL_VER=$(python3 -c "import matplotlib; print(matplotlib.__version__)" 2>/dev/null)
    printf "│ %-16s │ ✅ %-29s │\n" "matplotlib" "$MPL_VER"
else
    printf "│ %-16s │ ❌ %-29s │\n" "matplotlib" "未安装"
fi

# Java
if command -v java &>/dev/null; then
    JAVA_VER=$(java -version 2>&1 | head -1 | awk -F'"' '{print $2}')
    printf "│ %-16s │ ✅ %-29s │\n" "Java" "$JAVA_VER"
else
    printf "│ %-16s │ ❌ %-29s │\n" "Java" "未安装"
fi

# numactl
if command -v numactl &>/dev/null; then
    NUMA_NODES=$(numactl --hardware 2>/dev/null | grep "available:" | awk '{print $2}' || echo "?")
    printf "│ %-16s │ ✅ %-29s │\n" "numactl" "${NUMA_NODES} 个 NUMA 节点"
else
    printf "│ %-16s │ ❌ %-29s │\n" "numactl" "未安装"
fi

# bc
if command -v bc &>/dev/null; then
    printf "│ %-16s │ ✅ %-29s │\n" "bc" "可用"
else
    printf "│ %-16s │ ❌ %-29s │\n" "bc" "未安装"
fi

# redis-server
if [[ -x "$PROJECT_ROOT/src/redis-server" ]]; then
    RS_SIZE=$(du -h "$PROJECT_ROOT/src/redis-server" | awk '{print $1}')
    printf "│ %-16s │ ✅ %-29s │\n" "redis-server" "$RS_SIZE"
else
    printf "│ %-16s │ ❌ %-29s │\n" "redis-server" "未编译"
fi

# YCSB
YCSB_SH="$SCRIPT_DIR/ycsb-0.17.0/bin/ycsb.sh"
if [[ -f "$YCSB_SH" ]]; then
    printf "│ %-16s │ ✅ %-29s │\n" "YCSB" "0.17.0 (ycsb.sh)"
else
    printf "│ %-16s │ ❌ %-29s │\n" "YCSB" "未找到"
fi

echo "└──────────────────┴─────────────────────────────────┘"

# ── 8. 后续操作提示 ──
echo ""
echo "════════════════════════════════════════════════════"
echo "  环境准备完成！运行三阶段基准测试："
echo ""
echo "    cd \"$SCRIPT_DIR\""
echo "    bash run_bw_benchmark.sh --port 16379"
echo ""
echo "  注意：使用 16379 端口避免与 QEMU 冲突"
echo "════════════════════════════════════════════════════"
