# YCSB 压力测试框架

基于 YCSB (Yahoo! Cloud Serving Benchmark) 的 Redis NUMA 压力测试框架。

## 目录结构

```
tests/ycsb/
├── run_ycsb.sh          # 统一入口脚本（baseline / stress 双模式）
├── workloads/
│   ├── workload_baseline  # 基线测试：10万条记录，4线程，读写各半
│   └── workload_stress    # 压力测试：100万条记录，32线程，热点写密集
├── scripts/
│   ├── install_ycsb.sh    # YCSB 安装脚本
│   └── analyze_results.py # 结果分析工具
├── results/               # 测试结果（按 mode_timestamp/ 分目录存放）
├── legacy/                # 旧版脚本归档
│   ├── scripts/           # 历史测试脚本
│   ├── results/           # 历史测试结果
│   └── workloads/         # 废弃工作负载
└── ycsb-0.17.0/           # YCSB 工具包
```

## 快速开始

### 安装 YCSB（首次使用）

```bash
cd tests/ycsb
./scripts/install_ycsb.sh
```

依赖：Java 11+、Maven

### 基线测试（验证功能正确性）

```bash
./run_ycsb.sh --mode baseline
```

- 数据量：10 万条，Value 1KB
- 线程数：4
- 访问模式：均匀分布，50% 读 / 50% 更新
- 耗时：约 2 分钟，内存 ~200MB
- 用途：日常开发回归、功能验证

### 压力测试（评估极限性能）

```bash
./run_ycsb.sh --mode stress
```

- 数据量：100 万条，Value 8KB（总计约 8GB）
- 线程数：32
- 访问模式：Hotspot 80/20，30% 读 / 60% 更新 / 10% 插入
- 耗时：约 10-20 分钟，内存 8GB+
- 用途：发布前测试、NUMA 迁移策略验证、性能基准

## 完整选项

```
./run_ycsb.sh --mode <baseline|stress> [选项]

  --port PORT        Redis 端口      (默认: 6379)
  --host HOST        Redis 地址      (默认: 127.0.0.1)
  --maxmem MEM       Redis 最大内存  (默认: 8gb)
  --output-dir DIR   结果输出目录    (默认: results/)
  --no-restart       跳过 Redis 重启，使用已有实例
```

示例：

```bash
# 压力测试，16GB 内存
./run_ycsb.sh --mode stress --maxmem 16gb

# 基线测试，使用已有 Redis 实例
./run_ycsb.sh --mode baseline --no-restart

# 指定自定义端口
./run_ycsb.sh --mode baseline --port 6380
```

## 测试流程

```
1. 前置检查：YCSB、redis-server、workload 文件均存在
2. 重启 Redis：pkill 旧进程 → 新实例（禁用持久化，限制内存）
3. Load 阶段：FLUSHALL → ycsb load（写入全量数据）
4. Run 阶段： ycsb run（混合读写压测）
5. 结果摘要：输出 Throughput 和 AverageLatency
6. 保存结果：results/<mode>_<timestamp>/load.txt + run.txt + sysinfo.txt
```

## 输出文件

每次测试在 `results/<mode>_<timestamp>/` 下生成：

| 文件 | 内容 |
|------|------|
| `load.txt` | YCSB Load 阶段详细日志 |
| `run.txt` | YCSB Run 阶段详细日志（含吞吐量、延迟分布） |
| `sysinfo.txt` | 测试时的 CPU / 内存 / NUMA 拓扑信息 |

## 注意事项

1. **Redis 重启**：脚本默认重启 Redis，确保内存状态干净，避免碎片率测量失真
2. **内存要求**：stress 模式至少需要 8GB 可用内存
3. **NUMA 支持**：`numactl --hardware` 确认节点数量
4. **旧脚本**：历史版本脚本位于 `legacy/scripts/`，仅供参考

## 相关文档

- [YCSB 负载设计详解](../../docs/devlog/ycsb_migration_workload_design.md)
- [Legacy 测试归档](legacy/README.md)
- [NUMA 策略文档](../../docs/modules/07-numa-composite-lru.md)
- [YCSB 官方文档](https://github.com/brianfrankcooper/YCSB)
