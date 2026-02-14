# YCSB NUMA 自动化压力测试框架

基于 YCSB (Yahoo! Cloud Serving Benchmark) 的 Redis NUMA 扩展压力测试框架。

## 目录结构

```
tests/ycsb/
├── README.md                   # 本文档
├── run_ycsb_test.sh            # 主测试脚本
├── ycsb-run.sh                 # YCSB 包装脚本 (安装后生成)
├── workloads/                  # 工作负载配置
│   ├── workload_numa_hotspot       # 热点访问模式 (80/20)
│   ├── workload_numa_mixed         # 混合工作负载
│   └── workload_numa_write_heavy   # 写密集型
├── scripts/                    # 辅助脚本
│   ├── install_ycsb.sh         # YCSB 安装脚本
│   └── analyze_results.py      # 结果分析工具
└── results/                    # 测试结果目录
```

## 快速开始

### 1. 安装 YCSB

```bash
cd tests/ycsb
./scripts/install_ycsb.sh
```

依赖:
- Java 11+
- Maven
- Python 3

### 2. 运行测试

```bash
# 运行完整测试套件
./run_ycsb_test.sh

# 仅运行热点测试
./run_ycsb_test.sh -w workload_numa_hotspot

# 自定义参数
./run_ycsb_test.sh -r 100000 -t 50

# 仅分析已有结果
./run_ycsb_test.sh -p
```

### 3. 分析结果

```bash
python3 scripts/analyze_results.py results/
```

## 工作负载说明

### workload_numa_hotspot

模拟 NUMA 场景下的热点访问模式:
- 80% 的操作集中在 20% 的 key 上
- 读多写少 (80% 读, 20% 写)
- 测试 NUMA 热点迁移效果

### workload_numa_mixed

模拟真实的混合工作负载:
- Zipfian 分布
- 70% 读, 20% 更新, 5% 扫描, 5% 插入
- 测试综合性能

### workload_numa_write_heavy

写密集型测试:
- 均匀分布
- 20% 读, 60% 更新, 20% 插入
- 测试内存分配和 NUMA 写入性能

## 测试流程

```
1. 启动 Redis (自动)
   - 配置 8GB 内存
   - 禁用持久化
   - 启用 NUMA 优化

2. 数据加载 (Load Phase)
   - 根据配置插入指定数量记录
   - 收集加载后内存使用

3. 压力测试 (Run Phase)
   - 执行指定数量的操作
   - 多线程并发访问
   - 实时输出吞吐量

4. 结果收集
   - YCSB 性能指标
   - Redis 统计信息
   - 系统资源使用

5. 生成报告
   - 吞吐量对比
   - 延迟分布
   - JSON 格式导出
```

## 输出文件

测试结果保存在 `results/` 目录:

- `{test_name}_{timestamp}_load.log` - 加载阶段日志
- `{test_name}_{timestamp}_run.log` - 运行阶段日志
- `{test_name}_{timestamp}_redis_stats.txt` - Redis 统计
- `system_info_{timestamp}.txt` - 系统信息
- `summary_report_{timestamp}.txt` - 汇总报告
- `results_{timestamp}.csv` - CSV 格式结果
- `analysis_{timestamp}.json` - JSON 分析报告

## 性能指标

### YCSB 指标

- **Throughput**: 吞吐量 (ops/sec)
- **AverageLatency**: 平均延迟 (us)
- **95thPercentileLatency**: P95 延迟
- **99thPercentileLatency**: P99 延迟

### Redis 指标

- **used_memory**: 内存使用
- **instantaneous_ops_per_sec**: 实时 OPS
- **keyspace_hits/misses**: 缓存命中率
- **total_commands_processed**: 总命令数

## NUMA 特定测试

### 热点迁移测试

```bash
# 运行热点工作负载
./run_ycsb_test.sh -w workload_numa_hotspot -r 100000

# 观察 NUMAMIGRATE 统计
./src/redis-cli NUMAMIGRATE STATS
```

### 多节点对比

```bash
# 配置不同 NUMA 策略后分别测试
./src/redis-cli NUMACONFIG SET strategy interleaved
./run_ycsb_test.sh -w workload_numa_mixed

./src/redis-cli NUMACONFIG SET strategy local
./run_ycsb_test.sh -w workload_numa_mixed
```

## 注意事项

1. **内存要求**: 测试需要至少 8GB 可用内存
2. **NUMA 支持**: 确保系统支持 NUMA (`numactl --hardware`)
3. **Redis 配置**: 测试会自动启动 Redis，无需手动启动
4. **清理**: 测试完成后会自动清理 Redis 进程

## 故障排查

### YCSB 未安装

```bash
# 手动安装
wget https://github.com/brianfrankcooper/YCSB/releases/download/0.17.0/ycsb-0.17.0.tar.gz
tar -xzf ycsb-0.17.0.tar.gz
```

### Redis 连接失败

```bash
# 检查 Redis 状态
./src/redis-cli -p 6379 ping

# 手动启动 Redis
./src/redis-server --port 6379 --daemonize yes
```

### 权限问题

```bash
# 确保脚本可执行
chmod +x run_ycsb_test.sh scripts/*.sh
```

## 扩展开发

### 添加新工作负载

1. 在 `workloads/` 创建配置文件
2. 参考现有工作负载格式
3. 在 `run_ycsb_test.sh` 的 `TEST_WORKLOADS` 中添加

### 自定义分析

修改 `scripts/analyze_results.py`:
- 添加新的解析逻辑
- 生成自定义图表
- 导出其他格式

## 相关文档

- [YCSB 官方文档](https://github.com/brianfrankcooper/YCSB)
- [Redis NUMA 开发日志](../../NUMA_DEVELOPMENT_LOG.md)
- [NUMA 策略文档](../../docs/modules/07-numa-composite-lru.md)
