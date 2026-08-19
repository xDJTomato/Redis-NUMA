# YCSB 压力测试框架

基于 YCSB (Yahoo! Cloud Serving Benchmark) 的 Redis NUMA 压力测试框架。

## 目录结构

```
tests/ycsb/
├── run_ycsb.sh               # 统一入口脚本（baseline / stress 双模式）
├── run_bw_benchmark.sh       # NUMA 带宽饱和基准测试（三阶段）
├── fetch_results.sh          # 从远程服务器下载测试结果
├── workloads/
│   ├── workload_baseline     # 基线测试：10万条记录，4线程，读写各半
│   ├── workload_stress       # 压力测试：100万条记录，32线程，热点写密集
│   └── workload_bw_saturate  # 带宽饱和测试：百万条大 Value，64线程
├── scripts/
│   ├── install_ycsb.sh           # YCSB 安装脚本
│   ├── analyze_results.py        # 结果分析工具
│   └── visualize_bw_benchmark.py # 带宽基准测试可视化报告生成
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

---

## run_algorithm_comparison.sh — 迁移策略全量对比

### 用途

依次跑通 5 组单一变量对比（Vanilla Redis 7.2.6 / Redis-NUMA 禁用迁移(noop) /
Redis-NUMA Composite LRU / Redis-NUMA TinyLFU / Redis-NUMA CAAT（NUMAflow 默认
策略，见 `numaflow/` 及 `docs/new/09-architecture-decisions.md` 的 ADR-08）），
每组都复用 `run_bw_benchmark.sh` / `run_bw_benchmark_vanilla.sh` 的三阶段负载
(Fill→Hotspot→Sustain)，最后用 `scripts/visualize_bw_benchmark.py`（旧的画图
方案）一次性画出 5 组吞吐量/本地访问率曲线图和阶段延迟对比图。ADR-08 之后三个
迁移策略统一收敛到 NUMAflow 的原子操作框架，`composite_lru`/`tinylfu`/`caat`
三组都是 `run_bw_benchmark.sh` 内部调用 `NUMA FLOW DEFAULT <name>` 直接切换
（不再需要导出/加载 workflow JSON 文件），三者走的是同一套执行引擎，只是加载
的预设不同。

### 快速开始

```bash
cd tests/ycsb
./run_algorithm_comparison.sh                       # 跑全部 5 组，maxmem 统一为 8gb
./run_algorithm_comparison.sh --only noop,tinylfu,caat   # 只跑指定的组
```

### 输出

结果写到仓库最外层的 `Results/algorithms_<timestamp>/`（不是本目录下的
`results/`），每组一个子目录（`vanilla/` `noop/` `composite_lru/` `tinylfu/`
`caat/`，内容同 `run_bw_benchmark.sh` 的输出），外加：

| 文件 | 内容 |
|------|------|
| `comparison_report.png` | 5 组吞吐量曲线 + 本地 NUMA 访问率（阶段对齐） |
| `comparison_report_latency.png` | 5 组按阶段的平均延迟柱状图 |
| `summary.txt` | 5 组每阶段吞吐量/延迟文本摘要 |

### 依赖

同 `run_bw_benchmark_vanilla.sh`：需要预先生成同版本的原版 Redis 7.2.6 对比基线
（`git worktree add ../redis-7.2.6-vanilla 7.2.6 && cd ../redis-7.2.6-vanilla/src
&& make -j$(nproc)`）。`composite_lru`/`tinylfu`/`caat` 三组都只依赖已编译好的
`redis-server`/`redis-cli`（NUMA FLOW 命令内置在内核里），不需要单独构建
`numaflow/` 的 CLI 二进制。


---

## run_bw_benchmark.sh — NUMA 带宽饱和基准测试

### 用途

用于在双路 NUMA 服务器上执行三阶段带宽饱和测试，目标是吃满 NUMA 节点带宽、触发
热点迁移和降级策略，并采集实时指标生成可视化报告。

### 依赖

| 依赖 | 说明 |
|------|------|
| `java` 11+ | 运行 YCSB |
| `numactl` | CPU 绑定到 Node 0（可选，缺失时跳过绑定） |
| `python3` + `venv` | 生成可视化报告（可选，首次运行自动创建虚拟环境） |
| `bc` | 内存用量计算（可选，缺失时回退到整数运算） |

### 三阶段说明

```
Phase 1 (Fill)     写入 100 万条 × 1800B 记录（~1.7 GB）
                   8 线程，吃满内存，建立初始数据集

Phase 2 (Hotspot)  2000 万次读写，Zipfian α=0.99（极端热点分布）
                   64 线程，触发 NUMA 热点迁移策略

Phase 3 (Sustain)  2000 万次写密集操作（70% UPDATE / 30% READ）
                   64 线程，持续高压，评估策略的持久承压能力
```

### 快速开始

```bash
cd tests/ycsb

# 启动全部三个阶段（脚本自动启动/停止 Redis）
./run_bw_benchmark.sh

