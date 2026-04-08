# YCSB 测试使用教程

## 概述

本项目集成了 **YCSB（Yahoo! Cloud Serving Benchmark）** 作为压力测试框架，提供三种测试模式：基线测试、压力测试和 NUMA 迁移测试。通过这套工具，你可以全面评估 Redis 在不同负载下的性能表现和 NUMA 迁移效果。

## 目录结构

```
tests/ycsb/
├── run_ycsb.sh                      # 统一测试入口（主脚本）
├── run_numa_migration_test.sh       # NUMA 迁移测试
├── numa_migration_monitor.sh        # 迁移监控工具
├── workloads/
│   ├── workload_baseline            # 基线测试配置
│   ├── workload_stress              # 压力测试配置
│   └── workload_numa_migration      # NUMA 迁移测试配置
├── scripts/
│   ├── install_ycsb.sh              # YCSB 安装脚本
│   └── analyze_results.py           # 结果分析工具
├── results/                         # 测试结果输出目录
├── ycsb-0.17.0/                     # YCSB 工具包
└── legacy/                          # 历史脚本归档
```

## 环境准备

### 系统要求

- Linux 系统（推荐 Ubuntu 20.04+ 或 CentOS 8+）
- 已编译的 Redis（`src/redis-server`）
- Java 11+（YCSB 依赖）
- Python 3（结果分析工具依赖）

### 安装步骤

#### 1. 编译 Redis

```bash
cd /path/to/redis-cxl
cd src && make -j$(nproc)
cd ..
```

#### 2. 安装 YCSB

```bash
cd tests/ycsb
./scripts/install_ycsb.sh
```

安装脚本会自动：
- 检查 Java、Maven 是否安装
- 下载 YCSB 0.17.0
- 编译 Redis binding
- 创建包装脚本

**手动安装（如脚本失败）**：

```bash
# 安装依赖
sudo apt-get install openjdk-11-jdk maven python3 -y

# 下载 YCSB
wget https://github.com/brianfrankcooper/YCSB/releases/download/0.17.0/ycsb-0.17.0.tar.gz
tar xfvz ycsb-0.17.0.tar.gz

# 编译 Redis binding
cd ycsb-0.17.0
mvn -pl site.ycsb:redis-binding -am clean package
```

#### 3. 验证安装

```bash
# 检查 YCSB
./ycsb-0.17.0/bin/ycsb --help

# 检查 Redis
../src/redis-server --version

# 检查 Java
java -version
```

## 测试模式详解

### 模式 1：基线测试（Baseline）

**用途**：验证功能正确性，日常开发回归测试

**特点**：
- 数据量小（10 万条记录）
- 线程数低（4 线程）
- 均匀访问分布
- 耗时约 2 分钟

**运行**：

```bash
cd tests/ycsb
./run_ycsb.sh --mode baseline
```

**参数**：

```bash
./run_ycsb.sh --mode baseline \
    --host 127.0.0.1 \
    --port 6379 \
    --maxmem 8gb \
    --output-dir results/
```

**工作负载配置** (`workloads/workload_baseline`)：

```properties
recordcount=100000              # 记录数
operationcount=100000           # 操作数
workload=site.ycsb.workloads.CoreWorkload
readproportion=0.5              # 读比例 50%
updateproportion=0.5            # 更新比例 50%
scanproportion=0                # 扫描比例 0%
insertproportion=0              # 插入比例 0%
requestdistribution=uniform     # 均匀分布
fieldcount=1                    # 字段数
fieldlength=1024                # Value 大小 1KB
redis.host=127.0.0.1
redis.port=6379
```

**预期输出**：

```
========================================
YCSB Baseline Test
========================================
Starting Redis server...
Loading data...
[LOAD] Operations: 100000
[LOAD] Runtime(ms): 15000
[LOAD] Throughput(ops/sec): 6666.67

Running benchmark...
[READ] Operations: 50000
[READ] AverageLatency(us): 150
[READ] Throughput(ops/sec): 33333.33

[UPDATE] Operations: 50000
[UPDATE] AverageLatency(us): 200
[UPDATE] Throughput(ops/sec): 25000

========================================
Summary
========================================
Overall Throughput: 50000 ops/sec
Overall Average Latency: 175 us
```

---

### 模式 2：压力测试（Stress）

**用途**：评估极限性能，验证 NUMA 迁移策略

**特点**：
- 数据量大（100 万条记录，~8GB）
- 高并发（32 线程）
- Hotspot 访问分布（80/20 法则）
- 耗时 10-20 分钟

**运行**：

```bash
cd tests/ycsb
./run_ycsb.sh --mode stress
```

**参数**：

