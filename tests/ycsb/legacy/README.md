# Legacy 测试文件归档

本目录存放历史测试脚本和结果，用于追溯和参考。

## 目录结构

```
legacy/
├── scripts/          # 旧版测试脚本
├── results/          # 历史测试结果
└── workloads/        # 废弃的工作负载配置
```

## 脚本分类

### 基础测试脚本

**run_ycsb_test.sh**
- 描述：早期 YCSB 测试框架
- 创建时间：2026-02-14
- 功能：支持多种工作负载（hotspot, write_heavy, mixed）
- 状态：已被 `run_migration_high_pressure_v2.sh` 替代
- 特点：
  - 支持 3 种预定义工作负载
  - 自动分析测试结果
  - 生成 JSON 格式报告

**run_migration_test.sh**
- 描述：迁移策略基础测试
- 创建时间：2026-02-22
- 功能：验证 Composite LRU 迁移策略基本功能
- 状态：作为轻量级测试保留
- 特点：
  - 小规模测试（100K 记录）
  - 快速验证（约 2 分钟）
  - 适合日常开发测试

**run_migration_high_pressure.sh**
- 描述：第一版高强度测试
- 创建时间：2026-02-22
- 功能：使用 redis-cli pipe 批量插入数据
- 状态：已被优化版替代
- 问题：
  - 数据加载速度慢
  - 使用 pipe 模式不够高效
- 替代方案：`run_migration_high_pressure_v2.sh`（使用 Lua 脚本）

## 测试结果分类

### 早期 YCSB 测试（2026-02-14）

**测试环境**：
- 记录数：50,000
- 操作数：500,000
- 线程数：50

**结果文件**：

1. **analysis_20260214_205145.json**
   - 第一次 YCSB 测试
   - Hotspot 工作负载
   - 吞吐量：98K ops/sec

2. **analysis_20260214_212834.json**
   - 高负载测试
   - 三种工作负载对比
   - 结果：
     - Hotspot: 131K ops/sec
     - Write Heavy: 100K ops/sec
     - Mixed: 57K ops/sec

3. **test_summary.txt**
   - 测试总结报告
   - 包含性能对比和缓存命中率

### 工作负载日志

- **ycsb_hotspot_load.log**: Hotspot 工作负载 Load 阶段
- **ycsb_hotspot_run.log**: Hotspot 工作负载 Run 阶段（初始版本）
- **ycsb_hotspot_high_run.log**: Hotspot 工作负载 Run 阶段（高负载版本）
- **ycsb_mixed_run.log**: Mixed 工作负载
- **ycsb_mixed_high_run.log**: Mixed 工作负载（高负载）
- **ycsb_write_heavy_run.log**: Write Heavy 工作负载
- **ycsb_test_run.log**: 初始测试运行日志

### 其他日志

- **redis_server.log**: Redis 服务器早期日志

## 当前推荐方案

### 日常开发测试
```bash
cd tests/ycsb
./run_migration_test.sh
```
- 快速验证（2 分钟）
- 小规模数据集
- 检查策略是否正常工作

### 压力测试
```bash
cd tests/ycsb
./run_migration_high_pressure_v2.sh
```
- 完整功能验证（5-10 分钟）
- 10+ GB 内存压力
- 覆盖三种内存分配方案
- 性能基准测试

## 历史改进记录

### 2026-02-14
- ✅ 建立 YCSB 测试框架
- ✅ 实现三种工作负载（hotspot, write_heavy, mixed）
- ✅ 添加自动化分析脚本
- ⚠️ 发现 YCSB 只能产生单一字段大小

### 2026-02-22
- ✅ 实现迁移策略日志输出
- ✅ 创建专用迁移测试脚本
- ✅ 突破 YCSB 限制，使用 Lua 脚本批量生成数据
- ✅ 实现 10+ GB 内存压力测试
- ✅ 覆盖 SLAB/Pool/Direct 三种分配方案

## 参考资料

- 当前测试文档：`docs/ycsb_migration_workload_design.md`
- NUMA 开发日志：`NUMA_DEVELOPMENT_LOG.md`
- 迁移策略文档：`docs/modules/07-numa-composite-lru.md`

## 文件保留原则

所有归档文件按以下原则保留：
1. **脚本文件**：保留所有历史版本，用于追溯演进过程
2. **测试结果**：保留代表性结果，删除重复测试
3. **工作负载**：保留特殊配置，删除通用配置
4. **日志文件**：保留关键日志，定期清理大文件（> 50MB）

---

**最后更新**：2026-02-22
**维护者**：NUMA 开发团队
