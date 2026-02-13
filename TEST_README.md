# Composite LRU策略测试脚本使用说明

## 测试脚本说明

本项目提供两个测试脚本，用于验证Composite LRU策略的功能和性能：

| 脚本 | 用途 | 适用环境 |
|-----|------|---------|
| `test_composite_lru.sh` | 通用功能测试 | 任何Linux环境 |
| `test_composite_lru_cxl.sh` | CXL专用测试 | 带CXL设备的虚拟机/物理机 |

---

## 快速开始

### 1. 通用功能测试脚本

**基本用法：**
```bash
# 默认参数（端口6379，测试60秒）
./test_composite_lru.sh

# 指定端口和测试时长
./test_composite_lru.sh 6379 120
```

**测试内容：**
1. ✅ Redis服务器状态检查
2. ✅ Composite LRU策略加载验证
3. ✅ 基础数据操作（STRING/HASH/LIST/SET/ZSET）
4. ✅ 热度追踪测试（模拟热点/冷key访问）
5. ✅ NUMA迁移命令测试（NUMAMIGRATE）
6. ✅ CXL设备检测
7. ✅ 压力测试（持续指定秒数）
8. ✅ 策略执行日志检查
9. ✅ 内存使用检查
10. ✅ 测试数据清理

**输出示例：**
```
========================================
Composite LRU策略完整测试脚本
========================================

[INFO] 检查Redis服务器状态...
[PASS] Redis服务器运行正常
[INFO] 测试1: 检查Composite LRU策略是否加载...
[PASS] Composite LRU策略已正确加载到slot 1
...
[PASS] 所有测试完成！详细日志见: /tmp/composite_lru_test.log
```

---

### 2. CXL专用测试脚本

**基本用法：**
```bash
# 默认参数（端口6379，测试120秒）
./test_composite_lru_cxl.sh

# 指定端口和测试时长
./test_composite_lru_cxl.sh 6379 300
```

**CXL专用测试内容：**
1. ✅ 系统环境检查（内核版本、CPU、内存）
2. ✅ **CXL设备检测**（总线、设备、内存区域）
3. ✅ NUMA拓扑分析
4. ✅ **CXL感知数据创建**（小/中/大对象模拟内存层级）
4. ✅ **CXL访问模式模拟**（热点/温数据/冷数据）
5. ✅ NUMA迁移功能测试
6. ✅ **CXL性能测试**（混合访问模式，持续指定秒数）
7. ✅ 详细测试报告生成

**CXL环境要求：**
- Linux内核 >= 5.12（推荐 >= 6.0）
- 启用CXL支持：`CONFIG_CXL_BUS=y`
- 加载CXL模块：`modprobe cxl`
- （可选）numactl工具用于NUMA分析

---

## 在CXL虚拟机上测试步骤

### 步骤1：准备环境

```bash
# 1. 登录CXL虚拟机
ssh user@cxl-vm-ip

# 2. 检查CXL支持
ls /sys/bus/cxl  # 应该存在此目录

# 3. 检查CXL设备
ls /sys/bus/cxl/devices/

# 4. 检查NUMA支持
numactl --hardware
```

### 步骤2：部署Redis

```bash
# 1. 复制Redis源码到虚拟机
scp -r redis-CXL-in-v6.2.21 user@cxl-vm-ip:~/

# 2. 在虚拟机上编译
cd ~/redis-CXL-in-v6.2.21
make clean && make -j4

# 3. 验证编译结果
./src/redis-server --version
```

### 步骤3：运行测试

```bash
# 方法1：使用CXL专用脚本（推荐）
./test_composite_lru_cxl.sh 6379 300

# 方法2：使用通用脚本
./test_composite_lru.sh 6379 120
```

### 步骤4：查看结果

```bash
# 查看详细日志
cat /tmp/composite_lru_cxl_test.log

# 查看测试报告
cat /tmp/composite_lru_cxl_result.txt

# 查看Redis日志
grep "Composite LRU\|NUMA Strategy" /tmp/redis-*.log
```

