# Redis Phase 1 内存占用 - 快速修复指南

## 问题回顾

Phase 1 Fill 阶段内存占用受限于 `maxmemory=3500mb`，导致：
- 实际存储: 133 万条（而非预期 100 万条）
- 被驱逐: 67 万条
- 内存占用: 3.5GB（maxmemory 上限）

**这不是 Bug，是配置与需求不匹配。**

---

## 快速验证 (5分钟)

### 验证 1: 确认问题存在

```bash
cd /home/xdjtomato/下载/Redis\ with\ CXL/redis-CXL\ in\ v6.2.21

# 启动测试
./tests/ycsb/run_bw_benchmark.sh --output-dir verify_test

# Phase 1 运行时，在另一个终端执行:
watch -n 1 'redis-cli -p 6379 INFO stats | grep -E "total_commands|evicted_keys"'

# 观察: evicted_keys 持续增加到 60+ 万 → 问题存在
```

### 验证 2: 应用修复 (方案 A)

```bash
# 用 11GB maxmemory 重新测试
./tests/ycsb/run_bw_benchmark.sh --maxmem 11gb --output-dir verify_fix

# 查看结果
tail -1 verify_fix/metrics.csv | awk -F, '{print "ops:", $3, "mem:", $5, "evicted:", $12}'

# 预期: ops: 1000000, mem: ~10500, evicted: 0
```

---

## 三种修复方案

### 方案 A: 命令行调整 (立即可用，推荐测试)

```bash
./run_bw_benchmark.sh --maxmem 11gb
```

**优点**: 无需修改代码，立即可测试完整工作负载

**缺点**: 需要 11GB 可用内存，每次测试需手动指定

---

### 方案 B: 修改脚本默认值 (长期方案，推荐平衡)

**修改文件**: `tests/ycsb/run_bw_benchmark.sh`

**修改内容**:
```bash
# 第 43 行: 增加 maxmemory
- MAX_MEMORY="3500mb"
+ MAX_MEMORY="11gb"

# 或者，改为平衡配置 (推荐)
+ # 配置与数据规模对齐: 500K records × 7KB = 3.5GB
+ PHASE1_RECORDS=500000
+ PHASE1_FIELD_LENGTH=7000
+ MAX_MEMORY="3500mb"  # 保持原值，完美匹配
```

**验证**:
```bash
# 修改后重新运行
./tests/ycsb/run_bw_benchmark.sh

# 检查 Phase 1 结束时的指标
grep "1_fill" results/bw_bench_*/metrics.csv | tail -1
# 应显示: evicted_keys ≈ 0
```

---

### 方案 C: 关闭内存限制 (激进，仅用于压力测试)

**修改文件**: `tests/ycsb/run_bw_benchmark.sh` 第 265-266 行

```bash
# 注释掉这两行:
# --maxmemory "$MAX_MEMORY" \
# --maxmemory-policy allkeys-lru \
```

**优点**: 最大化内存使用，充分测试 NUMA 性能

**缺点**: 失去内存控制，可能导致 OOM

---

## 推荐方案

### 对于开发/调试

**选择方案 B (平衡配置)**:
```bash
PHASE1_RECORDS=500000
PHASE1_FIELD_LENGTH=7000
MAX_MEMORY="3500mb"
```

优点:
- 配置与内存完美对齐
- 压力测试充分 (3.5GB 完全占用)
- Phase 1 无任何浪费或 eviction
- 便于重现和验证

### 对于性能测试

**选择方案 A 或修改后的方案 B**:
```bash
./run_bw_benchmark.sh --maxmem 11gb
```

优点:
- 完整测试 1M × 10KB 工作负载
- NUMA 内存池真实性能
- Phase 2/3 有充足热数据

---

## 实施步骤

### 步骤 1: 选择方案

- [ ] 方案 A (推荐快速验证)
- [ ] 方案 B (推荐长期使用)
- [ ] 方案 C (仅用于压力测试)

### 步骤 2: 应用修改

```bash
cd /home/xdjtomato/下载/Redis\ with\ CXL/redis-CXL\ in\ v6.2.21

# 如选择方案 B，编辑脚本
vi tests/ycsb/run_bw_benchmark.sh

# 修改第 43 行和/或 50-51 行
# (根据选择的具体方案)
```

### 步骤 3: 验证修复

```bash
# 运行基准测试
./tests/ycsb/run_bw_benchmark.sh --output-dir fixed_test

# 检查结果
metrics_file="fixed_test/metrics.csv"
tail -1 "$metrics_file" | awk -F, '{
    print "操作数:", $3
    print "内存(MB):", $5
    print "驱逐(条):", $12
}'

# 验收标准:
# 驱逐 ≈ 0 (之前是 67+ 万)
# 内存 ≈ 3500 (如方案B) 或 10500 (如方案A)
```

### 步骤 4: 更新文档

```bash
# 更新 README 或注释说明
# • maxmemory 的用途和默认值
# • 各 Phase 的预期内存占用
# • 调整指南和推荐配置
```

---

## 关键文件

| 文件 | 作用 | 关键行 |
|------|------|--------|
| `tests/ycsb/run_bw_benchmark.sh` | 主脚本 | L43, L50-51, L265-266 |
| `tests/ycsb/workloads/workload_bw_saturate` | 工作负载 | recordcount, fieldlength |
| `tests/ycsb/results/*/metrics.csv` | 测试结果 | evicted_keys, used_mem_mb |

---

## 回退方案

如果修改后出现问题，可以快速回退：

```bash
# 查看 git 状态
git diff tests/ycsb/run_bw_benchmark.sh

# 回退到原始版本
git checkout tests/ycsb/run_bw_benchmark.sh
```

---

## FAQ

**Q: 为什么不直接改成 11GB?**
A: 方案 B (平衡配置) 更优，因为：
   - 内存占用清晰可控
   - 测试结果可重现
   - 不依赖系统配置

**Q: 会影响 Phase 2 和 Phase 3 吗?**
A: 是的，Phase 1 的修改会影响后续阶段的数据量。
   - 方案 A (11GB): Phase 2/3 会有更多热数据
   - 方案 B (平衡): Phase 2/3 数据量相应减少

**Q: 需要重新编译 Redis 吗?**
A: 否。这是配置修改，无需重新编译。

**Q: 旧的测试结果还有效吗?**
A: 否。修改后的测试与之前的不可比。建议清除旧结果后重新测试。

---

## 下一步

1. 运行方案 A 验证修复有效性 (5 分钟)
2. 选择长期方案 (方案 B 推荐)
3. 应用修改并验证 (10 分钟)
4. 更新项目文档
5. 清除旧测试结果，重新基准测试

---

**总耗时**: ~30 分钟完成所有步骤

**关键成果**: 
- 问题诊断完成
- 修复方案清晰
- 可立即实施
- 无需代码修改

