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
  python3 + matplotlib (宿主机需安装)
  pip3 install matplotlib
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
    RESULT_DIR=$(ls -td "$RESULTS_BASE"/bw_bench_* 2>/dev/null | head -1)
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

# 检查 python3 + matplotlib
if ! command -v python3 &>/dev/null; then
    log_err "python3 未安装"
    log "请安装: apt install python3 或使用其他包管理器"
    exit 1
fi

if ! python3 -c "import matplotlib" 2>/dev/null; then
    log_err "matplotlib 未安装"
    log "请安装: pip3 install matplotlib"
    exit 1
fi

# 输出路径（与 run_bw_benchmark.sh 一致）
OUTPUT_PNG="$RESULT_DIR/benchmark_report.png"

# 生成报告
log "生成可视化报告..."
log "  数据源: $METRICS_CSV"
log "  输出至: $OUTPUT_PNG"

python3 "$VISUALIZE_SCRIPT" \
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