# 使用已运行的 Redis 实例（跳过重启）
./run_bw_benchmark.sh --no-restart

# 仅运行 Phase 2 和 Phase 3（数据已填充时）
./run_bw_benchmark.sh --skip-fill

# 仅运行指定阶段
./run_bw_benchmark.sh --phase 2
```

### 完整选项

```
./run_bw_benchmark.sh [选项]

  --port PORT          Redis 端口       (默认: 6379)
  --maxmem MEM         Redis 最大内存   (默认: 11gb)
  --output-dir DIR     结果输出目录     (默认: results/bw_bench_<timestamp>/)
  --phase 1|2|3|all    运行指定阶段     (默认: all)
  --skip-fill          跳过 Phase 1 填充阶段
  --no-restart         不重启 Redis，使用已有实例
  --help               显示帮助
```

### 输出文件

每次测试在 `results/bw_bench_<timestamp>/` 下生成：

| 文件 | 内容 |
|------|------|
| `phase1_load.txt` | Phase 1 YCSB 详细日志（吞吐量、延迟分布） |
| `phase2_hotspot.txt` | Phase 2 YCSB 详细日志 |
| `phase3_sustain.txt` | Phase 3 YCSB 详细日志 |
| `metrics.csv` | 每秒采集的 Redis 内存、ops、NUMA 迁移数等时序指标 |
| `system_info.txt` | 测试时的 CPU / 内存 / NUMA 拓扑 / 测试参数快照 |
| `redis.log` | Redis 运行日志（verbose 级别） |
| `benchmark_report.png` | 可视化报告（需 python3，含吞吐量曲线、延迟热图、迁移趋势） |

`metrics.csv` 列定义：

| 列名 | 说明 |
|------|------|
| `timestamp` | Unix 时间戳（秒） |
| `phase` | 当前阶段标记（`1_fill` / `2_hotspot` / `3_sustain`） |
| `ops_total` | Redis 累计处理命令数 |
| `ops_sec` | 每秒增量命令数 |
| `used_mem_mb` | Redis 已用内存（MB） |
| `rss_mb` | Redis RSS 内存（MB） |
| `frag_ratio` | 内存碎片率 |
| `migrate_total` | NUMA 累计成功迁移次数 |
| `migrate_sec` | 每秒迁移次数 |
| `numa_pages_n0/n1` | Node 0 / Node 1 每秒 NUMA hit 页面增量 |
| `evicted_keys` | Redis 累计驱逐 key 数 |
| `accesses_local/remote` | 每秒本地 / 跨节点访问增量 |

### 注意事项

1. **root 权限**：脚本会尝试关闭 NUMA Balancing 和 Transparent Huge Pages
   以减少内核干扰；非 root 时跳过，建议手动执行：
   ```bash
   sudo bash -c 'echo 0 > /proc/sys/kernel/numa_balancing'
   sudo bash -c 'echo never > /sys/kernel/mm/transparent_hugepage/enabled'
   ```
2. **内存要求**：Phase 1 填充约 1.7 GB，默认 `maxmem=11gb`，实际需保留足够 RSS 空间。
3. **YCSB 入口**：优先使用 `ycsb.sh`（Java wrapper），避免 Python 2/3 兼容问题。
4. **路径空格**：脚本通过 `/tmp/redis-cxl-bench-<PID>` 符号链接绕过 YCSB 不支持
   路径中含空格的限制（适用于 `Redis-NUMA main` 等含空格的项目路径）。
5. **NUMA 命令**：`NUMA MIGRATE STATS` 等命令仅在编译了 NUMA 模块的 redis-server
   上可用；普通 Redis 实例会输出 `ERR unknown command`，不影响测试正常运行。

---

## fetch_results.sh — 远程结果下载工具

### 用途

将远程服务器上的 YCSB 基准测试结果同步到本地，支持下载最新一次或指定目录。

### 快速开始

```bash
cd tests/ycsb

# 下载远程最新一次测试结果到本地 results/
./fetch_results.sh

# 下载全部远程结果
./fetch_results.sh --all

# 下载指定目录
./fetch_results.sh --dir bw_bench_20260514_113507

# 指定远程服务器和本地输出目录
./fetch_results.sh --host 192.168.12.204 --user dell \
                   --remote-dir ~/lx/Redis-NUMA-main/tests/ycsb/results \
                   --local-dir ./results/remote
```

### 完整选项

```
./fetch_results.sh [选项]

  --host HOST          远程主机地址   (默认: 192.168.12.204)
  --user USER          SSH 用户名     (默认: dell)
  --port PORT          SSH 端口       (默认: 22)
  --remote-dir DIR     远程结果根目录 (默认: ~/lx/Redis-NUMA-main/tests/ycsb/results)
  --local-dir DIR      本地保存目录   (默认: results/remote/)
  --dir NAME           下载指定结果目录名（优先级高于 --all）
  --all                下载全部远程结果（默认只下载最新一次）
  --help               显示帮助
```
