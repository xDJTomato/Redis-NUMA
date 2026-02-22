# YCSB NUMA 迁移策略负载设计文档

## 设计目标

设计一个能够充分触发 Redis NUMA Composite LRU 迁移策略的 YCSB 工作负载，并覆盖 Redis 的三种内存分配方案：
1. **SLAB 分配器**：≤ 128B 的小对象
2. **Pool 分配器**：129B - 16KB 的中等对象
3. **直接分配**：> 16KB 的大对象

## 设计原理

### 1. 内存分配方案覆盖

Redis NUMA 内存分配器根据对象大小选择不同的分配策略：

```
┌─────────────────────────────────────────────┐
│         内存分配决策树                       │
├─────────────────────────────────────────────┤
│ size ≤ 128B    → SLAB 分配器 (快速)        │
│ 129B ≤ size ≤ 16KB → Pool 分配器 (中等)    │
│ size > 16KB    → 直接分配 (mmap/malloc)     │
└─────────────────────────────────────────────┘
```

**工作负载设计**：
- 使用 YCSB 的多字段功能（`fieldcount=10`）
- 每个字段大小不同，确保覆盖所有分配方案
- Redis hash 存储会为每个字段分配独立的内存块

### 2. 热点访问模式

为了触发迁移策略，需要制造明显的**跨节点热点访问**：

```
访问分布：Hotspot (80/20 法则)
- 80% 的操作访问 20% 的热点数据
- 热点数据被频繁访问，热度值快速上升
- 当热度达到阈值且发生远程访问时，触发迁移
```

**配置参数**：
```properties
requestdistribution=hotspot
hotspotdatafraction=0.2    # 20% 的数据是热点
hotspotopnfraction=0.8     # 80% 的操作访问热点
```

### 3. 读写比例

```
读写比例设计：
- Read:  70% (主要操作，触发热度检测)
- Update: 20% (修改热点数据，增加访问频率)
- Insert: 10% (持续增加新数据)
- Scan:   0%  (避免影响性能测试)
```

**设计理由**：
- 读操作占主导，模拟真实缓存场景
- 频繁读取热点数据会触发远程访问检测
- 更新操作保证数据变化，测试迁移后的一致性

### 4. 字段大小分布策略

#### 方案 A: 基础配置（当前实现）

```properties
fieldcount=10
fieldlength=20  # 基础字段长度

# 字段分布（YCSB 默认行为）：
# field0-9: 每个字段约 20B
# 总大小：约 200B → Pool 分配
```

**问题**：无法覆盖 SLAB 和 Direct 分配

#### 方案 B: 混合大小配置（推荐）

需要修改 YCSB 或使用 Redis 命令直接生成数据：

```bash
# 小对象 (SLAB)
redis-cli HSET user:1:small name "Alice" age "25"  # ~50B

# 中等对象 (Pool)
redis-cli HSET user:1:medium bio "$(head -c 500 < /dev/urandom | base64)"  # ~500B

# 大对象 (Direct)
redis-cli HSET user:1:large avatar "$(head -c 20000 < /dev/urandom | base64)"  # ~20KB
```

#### 方案 C: 自定义 YCSB 生成器（最佳方案）

创建自定义字段生成器，为不同字段设置不同大小：

```java
// 伪代码
field0-3: size = 20B   (SLAB)
field4-7: size = 500B  (Pool)
field8-9: size = 18KB  (Direct)
```

### 5. 迁移触发条件

Composite LRU 策略的迁移触发条件：

```c
// 触发条件（src/numa_composite_lru.c）
if (mem_node != current_node &&           // 远程访问
    current_hotness >= migrate_threshold)  // 热度达到阈值
{
    // 加入待迁移队列
    enqueue_migration(key, target_node);
}
```

**关键参数**：
- `migrate_hotness_threshold`: 默认 5
- `hotness` 范围: 0-7
- 每次本地访问：`hotness++`
- 热度衰减周期：默认 10 秒

## 工作负载配置文件

### workload_numa_migration_full

```properties
# 基础配置
recordcount=100000
operationcount=1000000
workload=site.ycsb.workloads.CoreWorkload

# 读写比例
readproportion=0.7
updateproportion=0.2
insertproportion=0.1
scanproportion=0

# 热点访问
requestdistribution=hotspot
hotspotdatafraction=0.2
hotspotopnfraction=0.8

# 字段配置
fieldcount=10
fieldlength=20

# 并发配置
threadcount=50

# Redis 配置
redis.host=127.0.0.1
redis.port=6379
redis.timeout=5000
```

## 测试脚本

### run_migration_test.sh

