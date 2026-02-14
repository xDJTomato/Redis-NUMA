# Redis NUMA 内存池诊断功能

## 快速开始

### 1. 准备环境
```bash
# 启动Redis (NUMA内存池版本)
cd /path/to/redis-CXL  
make MALLOC=libc
./src/redis-server ./redis.conf &

# 安装诊断工具 (可选)
sudo apt-get install linux-tools-generic strace numactl
```

### 2. 运行诊断测试
```bash
cd tests

# 快速测试 (推荐首次运行，避免OOM)
./numa_cxl_stress_test.sh quick

# 完整测试 (所有策略)
./numa_cxl_stress_test.sh full
```

### 3. 查看诊断结果
```bash
# 查看诊断摘要
cat tests/reports/diagnosis_*/DIAGNOSIS_SUMMARY.txt

# 查看CPU热点 (识别性能瓶颈)
head -30 tests/reports/diagnosis_*/perf_cpu_report.txt

# 查看内存增长曲线
cat tests/reports/diagnosis_*/memory_timeline.csv
```

## 诊断功能说明

### 已新增功能
1. **Perf性能分析** - 识别CPU热点函数
   - 采样时长: 10秒 (可配置`PERF_DURATION`)
   - 输出: CPU函数调用占比、内存访问模式

2. **实时内存监控** - 跟踪内存使用趋势
   - 采样间隔: 2秒
   - 输出: RSS/VSZ/CPU/Keys时间线CSV

3. **内存池统计** - 分析内存池效率
   - Redis内存详情
   - 进程内存映射
   - NUMA节点分布
   - 碎片率分析

4. **Strace追踪** - 系统调用开销
   - 追踪: mbind/mmap/munmap等
   - 统计: 调用次数和耗时

### 配置参数 (脚本开头)
```bash
# 内存配置 - 已降低到50%避免OOM
TARGET_USAGE_PERCENT=50       # 目标内存使用率
PARALLEL_CLIENTS=20           # 并发客户端
PIPELINE_DEPTH=8              # 管道深度

# 诊断开关
ENABLE_PERF=1                 # 启用perf (0=禁用)
ENABLE_MEMORY_MONITOR=1       # 启用内存监控
PERF_DURATION=10              # perf采样时长
```

## 关键诊断指标

### 1. CPU热点函数
查看 `perf_cpu_report.txt`:
- **numa_alloc_onnode > 5%** → 内存池太小，池外分配多
- **pool_alloc > 10%** → 分配逻辑复杂，需优化
- **malloc/free显著** → 内存池未生效

### 2. 内存碎片率
查看 `fragmentation.txt`:
- **< 1.3** → 良好
- **1.3-1.5** → 可接受  
- **> 1.5** → 严重，需调整chunk大小

### 3. NUMA分布
查看 `numa_distribution.txt`:
- 各节点内存是否按策略分配
- 跨节点访问(Other列)比例

### 4. 系统调用
查看 `strace_summary.txt`:
- mbind调用应该很少 (内存池生效)
- mmap/munmap频率

## 常见问题排查

### 问题1: OOM被杀
**症状**: 测试中途进程消失  
**诊断**:
```bash
dmesg | grep -i "out of memory"
```
**解决**: 降低 `TARGET_USAGE_PERCENT=30`，减小 `batch_size`

### 问题2: 性能低
**症状**: SET/GET < 100K req/s  
**诊断**: 查看perf热点，看是否`numa_alloc_onnode`或`pool_alloc`占比高  
**解决**: 扩大内存池，优化分配逻辑

### 问题3: 碎片严重
**症状**: fragmentation_ratio > 1.5  
**诊断**: 查看 `memory_blocks.txt` 分析块大小分布  
**解决**: 调整chunk大小(当前64KB)或size_class分级

## 内存池设计评估

根据诊断数据评估当前简单内存池设计:

1. **池大小**: numa_alloc_onnode频率 → 是否需要扩大  
2. **chunk大小**: 内存块分布 → 64KB是否合理
3. **size_class**: 对象大小分布 → 8级分类是否够用
4. **碎片管理**: 碎片率 → 是否需要compact机制
5. **NUMA亲和**: 跨节点访问比例 → 迁移开销

## 输出文件说明

诊断目录: `tests/reports/diagnosis_YYYYMMDD_HHMMSS/`

```
DIAGNOSIS_SUMMARY.txt    # ★ 诊断摘要 (先看这个)
perf_cpu_report.txt      # ★ CPU热点分析
memory_timeline.csv      # ★ 内存使用曲线
fragmentation.txt        # ★ 碎片率
numa_distribution.txt    # ★ NUMA分布
perf_cpu.data            # perf原始数据
redis_memory_info.txt    # Redis内存详情
smaps_summary.txt        # 进程内存映射
memory_blocks.txt        # 内存块分布
strace_summary.txt       # 系统调用统计
```

## 性能基准

正常指标:
- SET: 150K-180K req/s
- GET: 160K-200K req/s  
- 碎片率: < 1.3
- numa_alloc_onnode: < 1% CPU
- pool_alloc: < 5% CPU

偏离这些指标说明内存池设计有问题。

