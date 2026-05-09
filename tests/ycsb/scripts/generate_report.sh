#!/bin/bash
# ============================================================================
# 独立可视化报告生成脚本
#
# 用途：在宿主机上读取 CXL 虚拟机中产生的测试结果并生成可视化报告
#       通过 virtio-9p 共享文件系统，VM 中的测试结果在宿主机上可直接访问
#
# 使用：
#   bash generate_report.sh [结果目录路径]
#   bash generate_report.sh                    # 自动查找最新结果
#   bash generate_report.sh results/bw_bench_20260416_232722
#
# 输出：
#   <结果目录>/benchmark_report.png (2400x1800 像素, 150 DPI)
# ============================================================================

set -euo pipefail

# ============ 路径配置 ============
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
YCSB_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
RESULTS_BASE="$YCSB_DIR/results"
VISUALIZE_SCRIPT="$SCRIPT_DIR/visualize_bw_benchmark.py"
VENV_DIR="$SCRIPT_DIR/.venv"

# ============ 颜色输出 ============
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

log()     { echo -e "${BLUE}[INFO]${NC} $*"; }
log_ok()  { echo -e "${GREEN}[OK]${NC}   $*"; }
log_err() { echo -e "${RED}[ERR]${NC}  $*"; }

# ============ 帮助信息 ============
usage() {
    cat <<EOF
用法: $(basename "$0") [结果目录路径]

功能:
  在宿主机上读取 CXL 虚拟机中产生的测试结果并生成可视化报告

参数:
  结果目录路径    可选，包含 metrics.csv 的测试结果目录
                 若不指定，自动查找 results/ 下最新的 bw_bench_* 目录

输出:
  <结果目录>/benchmark_report.png (2400x1800 像素, 150 DPI)

示例:
  $(basename "$0")                              # 自动查找最新结果
  $(basename "$0") results/bw_bench_20260416_232722  # 指定结果目录

依赖:
  python3 + python3-venv (宿主机需安装)
  matplotlib 和 pandas 会自动安装到脚本目录下的 .venv 中
EOF
    exit 0
}

# ============ 主流程 ============

# 处理帮助参数
if [[ "${1:-}" == "-h" || "${1:-}" == "--help" ]]; then
    usage
fi

# 确定结果目录
if [[ $# -ge 1 ]]; then
    RESULT_DIR="$1"
else
    # 自动查找最新的 bw_bench_* 目录
    RESULT_DIR=$(find "$RESULTS_BASE" -maxdepth 1 -type d -name 'bw_bench_*' -printf '%T@ %p\n' 2>/dev/null | sort -rn | head -1 | cut -d' ' -f2-)
    if [[ -z "$RESULT_DIR" ]]; then
        log_err "未找到测试结果目录"
        log "用法: $0 [结果目录路径]"
        log "请先运行 run_bw_benchmark.sh 生成测试结果"
        exit 1
    fi
    log "自动选择最新结果目录: $RESULT_DIR"
fi

# 验证结果目录存在
if [[ ! -d "$RESULT_DIR" ]]; then
    log_err "目录不存在: $RESULT_DIR"
    exit 1
fi

# 验证 metrics.csv 存在
METRICS_CSV="$RESULT_DIR/metrics.csv"
if [[ ! -f "$METRICS_CSV" ]]; then
    log_err "metrics.csv 不存在: $METRICS_CSV"
    log "请确认该目录是有效的测试结果目录"
    exit 1
fi

# 检查可视化脚本
if [[ ! -f "$VISUALIZE_SCRIPT" ]]; then
    log_err "可视化脚本不存在: $VISUALIZE_SCRIPT"
    exit 1
fi

# 检查 python3
if ! command -v python3 &>/dev/null; then
    log_err "python3 未安装"
    log "请安装: apt install python3"
    exit 1
fi

# 选择 Python 解释器：优先系统 python3（已有 matplotlib 即可），否则走 venv
PYTHON=""
if python3 -c "import matplotlib" 2>/dev/null; then
    PYTHON="python3"
    log "使用系统 Python (matplotlib 已可用)"
else
    # 系统缺 matplotlib，尝试 venv
    VENV_PYTHON="$VENV_DIR/bin/python"
    if [[ -x "$VENV_PYTHON" ]] && "$VENV_PYTHON" -c "import matplotlib" 2>/dev/null; then
        PYTHON="$VENV_PYTHON"
        log "使用已有 venv: $VENV_DIR"
    else
        # venv 不存在或已损坏，重建
        if [[ -d "$VENV_DIR" ]]; then
            log "检测到损坏的 venv，尝试删除重建..."
            rm -rf "$VENV_DIR" 2>/dev/null || {
                log_err "无法删除损坏的 venv (权限不足)"
                log "请手动执行: sudo rm -rf $VENV_DIR"
                exit 1
            }
        fi
        log "创建 Python 虚拟环境..."
        python3 -m venv "$VENV_DIR" || {
            log_err "创建 venv 失败，请确认已安装 python3-venv"
            log "  sudo apt install python3-venv"
            exit 1
        }
        log "安装可视化依赖 (matplotlib)..."
        "$VENV_DIR/bin/pip" install --quiet matplotlib || {
            log_err "依赖安装失败"
            exit 1
        }
        log_ok "虚拟环境就绪: $VENV_DIR"
        PYTHON="$VENV_PYTHON"
    fi
fi

# 输出路径：优先写入结果目录，不可写时回退到 results/ 根目录
OUTPUT_PNG="$RESULT_DIR/benchmark_report.png"
if ! touch "$OUTPUT_PNG" 2>/dev/null; then
    BASENAME="$(basename "$RESULT_DIR")"
    OUTPUT_PNG="$RESULTS_BASE/${BASENAME}_report.png"
    log "${YELLOW}结果目录不可写（root 所有），输出回退至:${NC}"
    log "  $OUTPUT_PNG"
fi
rm -f "$OUTPUT_PNG" 2>/dev/null

# 生成报告
log "生成可视化报告..."
log "  数据源: $METRICS_CSV"
log "  输出至: $OUTPUT_PNG"

"$PYTHON" "$VISUALIZE_SCRIPT" \
    --input "$METRICS_CSV" \
    --output "$OUTPUT_PNG" \
    --dpi 150

# 验证输出
if [[ -f "$OUTPUT_PNG" ]]; then
    log_ok "报告已生成: $OUTPUT_PNG"
    # 显示文件大小
    size_kb=$(du -k "$OUTPUT_PNG" | cut -f1)
    log "  文件大小: ${size_kb} KB"
else
    log_err "报告生成失败"
    exit 1
fi
