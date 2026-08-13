#!/bin/bash

# 验证NUMA配置的脚本
echo "=== NUMA配置验证脚本 ==="

# 1. 检查系统NUMA节点
echo "1. 系统NUMA节点信息："
numactl --hardware

# 2. 检查CPU拓扑
echo -e "\n2. CPU拓扑信息："
lscpu | grep -E "(NUMA|Core|Socket|^CPU)"

# 3. 检查内存信息
echo -e "\n3. 内存信息："
free -h

# 4. 检查大页内存（如果可用）
echo -e "\n4. 大页内存信息："
if [ -f /proc/meminfo ]; then
    grep -i huge /proc/meminfo
fi

# 5. 检查NUMA距离
echo -e "\n5. NUMA距离矩阵："
numastat -m 2>/dev/null || echo "numastat命令不可用"

# 6. 验证CXL相关文件
echo -e "\n6. CXL相关检查："
if [ -f /tmp/cxl_mem.raw ]; then
    echo "✓ CXL内存文件存在: $(ls -lh /tmp/cxl_mem.raw)"
else
    echo "✗ CXL内存文件不存在"
fi

# 7. 检查QEMU是否支持CXL
echo -e "\n7. QEMU CXL支持检查："
if command -v qemu-system-x86_64 &> /dev/null; then
    qemu-system-x86_64 -machine help | grep -i cxl && echo "✓ QEMU支持CXL" || echo "✗ QEMU可能不支持CXL"
else
    echo "✗ QEMU未安装"
fi

echo -e "\n=== 验证完成 ==="