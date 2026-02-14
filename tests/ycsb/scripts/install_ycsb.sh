#!/bin/bash
#
# YCSB 安装脚本
# 自动下载、编译和配置 YCSB for Redis

set -e

YCSB_VERSION="0.17.0"
YCSB_DIR="$(cd "$(dirname "$0")/.." && pwd)"
REDIS_DIR="$(dirname "$(dirname "$YCSB_DIR")")"

# 颜色定义
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}=== YCSB 安装脚本 ===${NC}"
echo "YCSB目录: $YCSB_DIR"
echo "Redis目录: $REDIS_DIR"

# 检查依赖
check_dependencies() {
    echo -e "\n${YELLOW}检查依赖...${NC}"
    
    local missing_deps=()
    
    if ! command -v java &> /dev/null; then
        missing_deps+=("java")
    fi
    
    if ! command -v maven &> /dev/null; then
        missing_deps+=("maven")
    fi
    
    if ! command -v python3 &> /dev/null; then
        missing_deps+=("python3")
    fi
    
    if [ ${#missing_deps[@]} -gt 0 ]; then
        echo -e "${RED}缺少依赖: ${missing_deps[*]}${NC}"
        echo "请安装:"
        echo "  Ubuntu/Debian: sudo apt-get install openjdk-11-jdk maven python3 python3-pip"
        echo "  CentOS/RHEL:   sudo yum install java-11-openjdk-devel maven python3 python3-pip"
        exit 1
    fi
    
    echo -e "${GREEN}所有依赖已安装${NC}"
}

# 下载 YCSB
download_ycsb() {
    echo -e "\n${YELLOW}下载 YCSB ${YCSB_VERSION}...${NC}"
    
    cd "$YCSB_DIR"
    
    if [ -d "ycsb-${YCSB_VERSION}" ]; then
        echo -e "${GREEN}YCSB 已存在，跳过下载${NC}"
        return 0
    fi
    
    local ycsb_url="https://github.com/brianfrankcooper/YCSB/releases/download/${YCSB_VERSION}/ycsb-${YCSB_VERSION}.tar.gz"
    
    echo "下载地址: $ycsb_url"
    if wget -q --show-progress "$ycsb_url" -O ycsb.tar.gz; then
        echo -e "${GREEN}下载成功${NC}"
    else
        echo -e "${RED}下载失败${NC}"
        exit 1
    fi
    
    echo "解压中..."
    tar -xzf ycsb.tar.gz
    rm ycsb.tar.gz
    
    echo -e "${GREEN}YCSB 解压完成${NC}"
}

# 配置 Redis 绑定
configure_redis_binding() {
    echo -e "\n${YELLOW}配置 Redis 绑定...${NC}"
    
    # 创建 YCSB Redis 配置文件
    cat > "$YCSB_DIR/redis-binding.conf" << 'EOF'
# YCSB Redis 绑定配置
redis.host=127.0.0.1
redis.port=6379
redis.timeout=2000
redis.database=0
EOF
    
    echo -e "${GREEN}Redis 绑定配置完成${NC}"
}

# 创建 YCSB 包装脚本
create_wrapper_scripts() {
    echo -e "\n${YELLOW}创建 YCSB 包装脚本...${NC}"
    
    cd "$YCSB_DIR"
    
    # 主 YCSB 运行脚本
    cat > "ycsb-run.sh" << 'EOF'
#!/bin/bash
# YCSB 运行包装脚本

YCSB_HOME="$(cd "$(dirname "$0")" && pwd)/ycsb-0.17.0"
WORKLOAD_DIR="$(cd "$(dirname "$0")" && pwd)/workloads"
RESULTS_DIR="$(cd "$(dirname "$0")" && pwd)/results"

# 默认参数
DATABASE="redis"
WORKLOAD="workloada"
OPERATION_COUNT=10000
RECORD_COUNT=10000
THREADS=50

# 解析参数
while [[ $# -gt 0 ]]; do
    case $1 in
        -db|--database)
            DATABASE="$2"
            shift 2
            ;;
        -w|--workload)
            WORKLOAD="$2"
            shift 2
            ;;
        -op|--operationcount)
            OPERATION_COUNT="$2"
            shift 2
            ;;
        -rc|--recordcount)
            RECORD_COUNT="$2"
            shift 2
            ;;
        -t|--threads)
            THREADS="$2"
            shift 2
            ;;
        -p|--phase)
            PHASE="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            exit 1
            ;;
    esac
done

# 检查 YCSB 是否存在
if [ ! -d "$YCSB_HOME" ]; then
    echo "错误: YCSB 未安装，请先运行 ./scripts/install_ycsb.sh"
    exit 1
fi

# 构建命令
YCSB_CMD="$YCSB_HOME/bin/ycsb"

if [ "$PHASE" = "load" ]; then
    echo "=== YCSB Load Phase ==="
    echo "Database: $DATABASE"
    echo "Workload: $WORKLOAD"
    echo "Record Count: $RECORD_COUNT"
    echo "Threads: $THREADS"
    
    "$YCSB_CMD" load "$DATABASE" \
        -P "$WORKLOAD_DIR/$WORKLOAD" \
        -p recordcount="$RECORD_COUNT" \
        -p threadcount="$THREADS" \
        -s \
        2>&1 | tee "$RESULTS_DIR/${WORKLOAD}_load_$(date +%Y%m%d_%H%M%S).log"
        
elif [ "$PHASE" = "run" ]; then
    echo "=== YCSB Run Phase ==="
    echo "Database: $DATABASE"
    echo "Workload: $WORKLOAD"
    echo "Operation Count: $OPERATION_COUNT"
    echo "Threads: $THREADS"
    
    "$YCSB_CMD" run "$DATABASE" \
        -P "$WORKLOAD_DIR/$WORKLOAD" \
        -p operationcount="$OPERATION_COUNT" \
        -p threadcount="$THREADS" \
        -s \
        2>&1 | tee "$RESULTS_DIR/${WORKLOAD}_run_$(date +%Y%m%d_%H%M%S).log"
else
    echo "用法: $0 -p <load|run> [选项]"
    echo ""
    echo "选项:"
    echo "  -db, --database       数据库类型 (默认: redis)"
    echo "  -w,  --workload       工作负载 (默认: workloada)"
    echo "  -op, --operationcount 操作数量 (默认: 10000)"
    echo "  -rc, --recordcount     记录数量 (默认: 10000)"
    echo "  -t,  --threads        线程数 (默认: 50)"
    echo "  -p,  --phase          阶段 (load 或 run)"
    exit 1
fi
EOF
    
    chmod +x "ycsb-run.sh"
    
    echo -e "${GREEN}包装脚本创建完成${NC}"
}

# 主函数
main() {
    echo -e "${BLUE}========================================${NC}"
    echo -e "${BLUE}      YCSB 安装和配置脚本              ${NC}"
    echo -e "${BLUE}========================================${NC}"
    
    check_dependencies
    download_ycsb
    configure_redis_binding
    create_wrapper_scripts
    
    echo -e "\n${GREEN}========================================${NC}"
    echo -e "${GREEN}      YCSB 安装完成!                  ${NC}"
    echo -e "${GREEN}========================================${NC}"
    echo ""
    echo "使用方法:"
    echo "  1. 加载数据: ./ycsb-run.sh -p load -w workloada -rc 100000"
    echo "  2. 运行测试: ./ycsb-run.sh -p run -w workloada -op 100000 -t 50"
    echo ""
    echo "工作负载文件位于: workloads/"
    echo "测试结果保存于: results/"
}

main "$@"
