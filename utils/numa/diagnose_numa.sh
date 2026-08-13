#!/bin/bash

# NUMA诊断和CXL环境检查工具

echo "========================================"
echo "NUMA和CXL环境诊断工具"
echo "========================================"

echo "[1] 基本NUMA信息"
echo "------------------"
numactl --hardware 2>/dev/null || echo "numactl不可用"

echo -e "\n[2] CPU拓扑信息"
echo "------------------"
lscpu | grep -E "(NUMA|Socket|Core|Thread)"

echo -e "\n[3] 内存信息"
echo "------------------"
free -h

echo -e "\n[4] 检查CXL设备"
echo "------------------"
if lsmod | grep -q cxl; then
    echo "✓ CXL内核模块已加载"
    modinfo cxl 2>/dev/null | head -5
else
    echo "✗ CXL内核模块未加载"
fi

# 检查/sys/bus/cxl是否存在
if [ -d "/sys/bus/cxl" ]; then
    echo "✓ 发现CXL总线"
    echo "CXL设备:"
    ls /sys/bus/cxl/devices/ 2>/dev/null || echo "无CXL设备"
else
    echo "✗ 未发现CXL总线"
fi

echo -e "\n[5] 检查NUMA内存节点"
echo "------------------"
# 检查/proc/self/numa_maps
if [ -r "/proc/self/numa_maps" ]; then
    echo "当前进程NUMA映射示例:"
    head -10 /proc/self/numa_maps 2>/dev/null
fi

# 检查各节点内存信息
echo -e "\n各NUMA节点内存信息:"
for node_dir in /sys/devices/system/node/node*/; do
    if [ -d "$node_dir" ]; then
        node_num=$(basename "$node_dir" | cut -d'-' -f2)
        mem_total=$(cat "$node_dir/meminfo" 2>/dev/null | grep MemTotal | awk '{print $4}' || echo "N/A")
        mem_free=$(cat "$node_dir/meminfo" 2>/dev/null | grep MemFree | awk '{print $4}' || echo "N/A")
        echo "Node $node_num: Total=${mem_total}kB Free=${mem_free}kB"
    fi
done

echo -e "\n[6] Redis NUMA支持检查"
echo "------------------"
if [ -f "./src/redis-server" ]; then
    echo "检查Redis编译时的NUMA支持:"
    ldd ./src/redis-server | grep numa
    echo ""
    echo "Redis版本信息:"
    ./src/redis-server --version
else
    echo "Redis服务器未找到"
fi

echo -e "\n[7] 系统调用检查"
echo "------------------"
echo "检查可用的NUMA系统调用:"
for func in numa_available numa_max_node numa_node_size numa_alloc_onnode numa_free; do
    if grep -q "$func" /usr/include/numa.h 2>/dev/null; then
        echo "✓ $func"
    else
        echo "✗ $func (可能需要安装numactl-devel包)"
    fi
done

echo -e "\n[8] 建议"
echo "------------------"
echo "如果要测试真正的NUMA负载均衡效果，需要:"
echo "1. 多个物理NUMA节点的硬件环境"
echo "2. 或者支持CXL内存扩展的虚拟化环境"
echo "3. 正确配置的NUMA策略"

echo -e "\n诊断完成！"