**功能**：
1. 启动 Redis (VERBOSE 日志)
2. 配置 Composite LRU 策略
3. 执行 YCSB Load 阶段
4. 执行 YCSB Run 阶段
5. 收集迁移统计
6. 分析日志中的迁移事件
7. 生成测试报告

**使用方法**：
```bash
cd tests/ycsb
./run_migration_test.sh
```

## 日志分析

### 关键日志标识

#### 1. 策略初始化
```
[Composite LRU] Strategy initialized: migrate_threshold=5, decay_threshold=10000000us, stability_count=3
```

#### 2. 远程访问检测
```
[Composite LRU] Remote access detected: val=0x7f8b4c001234, current_node=0, mem_node=1, hotness=6, threshold=5
```

表示：
- 对象在节点 1
- 当前访问来自节点 0（远程访问）
- 热度为 6，超过阈值 5
- **即将触发迁移评估**

#### 3. 迁移触发
```
[Composite LRU] *** MIGRATION TRIGGERED *** key=0x7f8b4c005678, target_node=0, priority=6, pending_time=1234us
```

表示：
- 迁移已触发
- 目标节点：0
- 优先级（热度）：6
- 在队列中等待时间：1234μs

#### 4. 热度衰减
```
[Composite LRU] Executing heat decay cycle
```

周期性执行，降低不活跃对象的热度

#### 5. 待迁移队列处理
```
[Composite LRU] Processing 15 pending migrations
```

处理待迁移队列中的对象

## 测试验证

### 验证迁移策略生效

```bash
# 1. 查看 Redis 日志
tail -f tests/ycsb/results/redis_migration_test.log | grep "MIGRATION"

# 2. 统计迁移次数
grep -c "MIGRATION TRIGGERED" tests/ycsb/results/redis_migration_test.log

# 3. 分析热点分布
grep "Remote access detected" tests/ycsb/results/redis_migration_test.log | \
    awk '{print $9}' | sort | uniq -c
```

### 预期结果

正常情况下应该观察到：
1. ✅ 远程访问检测：数百到数千次
2. ✅ 迁移触发：数十到数百次
3. ✅ 热度衰减：周期性执行（每 10 秒）
4. ✅ 待迁移队列：非空并定期处理

### 异常情况排查

| 现象 | 可能原因 | 解决方案 |
|------|---------|---------|
| 无远程访问检测 | 单节点系统 | 使用 `numactl` 绑定多个节点 |
| 无迁移触发 | 热度未达阈值 | 增加操作数或降低阈值 |
| 队列一直为空 | 资源检查失败 | 检查资源状态函数 |
| 日志无输出 | 日志级别不够 | 确保 `loglevel verbose` |

## 内存分配验证

### 验证三种分配方案

由于 YCSB 默认配置只能产生 Pool 分配的对象，需要手动验证：

```bash
# 启动 Redis
./src/redis-server --loglevel verbose --logfile redis_alloc_test.log

# 创建不同大小的对象
redis-cli SET small_key "$(head -c 64 < /dev/urandom | base64)"    # SLAB
redis-cli SET medium_key "$(head -c 5000 < /dev/urandom | base64)"  # Pool
redis-cli SET large_key "$(head -c 20000 < /dev/urandom | base64)"  # Direct

# 查看日志中的分配信息
grep "numa_zmalloc" redis_alloc_test.log
```

## 改进建议

### 短期改进

1. **修改 YCSB 字段生成**
   - 自定义字段长度分布
   - 为不同字段设置不同大小

2. **增加日志详细度**
   - 记录每次分配的大小和类型
   - 记录 SLAB/Pool/Direct 分配计数

3. **添加实时监控**
   - 使用 Redis INFO 命令监控内存使用
   - 定期输出热度分布统计

### 长期改进

1. **开发专用测试工具**
   - 直接调用 Redis C API
   - 精确控制对象大小和访问模式

2. **集成 NUMA 性能计数器**
   - 使用 `perf` 工具监控 NUMA 访问
   - 统计本地/远程访问比例

3. **自动化验证框架**
   - 自动分析日志
   - 生成可视化报告
   - CI/CD 集成

## 参考资料

- YCSB 官方文档: https://github.com/brianfrankcooper/YCSB
- Redis NUMA 内存池设计: `docs/modules/01-numa-pool.md`
- Composite LRU 策略文档: `docs/modules/07-numa-composite-lru.md`
- zmalloc 分配器实现: `src/zmalloc.c`

## 变更历史

| 版本 | 日期 | 作者 | 说明 |
|------|------|------|------|
| v1.0 | 2026-02-14 | System | 初始版本 |