---

## 预期测试结果

### 正常情况

```
[PASS] Redis服务器运行正常
[PASS] Composite LRU策略已正确加载到slot 1
[PASS] STRING/HASH/LIST/SET/ZSET操作正常
[PASS] NUMAMIGRATE命令正常
[PASS] 单key迁移成功
[PASS] 迁移后数据完整性验证通过
[PASS] CXL性能测试完成
```

### CXL设备检测

**有CXL设备时：**
```
[PASS] CXL总线已启用
[CXL] CXL设备列表:
  - mem0
    类型: cxl-mem
    内存: 536870912 bytes
```

**无CXL设备时：**
```
[WARN] 未检测到CXL总线
[INFO] 提示: 如果是CXL虚拟机，请确保内核已启用CXL支持
```

---

## 故障排查

### 问题1：Redis无法启动

**现象：**
```
[FAIL] Redis服务器未运行
```

**解决：**
```bash
# 检查端口占用
netstat -tlnp | grep 6379

# 使用其他端口
./src/redis-server --port 6380 --daemonize yes
./test_composite_lru.sh 6380 60
```

### 问题2：策略未加载

**现象：**
```
[WARN] 无法从日志确认策略加载状态
```

**解决：**
```bash
# 检查Redis日志
tail -100 /tmp/redis-test.log | grep -i "strategy\|lru"

# 确认编译包含策略模块
grep "numa_composite_lru" src/Makefile
```

### 问题3：CXL设备未检测到

**现象：**
```
[WARN] 未检测到CXL总线
```

**解决：**
```bash
# 检查内核配置
zcat /proc/config.gz | grep CXL

# 手动加载CXL模块
sudo modprobe cxl

# 检查dmesg
dmesg | grep -i cxl
```

### 问题4：NUMAMIGRATE命令不可用

**现象：**
```
[WARN] NUMAMIGRATE命令可能不支持
```

**解决：**
- 这是正常的，如果Key迁移模块未完全集成
- 检查05文档中的NUMAMIGRATE实现状态

---

## 高级用法

### 自定义测试参数

```bash
# 长时间压力测试（10分钟）
./test_composite_lru_cxl.sh 6379 600

# 指定Redis配置文件
./src/redis-server ./redis.conf --daemonize yes
./test_composite_lru.sh 6379 60
```

### 分析测试结果

```bash
# 提取性能数据
grep "OPS:" /tmp/composite_lru_cxl_test.log

# 提取错误信息
grep "\[FAIL\]" /tmp/composite_lru_cxl_test.log

# 生成图表数据
awk '/OPS:/{print $3}' /tmp/composite_lru_cxl_test.log > ops_data.txt
```

### 自动化测试

```bash
# 创建自动化测试脚本
cat > run_auto_test.sh << 'EOF'
#!/bin/bash
for duration in 60 120 300; do
    echo "Testing with duration=${duration}s"
    ./test_composite_lru_cxl.sh 6379 $duration
    sleep 10
done
EOF
chmod +x run_auto_test.sh
```

---

## 测试脚本对比

| 特性 | test_composite_lru.sh | test_composite_lru_cxl.sh |
|-----|----------------------|--------------------------|
| 执行时间 | 短（约1-2分钟） | 较长（根据参数） |
| CXL检测 | 基础检测 | 详细检测 |
| 数据对象 | 标准大小 | 分层大小（模拟CXL层级） |
| 访问模式 | 简单热点/冷key | CXL感知（热点/温/冷） |
| 性能测试 | 基础压力测试 | CXL优化压力测试 |
| 报告详细度 | 标准 | 详细（含系统信息） |
| 适用场景 | 功能验证 | CXL环境性能评估 |

---

## 联系与支持

如有问题，请检查：
1. Redis日志文件（`/tmp/redis-*.log`）
2. 测试脚本日志（`/tmp/composite_lru*.log`）
3. 系统dmesg日志（`dmesg | grep -i cxl`）