```bash
./run_ycsb.sh --mode stress \
    --host 127.0.0.1 \
    --port 6379 \
    --maxmem 16gb \
    --output-dir results/
```

**工作负载配置** (`workloads/workload_stress`)：

```properties
recordcount=1000000             # 记录数 100 万
operationcount=1000000          # 操作数 100 万
workload=site.ycsb.workloads.CoreWorkload
readproportion=0.3              # 读比例 30%
updateproportion=0.6            # 更新比例 60%
insertproportion=0.1            # 插入比例 10%
requestdistribution=hotspot     # 热点分布
hotspotdatafraction=0.2         # 20% 的数据
hotspotopnfraction=0.8          # 80% 的操作
fieldcount=1
fieldlength=8192                # Value 大小 8KB
redis.host=127.0.0.1
redis.port=6379
redis.timeout=10000
threadcount=32                  # 32 线程
```

**为什么使用 Hotspot 分布？**

Hotspot 模拟真实场景中的"二八定律"：20% 的 Key 接收 80% 的访问。这正是 NUMA 迁移最能发挥作用的场景——热点 Key 会被自动迁移到最优节点。

**预期输出**：

```
========================================
YCSB Stress Test
========================================
Starting Redis server...
Loading data...
[LOAD] Operations: 1000000
[LOAD] Runtime(ms): 120000
[LOAD] Throughput(ops/sec): 8333.33

Running benchmark...
[READ] Operations: 300000
[READ] AverageLatency(us): 180
[READ] p95Latency(us): 350
[READ] p99Latency(us): 800
[READ] Throughput(ops/sec): 50000

[UPDATE] Operations: 600000
[UPDATE] AverageLatency(us): 220
[UPDATE] p95Latency(us): 450
[UPDATE] p99Latency(us): 1000
[UPDATE] Throughput(ops/sec): 40000

[INSERT] Operations: 100000
[INSERT] AverageLatency(us): 250
[INSERT] Throughput(ops/sec): 8000

========================================
Summary
========================================
Overall Throughput: 98000 ops/sec
Overall Average Latency: 210 us
Memory Used: 8.5 GB
NUMA Migrations: 1523
```

---

### 模式 3：NUMA 迁移测试

**用途**：专门验证 NUMA 迁移策略的效果，观察跨节点数据迁移

**特点**：
- Zipfian 访问分布（α=0.99，极强热点）
- 双 NUMA 节点线程群
- 观察迁移统计
- 耗时 15-20 分钟

**运行**：

```bash
cd tests/ycsb
./run_numa_migration_test.sh
```

**参数**：

```bash
./run_numa_migration_test.sh \
    --mode full \
    --port 6379 \
    --threshold 4
```

**模式选择**：

| 模式 | 记录数 | 操作数 | 线程数 | 耗时 | 用途 |
|------|-------|-------|-------|------|------|
| `quick` | 10 万 | 50 万 | 8 | ~5 分钟 | 快速验证 |
| `full` | 50 万 | 200 万 | 16 | ~15 分钟 | 完整测试 |

**工作负载配置** (`workloads/workload_numa_migration`)：

```properties
recordcount=500000              # 记录数 50 万
operationcount=2000000          # 操作数 200 万
workload=site.ycsb.workloads.CoreWorkload
readproportion=0.4              # 读 40%
updateproportion=0.5            # 更新 50%
insertproportion=0.1            # 插入 10%
requestdistribution=zipfian     # Zipfian 分布
zipfianconstant=0.99            # α=0.99（极强热点）
fieldcount=1
fieldlength=2048                # Value 大小 2KB
redis.host=127.0.0.1
redis.port=6379
threadcount=16
```

**Zipfian vs Hotspot**：

| 分布 | 热点集中度 | 适用场景 |
|------|-----------|---------|
| Hotspot | 80% 操作在 20% 数据上 | 一般压力测试 |
| Zipfian (α=0.99) | ~1% 的 Key 接收 90% 访问 | 极端迁移测试 |

**执行流程**：

```
1. 前置检查
   ├── Redis 是否运行
   ├── YCSB 是否安装
   └── NUMA 环境是否正常

2. 启动 Redis
   └── 16GB 内存限制

3. 配置 NUMA 策略
   └── 设置为交错分配

4. 加载数据
   └── 分布在两个 NUMA 节点

5. 热点访问测试
   ├── 节点 0 线程群（访问热点）
   └── 节点 1 线程群（访问热点）

6. 查看迁移结果
   ├── 迁移统计
   ├── 策略状态
   └── 热点 Key 元数据
```

**预期输出**：

