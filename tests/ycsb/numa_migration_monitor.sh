#!/bin/bash
# ============================================================================
# NUMA 迁移诊断与分析工具
# 
# 用途: 实时监控 NUMA 迁移状态，分析热点分布和迁移效果
# 用法: ./numa_migration_monitor.sh [选项]
# ============================================================================

set -euo pipefail

REDIS_CLI="${REDIS_CLI:-./src/redis-cli}"
REDIS_HOST="${REDIS_HOST:-127.0.0.1}"
REDIS_PORT="${REDIS_PORT:-6379}"

# 颜色
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# ── 系统信息 ────────────────────────────────────────────────────────────────
show_system_info() {
    echo -e "${BOLD}${CYAN}=== 系统 NUMA 拓扑 ===${NC}"
    
    if command -v numactl &>/dev/null; then
        numactl --hardware
        echo ""
        numactl --show
    else
        echo "numactl 未安装"
    fi
    
    echo -e "\n${BOLD}${CYAN}=== NUMA 节点内存使用 ===${NC}"
    if command -v numastat &>/dev/null; then
        numastat
    else
        echo "numastat 未安装"
    fi
}

# ── Redis NUMA 配置 ────────────────────────────────────────────────────────
show_redis_config() {
    echo -e "\n${BOLD}${CYAN}=== Redis NUMA 配置 ===${NC}"
    
    $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT NUMA CONFIG GET 2>/dev/null || {
        echo "无法获取 NUMA 配置 (可能未启用 NUMA 支持)"
        return 1
    }
}

# ── 迁移统计 ────────────────────────────────────────────────────────────────
show_migration_stats() {
    echo -e "\n${BOLD}${CYAN}=== 迁移统计 ===${NC}"
    
    local stats
    stats=$($REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT NUMA MIGRATE STATS 2>/dev/null)
    
    if [[ $? -eq 0 ]]; then
        echo "$stats" | while read -r line; do
            echo "  $line"
        done
    else
        echo "无法获取迁移统计"
    fi
}

# ── 分配统计 ────────────────────────────────────────────────────────────────
show_allocation_stats() {
    echo -e "\n${BOLD}${CYAN}=== 分配统计 ===${NC}"
    
    $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT NUMA CONFIG STATS 2>/dev/null || {
        echo "无法获取分配统计"
    }
}

# ── 策略状态 ────────────────────────────────────────────────────────────────
show_strategy_status() {
    echo -e "\n${BOLD}${CYAN}=== 策略状态 ===${NC}"
    
    $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT NUMA STRATEGY LIST 2>/dev/null || {
        echo "无法获取策略状态"
    }
}

# ── Key 元数据 ──────────────────────────────────────────────────────────────
show_key_metadata() {
    local key="${1:-}"
    
    if [[ -z "$key" ]]; then
        echo -e "\n${BOLD}${CYAN}=== 示例 Key 元数据 ===${NC}"
        echo "用法: $0 --key <key>"
        return
    fi
    
    echo -e "\n${BOLD}${CYAN}=== Key '$key' 的 NUMA 元数据 ===${NC}"
    
    $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT NUMA MIGRATE INFO "$key" 2>/dev/null || {
        echo "无法获取 Key 元数据 (Key 可能不存在)"
    }
}

# ── 持续监控 ────────────────────────────────────────────────────────────────
monitor_loop() {
    local interval="${1:-5}"
    
    echo -e "${BOLD}${CYAN}开始监控 (间隔 ${interval}s, Ctrl+C 停止)${NC}"
    echo ""
    
    while true; do
        clear
        echo -e "${BOLD}$(date '+%Y-%m-%d %H:%M:%S') - NUMA 迁移监控${NC}"
        echo "=========================================="
        
        show_migration_stats
        show_allocation_stats
        
        echo -e "\n${YELLOW}下次更新: ${interval}s 后...${NC}"
        sleep "$interval"
    done
}

