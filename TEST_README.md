# Redis NUMA/CXL 测试指南

## 测试文件组织

项目测试文件按用途分层组织：

### tests/ycsb/ — 活跃测试脚本（推荐使用）

主力测试目录，包含完整的性能评估工具链：

| 脚本 | 用途 |
|------|------|
| `run_bw_benchmark.sh` | 三阶段 NUMA 带宽饱和基准测试（Fill→Hotspot→Sustain），主力测试脚本 |
| `run_ycsb.sh` | YCSB 统一入口（baseline/stress 双模式） |
| `run_numa_migration_test.sh` | NUMA 迁移触发专项测试 |
| `setup_vm_env.sh` | CXL 虚拟机一键环境安装（Python+Java+依赖） |
| `numa_migration_monitor.sh` | NUMA 迁移诊断与实时监控工具 |

### tests/ycsb/scripts/ — 辅助工具

独立工具脚本，配合主测试流程使用：

| 脚本 | 用途 |
|------|------|
| `eval_cxl_memory.sh` | CXL 内存性能评估（Node0 DRAM vs Node1 CXL 对比测试） |
| `generate_report.sh` | 独立可视化报告生成（在宿主机上读取 VM 测试结果） |
| `install_ycsb.sh` | YCSB 0.17.0 安装脚本 |

### tests/legacy/numa/ — 归档 NUMA 功能测试

历史功能验证脚本，已归档供参考：

| 脚本 | 用途 |
|------|------|
| `test_composite_lru.sh` | Composite LRU 通用功能测试（归档，仍可参考） |
| `test_composite_lru_cxl.sh` | CXL 专用 LRU 测试（归档，仍可参考） |
| `test_numa_command.sh` 等 | 其他 NUMA 命令/功能测试 |

### 项目根目录

快速诊断工具：

| 脚本 | 用途 |
|------|------|
| `check_numa_config.sh` | NUMA 配置快速检查 |
| `diagnose_numa.sh` | NUMA 环境诊断 |

---

## 推荐测试流程

### 快速开始（新环境首次运行）

```bash
# 1. 检查 NUMA 环境
./check_numa_config.sh

# 2. 进入测试目录
cd tests/ycsb

# 3. 运行完整三阶段基准测试
./run_bw_benchmark.sh
```

### CXL 虚拟机测试流程

```bash
# 1. 登录 CXL 虚拟机
ssh user@cxl-vm-ip

# 2. 一键安装依赖环境（Python + Java + YCSB）
cd tests/ycsb
./setup_vm_env.sh

# 3. 部署并编译 Redis
cd ~/redis-CXL-in-v6.2.21
make clean && make -j4

# 4. 运行测试
cd tests/ycsb
./run_bw_benchmark.sh --maxmem 11gb

# 5. 在宿主机上生成可视化报告
./scripts/generate_report.sh --vm-results /path/to/vm/results
```

### CXL 内存性能评估

对比 Node0 DRAM 与 Node1 CXL 内存性能：

```bash
cd tests/ycsb/scripts
./eval_cxl_memory.sh --port 6379 --duration 120
```

输出包括：
- Node0 DRAM 访问延迟统计
- Node1 CXL 访问延迟统计
- 带宽对比分析
- 延迟分布直方图

---

## 核心脚本详细说明

### run_bw_benchmark.sh — NUMA 带宽饱和基准测试

三阶段压力测试，验证 NUMA 带宽感知降级评分和迁移策略在高压场景下的行为。

#### 快速开始

```bash
cd tests/ycsb
./run_bw_benchmark.sh --help          # 查看所有选项
./run_bw_benchmark.sh                 # 运行完整三阶段测试
./run_bw_benchmark.sh --phase 2       # 仅运行热点迁移阶段
```

#### 三阶段说明

| 阶段 | 描述 | 数据量 | 线程数 | 预期时间 |
|------|------|--------|--------|----------|
| Phase 1: Fill | YCSB load 100万条x10KB，吃满 Node0 溢出到 Node1 | ~10GB | 自动 | 3-5 分钟 |
| Phase 2: Hotspot | Zipfian α=0.99 极端热点访问，触发升降级迁移 | 200万操作 | 16 | 5-10 分钟 |
| Phase 3: Sustain | 写密集 60% 更新 + 高并发，持续带宽饱和 | 300万操作 | 24 | 5-10 分钟 |

#### 常用选项