```
========================================
NUMA Migration Test (Full Mode)
========================================
Checking environment...
  Redis: Running
  YCSB: Installed
  NUMA: 2 nodes available

Starting test...
Loading 500000 records...
Load completed in 60s

Running migration workload...
Operations: 2000000
Runtime: 900s
Throughput: 2222 ops/sec

Checking migration stats...
Total migrations: 4521
Successful: 4498
Failed: 23
Bytes migrated: 9.2 GB

Hot keys migrated:
  user:12345 -> Node 1 (hotness: 7)
  session:abc -> Node 0 (hotness: 6)
  order:9999 -> Node 1 (hotness: 7)

Test completed successfully!
```

## 监控工具使用

### numa_migration_monitor.sh

实时监控 NUMA 迁移状态的工具。

#### 显示所有信息

```bash
./numa_migration_monitor.sh --all
```

#### 显示系统 NUMA 拓扑

```bash
./numa_migration_monitor.sh --system
```

输出示例：
```
NUMA System Topology
====================
Available nodes: 2
Node 0:
  CPUs: 0-15
  Memory: 32 GB
  Free: 18 GB
Node 1:
  CPUs: 16-31
  Memory: 32 GB
  Free: 20 GB
```

#### 显示 Redis NUMA 配置

```bash
./numa_migration_monitor.sh --config
```

#### 显示迁移统计

```bash
./numa_migration_monitor.sh --migration
```

#### 显示分配统计

```bash
./numa_migration_monitor.sh --allocation
```

#### 查看指定 Key 的元数据

```bash
./numa_migration_monitor.sh --key user:100
```

输出示例：
```
Key: user:100
Type: string
Current Node: 0
Hotness Level: 5
Access Count: 1234
Last Access: 2026-04-08 15:30:45
Memory Footprint: 2048 bytes
```

#### 分析热点 Key

```bash
./numa_migration_monitor.sh --hotspots
```

#### 持续监控

```bash
# 每 5 秒刷新
./numa_migration_monitor.sh --monitor 5

# 每 10 秒刷新
./numa_migration_monitor.sh --monitor 10
```

按 `Ctrl+C` 停止监控。

## 结果分析

### 查看测试结果

每次测试在 `results/<mode>_<timestamp>/` 下生成：

```
results/
├── baseline_20260408_143000/
│   ├── load.txt              # Load 阶段详细日志
│   ├── run.txt               # Run 阶段详细日志
│   └── sysinfo.txt           # 系统信息
└── stress_20260408_144500/
    ├── load.txt
    ├── run.txt
    └── sysinfo.txt
```

### 使用分析工具

```bash
cd tests/ycsb
./scripts/analyze_results.py results/
```

**功能**：
- 解析 YCSB 日志（吞吐量、延迟分布）
- 解析 Redis 统计（内存使用、命中率）
- 性能排名
- 生成 JSON 报告

**输出示例**：

```
========================================
YCSB Results Analysis
========================================

Test: stress_20260408_144500
----------------------------------------
Load Phase:
  Records Loaded: 1000000
  Load Time: 120s
  Load Throughput: 8333 ops/s

Run Phase:
  Total Operations: 1000000
  Runtime: 600s
  Overall Throughput: 1666 ops/s

  Operation Breakdown:
    READ:     300000 ops, Avg: 180us, P95: 350us, P99: 800us
    UPDATE:   600000 ops, Avg: 220us, P95: 450us, P99: 1000us
    INSERT:   100000 ops, Avg: 250us, P95: 500us, P99: 1200us

  Latency Distribution:
    p50:  150us
    p90:  400us
    p95:  500us
    p99:  1000us
    p99.9: 2500us

NUMA Statistics:
  Total Migrations: 1523
  Bytes Migrated: 9.2 GB
  Migration Overhead: 2.3%
```

### 手动查看结果

```bash
# 查看吞吐量和延迟
cat results/stress_20260408_144500/run.txt | grep -A 20 "Throughput"

# 查看系统信息
cat results/stress_20260408_144500/sysinfo.txt

# 查看 Redis 内存使用
redis-cli INFO memory
```

## 常见问题与排查

### 1. YCSB 无法连接 Redis

**错误**：
```
Exception in thread "main" java.net.ConnectException: Connection refused
```

**解决**：
```bash
# 检查 Redis 是否运行
ps aux | grep redis-server

# 手动启动
../src/redis-server --port 6379 --daemonize yes

# 测试连接
redis-cli ping
```

### 2. Java 内存不足

**错误**：
```
java.lang.OutOfMemoryError: Java heap space
```

**解决**：
```bash
# 增加 JVM 堆内存
export YCSB_OPTS="-Xmx4g"
./run_ycsb.sh --mode stress
```

### 3. NUMA 不可用

**错误**：
```
NUMA is not available
```

**检查**：
```bash
# 检查 NUMA 支持
numactl --hardware

# 检查 libnuma
ldd ../src/redis-server | grep numa
```