# ── 热点分析 ────────────────────────────────────────────────────────────────
analyze_hotspots() {
    echo -e "\n${BOLD}${CYAN}=== 热点 Key 分析 ===${NC}"
    
    # 使用 SLOWLOG 分析高频访问
    echo "最近的慢查询 (可能包含热点 Key):"
    $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT SLOWLOG GET 10 2>/dev/null || true
    
    # 分析内存使用
    echo -e "\n内存使用:"
    $REDIS_CLI -h $REDIS_HOST -p $REDIS_PORT INFO memory 2>/dev/null | grep -E "used_memory_human|used_memory_peak" || true
}

# ── 使用帮助 ────────────────────────────────────────────────────────────────
usage() {
    cat <<EOF
用法: $(basename "$0") [选项]

选项:
  --system          显示系统 NUMA 拓扑
  --config          显示 Redis NUMA 配置
  --migration       显示迁移统计
  --allocation      显示分配统计
  --strategy        显示策略状态
  --key <key>       显示指定 Key 的 NUMA 元数据
  --hotspots        分析热点 Key
  --monitor [N]     持续监控 (间隔 N 秒，默认 5)
  --all             显示所有信息
  --host HOST       Redis 地址 (默认: 127.0.0.1)
  --port PORT       Redis 端口 (默认: 6379)
  --help            显示此帮助

示例:
  $(basename "$0") --all
  $(basename "$0") --monitor 10
  $(basename "$0") --key user:100
  $(basename "$0") --port 6380 --migration
EOF
    exit 0
}

# ── 主流程 ─────────────────────────────────────────────────────────────────
main() {
    local show_all=0
    local action=""
    local key=""
    local monitor_interval=5
    
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --system)
                [[ $show_all -eq 1 ]] && show_system_info
                action="system"
                shift
                ;;
            --config)
                [[ $show_all -eq 1 ]] && show_redis_config
                action="config"
                shift
                ;;
            --migration)
                [[ $show_all -eq 1 ]] && show_migration_stats
                action="migration"
                shift
                ;;
            --allocation)
                [[ $show_all -eq 1 ]] && show_allocation_stats
                action="allocation"
                shift
                ;;
            --strategy)
                [[ $show_all -eq 1 ]] && show_strategy_status
                action="strategy"
                shift
                ;;
            --key)
                key="$2"
                action="key"
                shift 2
                ;;
            --hotspots)
                [[ $show_all -eq 1 ]] && analyze_hotspots
                action="hotspots"
                shift
                ;;
            --monitor)
                if [[ -n "${2:-}" && "$2" =~ ^[0-9]+$ ]]; then
                    monitor_interval="$2"
                    shift 2
                else
                    shift
                fi
                action="monitor"
                ;;
            --all)
                show_all=1
                action="all"
                shift
                ;;
            --host)
                REDIS_HOST="$2"
                shift 2
                ;;
            --port)
                REDIS_PORT="$2"
                shift 2
                ;;
            --help|-h)
                usage
                ;;
            *)
                echo "未知参数: $1"
                usage
                ;;
        esac
    done
    
    if [[ -z "$action" ]]; then
        usage
    fi
    
    if [[ "$action" == "all" ]]; then
        show_system_info
        show_redis_config
        show_migration_stats
        show_allocation_stats
        show_strategy_status
        analyze_hotspots
    elif [[ "$action" == "system" ]]; then
        show_system_info
    elif [[ "$action" == "config" ]]; then
        show_redis_config
    elif [[ "$action" == "migration" ]]; then
        show_migration_stats
    elif [[ "$action" == "allocation" ]]; then
        show_allocation_stats
    elif [[ "$action" == "strategy" ]]; then
        show_strategy_status
    elif [[ "$action" == "key" ]]; then
        show_key_metadata "$key"
    elif [[ "$action" == "hotspots" ]]; then
        analyze_hotspots
    elif [[ "$action" == "monitor" ]]; then
        monitor_loop "$monitor_interval"
    fi
}

main "$@"