```bash
--port PORT          # Redis 端口 (默认 6379)
--maxmem MEM         # 最大内存 (默认 11gb，触发溢出到 Node1)
--output-dir DIR     # 结果输出目录
--phase 1|2|3|all    # 选择运行阶段 (默认 all)
--skip-fill          # 跳过填充阶段（已有数据时）
--no-restart         # 使用已运行的 Redis 实例
```

#### 输出文件

测试完成后在 `results/bw_bench_<timestamp>/` 目录下生成：

| 文件 | 说明 |
|------|------|
| `metrics.csv` | 每秒采集的性能指标（吞吐量、内存、迁移、NUMA 页面访问等） |
| `benchmark_report.png` | matplotlib 生成的 6 面板可视化图表 |
| `phase1_load.txt` | Phase 1 YCSB 输出 |
| `phase2_hotspot.txt` | Phase 2 YCSB 输出 |
| `phase3_sustain.txt` | Phase 3 YCSB 输出 |
| `redis.log` | Redis 服务器日志 |
| `sysinfo.txt` | 系统 NUMA 拓扑信息 |

#### 前置依赖

- YCSB 0.17.0 (已安装在 `tests/ycsb/ycsb-0.17.0/`)
- Python 3 + matplotlib (`pip3 install matplotlib`)
- NUMA 支持 (`numactl --hardware` 可用)

#### 可视化图表

运行后自动生成 `benchmark_report.png`，包含 6 个子图：
1. **吞吐量** (ops/sec) — 各阶段性能变化
2. **内存使用** (MB) — used_memory + RSS，标注 Node0 容量线
3. **迁移速率** (migrations/sec) — NUMA 降级/升级触发频率
4. **NUMA 页面访问** — Node0 vs Node1 访问速率对比
5. **内存碎片率** — 碎片率变化趋势
6. **淘汰计数** — 累计淘汰 key 数量

也可手动重新生成图表：
```bash
python3 scripts/visualize_bw_benchmark.py \
    --input results/bw_bench_xxx/metrics.csv \
    --output results/bw_bench_xxx/benchmark_report.png
```

---

### eval_cxl_memory.sh — CXL 内存性能评估

对比评估 Node0 本地 DRAM 与 Node1 CXL 内存的性能差异。

#### 基本用法

```bash
cd tests/ycsb/scripts
./eval_cxl_memory.sh                    # 默认测试
./eval_cxl_memory.sh --duration 300     # 指定测试时长
./eval_cxl_memory.sh --pattern random  # 访问模式：random/sequential
```

#### 输出内容

- 延迟统计：P50/P90/P99 延迟对比
- 吞吐量：Node0 vs Node1 带宽对比
- 延迟分布直方图

---

### generate_report.sh — 独立报告生成

在宿主机上读取虚拟机测试结果，生成交互式可视化报告。

#### 基本用法

```bash
cd tests/ycsb/scripts
./generate_report.sh --vm-results /path/to/vm/results --output ./report
```

---

## 归档脚本说明

`tests/legacy/numa/` 目录包含历史 NUMA 功能测试脚本，已归档但可作为参考：

- **test_composite_lru.sh** — Composite LRU 策略功能验证，测试 STRING/HASH/LIST/SET/ZSET 操作及热度追踪
- **test_composite_lru_cxl.sh** — CXL 环境专用测试，包含 CXL 设备检测和分层访问模式模拟
- **test_numa_command.sh** — NUMA 命令功能测试

这些脚本不再作为主要测试入口，但功能逻辑仍可参考。

---

## 故障排查

### Redis 无法启动

```bash
# 检查端口占用
netstat -tlnp | grep 6379

# 使用其他端口
./src/redis-server --port 6380 --daemonize yes
./run_bw_benchmark.sh --port 6380
```

### 策略未加载

```bash
# 检查 Redis 日志
tail -100 /tmp/redis-test.log | grep -i "strategy\|lru"

# 确认编译包含策略模块
grep "numa_composite_lru" src/Makefile
```

### CXL 设备未检测到

```bash
# 检查内核配置
zcat /proc/config.gz | grep CXL

# 手动加载 CXL 模块
sudo modprobe cxl

# 检查 dmesg
dmesg | grep -i cxl
```

### NUMA 带宽测试异常

```bash
# 确认 NUMA 支持
numactl --hardware

# 检查 Redis 内存监控
redis-cli INFO memory | grep used_memory_rss
```

---

## 联系与支持

如有问题，请检查：
1. Redis 日志文件（`/tmp/redis-*.log`）
2. 测试脚本日志（`tests/ycsb/results/`）
3. 系统 dmesg 日志（`dmesg | grep -i cxl`）