**单节点系统测试**：
NUMA 不可用时，测试仍可运行，只是迁移命令会返回友好错误。

### 4. 测试结果异常

**现象**：吞吐量远低于预期

**排查**：
```bash
# 检查 CPU 使用率
top

# 检查内存
free -h

# 检查 Redis 日志
tail -f /var/log/redis/redis-server.log

# 检查是否有其他进程占用资源
htop
```

### 5. YCSB 编译失败

**错误**：
```
[ERROR] Failed to execute goal on project redis-binding
```

**解决**：
```bash
# 清理后重新编译
cd ycsb-0.17.0
mvn clean package -DskipTests

# 如果网络问题，使用镜像
mvn clean package -DskipTests -Dmaven.repo.local=~/.m2/repository
```

### 6. 迁移统计为零

**现象**：测试完成后 `NUMA MIGRATE STATS` 显示无迁移

**可能原因**：
1. 热度阈值设置过高
2. 测试时间不足
3. 热点不够集中

**解决**：
```bash
# 降低热度阈值
redis-cli NUMA CONFIG SET migrate_hotness_threshold 3

# 使用 Zipfian 分布
./run_numa_migration_test.sh --mode full

# 延长测试时间（修改 workload 中的 operationcount）
```

## 最佳实践

### 1. 测试前准备

```bash
# 清理环境
redis-cli FLUSHALL

# 重启 Redis
redis-cli SHUTDOWN NOSAVE
../src/redis-server redis.conf --daemonize yes

# 等待 warmup
sleep 5
```

### 2. 多次运行取平均值

```bash
for i in {1..5}; do
    ./run_ycsb.sh --mode baseline --output-dir results/run_$i
    sleep 10
done

# 分析所有结果
./scripts/analyze_results.py results/
```

### 3. 对比测试

```bash
# 测试 1：关闭 NUMA
redis-cli CONFIG SET numa-enabled no
./run_ycsb.sh --mode stress --output-dir results/no_numa

# 测试 2：开启 NUMA
redis-cli CONFIG SET numa-enabled yes
./run_ycsb.sh --mode stress --output-dir results/with_numa

# 对比结果
./scripts/analyze_results.py results/
```

### 4. 监控迁移效果

```bash
# 在一个终端启动监控
./numa_migration_monitor.sh --monitor 5

# 在另一个终端运行测试
./run_ycsb.sh --mode stress
```

### 5. 调整参数优化性能

```bash
# 调整候选池大小
redis-cli NUMA CONFIG SET hot_candidates_size 512

# 调整扫描批次
redis-cli NUMA CONFIG SET scan_batch_size 500

# 调整热度阈值
redis-cli NUMA CONFIG SET migrate_hotness_threshold 4
```

## 完整测试流程示例

```bash
#!/bin/bash
# complete_test.sh - 完整测试流程

cd tests/ycsb

echo "========================================="
echo "Complete NUMA Test Suite"
echo "========================================="

# 1. 环境检查
echo "[1/6] Checking environment..."
../src/redis-server --version
java -version
numactl --hardware

# 2. 启动 Redis
echo "[2/6] Starting Redis..."
../src/redis-server redis.conf --daemonize yes
sleep 3

# 3. 基线测试
echo "[3/6] Running baseline test..."
./run_ycsb.sh --mode baseline

# 4. 压力测试
echo "[4/6] Running stress test..."
./run_ycsb.sh --mode stress

# 5. NUMA 迁移测试
echo "[5/6] Running NUMA migration test..."
./run_numa_migration_test.sh --mode full

# 6. 分析结果
echo "[6/6] Analyzing results..."
./scripts/analyze_results.py results/

echo "========================================="
echo "All tests completed!"
echo "========================================="
```

## 命令速查表

| 命令 | 用途 | 耗时 |
|------|------|------|
| `./run_ycsb.sh --mode baseline` | 基线测试 | ~2 分钟 |
| `./run_ycsb.sh --mode stress` | 压力测试 | ~15 分钟 |
| `./run_numa_migration_test.sh --mode quick` | 迁移快速验证 | ~5 分钟 |
| `./run_numa_migration_test.sh --mode full` | 迁移完整测试 | ~15 分钟 |
| `./numa_migration_monitor.sh --monitor 5` | 持续监控 | 持续运行 |
| `./numa_migration_monitor.sh --key <key>` | 查看 Key 元数据 | 即时 |
| `./scripts/analyze_results.py` | 结果分析 | ~30 秒 |
| `redis-cli NUMA MIGRATE STATS` | 迁移统计 | 即时 |
| `redis-cli NUMA CONFIG GET` | 当前配置 | 即时 |
