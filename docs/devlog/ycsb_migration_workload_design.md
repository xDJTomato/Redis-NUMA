# YCSB NUMA 迁移策略负载设计文档

## 最新更新（2026-02-22）

### 高强度测试实现

已实现 **10+ GB 内存压力**测试，成功覆盖三种内存分配方案：

**测试脚本**：
- `tests/ycsb/run_migration_high_pressure_v2.sh` - 优化版高强度测试

**测试结果**：
- **内存使用**：10.20 GB / 16.00 GB
- **键数量**：919,994 条
- **内存碎片率**：1.20
- **数据分布**：
  - 小对象 (64B): 200K × 64B = 12.8 MB (SLAB)
  - 中对象 (4KB): 400K × 4KB = 1.6 GB (Pool)
  - 大对象 (32KB): 200K × 32KB = 6.4 GB (Direct)
  - 超大对象 (128KB): 20K × 128KB = 2.5 GB (Direct)

**性能表现**：
- 数据加载速度：
  - 小对象：200K keys/s
  - 中对象：133K keys/s
  - 大对象：66K keys/s
  - 超大对象：10K keys/s
- 热点访问性能：
  - SET：155K ops/s
  - GET：121K ops/s

**技术要点**：
- 使用 Lua 脚本批量生成数据（比 redis-cli pipe 更快）
- 多阶段数据加载策略，分别覆盖不同大小级别
- 集成 redis-benchmark 进行热点访问测试

---

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

## 使用教程

### 快速开始

#### 1. 环境准备

确保已安装必要的工具：

```bash
# 检查 Redis 是否已编译
ls -lh src/redis-server src/redis-cli

# 检查 Java 环境（YCSB 需要）
java -version

# 下载 YCSB（如果尚未下载）
cd tests/ycsb
wget https://github.com/brianfrankcooper/YCSB/releases/download/0.17.0/ycsb-0.17.0.tar.gz
tar -xzf ycsb-0.17.0.tar.gz
```

#### 2. 基础测试（推荐新手）

使用简化版测试脚本，快速验证功能：

```bash
cd tests/ycsb

# 运行基础迁移测试（约 2 分钟）
./run_migration_test.sh
```

**预期输出**：
- Redis 启动成功
- NUMA 策略配置完成
- YCSB Load 和 Run 阶段执行
- 迁移日志分析
- 测试报告生成

#### 3. 高强度测试（10+ GB 内存压力）

使用优化版脚本进行大规模测试：

```bash
cd tests/ycsb

# 确保有足够内存（建议 20GB+）
free -h

# 运行高强度测试（约 5-10 分钟）
./run_migration_high_pressure_v2.sh
```

**测试阶段**：
1. **阶段 1**: 加载小对象 (64B × 200K) - SLAB 分配
2. **阶段 2**: 加载中对象 (4KB × 400K) - Pool 分配
3. **阶段 3**: 加载大对象 (32KB × 200K) - Direct 分配
4. **阶段 4**: 加载超大对象 (128KB × 20K) - Direct 分配
5. **阶段 5**: 热点访问测试 (100 万次操作)

**预期结果**：
- 内存使用：~10 GB
- 键数量：~820K
- 吞吐量：SET 155K ops/s, GET 121K ops/s

### 自定义测试

#### 修改工作负载参数

编辑 `workloads/workload_numa_migration_full`：

```properties
# 调整记录数（影响内存使用）
recordcount=2000000  # 增加到 200 万

# 调整操作数（影响测试时长）
operationcount=5000000  # 500 万次操作

# 调整线程数（影响并发压力）
threadcount=100  # 100 线程并发

# 调整热点比例
hotspotdatafraction=0.1  # 10% 热点数据
hotspotopnfraction=0.9   # 90% 操作访问热点
```

#### 创建自定义工作负载

复制模板并修改：

```bash
cd tests/ycsb/workloads

# 复制模板
cp workload_numa_migration_full my_custom_workload

# 编辑配置
vim my_custom_workload
```

示例配置：

```properties
# 我的自定义工作负载
recordcount=1000000
operationcount=2000000

# 纯读测试
readproportion=1.0
updateproportion=0.0
insertproportion=0.0

# 极端热点（90/10）
requestdistribution=hotspot
hotspotdatafraction=0.1
hotspotopnfraction=0.9

# 小对象测试
fieldcount=1
fieldlength=64  # 64B，测试 SLAB 分配
```

#### 手动运行 YCSB

```bash
cd tests/ycsb/ycsb-0.17.0

# Load 阶段
bin/ycsb load redis -s \
  -P ../workloads/my_custom_workload \
  -p "redis.host=127.0.0.1" \
  -p "redis.port=6379"

# Run 阶段
bin/ycsb run redis -s \
  -P ../workloads/my_custom_workload \
  -p "redis.host=127.0.0.1" \
  -p "redis.port=6379"
```

### 结果分析

#### 查看测试报告

```bash
cd tests/ycsb/results

# 查看最新报告
ls -lt *.txt | head -1 | awk '{print $NF}' | xargs cat

# 查看详细日志
tail -100 redis_high_pressure.log
```

#### 分析迁移日志

```bash
# 策略初始化
grep "Strategy initialized" results/redis_high_pressure.log

# 远程访问检测
grep "Remote access detected" results/redis_high_pressure.log | wc -l

# 迁移触发事件
grep "MIGRATION TRIGGERED" results/redis_high_pressure.log

# 热度衰减周期
grep "Executing heat decay cycle" results/redis_high_pressure.log | wc -l
```

#### 查看 Redis 统计

```bash
# 内存使用
redis-cli INFO memory | grep -E "used_memory|fragmentation"

# 键空间统计
redis-cli INFO keyspace

# 命令统计
redis-cli INFO stats | grep total_commands_processed
```

### 故障排查

#### Redis 启动失败

```bash
# 检查端口占用
sudo lsof -i :6379

# 杀掉占用进程
sudo fuser -k 6379/tcp

# 查看错误日志
tail -50 tests/ycsb/results/redis_high_pressure.log
```

#### YCSB 连接失败

```bash
# 测试 Redis 连接
redis-cli -h 127.0.0.1 -p 6379 ping

# 检查 Redis 配置
redis-cli CONFIG GET protected-mode
redis-cli CONFIG GET bind
```

#### 内存不足

```bash
# 检查可用内存
free -h

# 减少测试规模
vim workloads/workload_numa_migration_full
# 将 recordcount 减半

# 或增加 Redis maxmemory
redis-cli CONFIG SET maxmemory 8gb
```

### 性能调优

#### 提高加载速度

```bash
# 禁用持久化
redis-cli CONFIG SET save ""
redis-cli CONFIG SET appendonly no

# 增加 Lua 脚本批处理大小
# 编辑 run_migration_high_pressure_v2.sh
# 修改 batch_size=5000 为更大值（如 10000）
```

#### 减少测试时间

```bash
# 减少操作数
vim workloads/workload_numa_migration_full
operationcount=1000000  # 从 500 万减到 100 万

# 减少记录数
recordcount=500000  # 从 200 万减到 50 万
```

### 多节点环境测试

如果有真实的 NUMA 多节点服务器：

```bash
# 查看 NUMA 拓扑
numactl --hardware

# 将 Redis 绑定到节点 0
numactl --cpunodebind=0 --membind=0 ./src/redis-server &

# 将 YCSB 客户端绑定到节点 1
numactl --cpunodebind=1 --membind=1 \
  tests/ycsb/ycsb-0.17.0/bin/ycsb run redis \
  -P tests/ycsb/workloads/workload_numa_migration_full
```

这样会产生跨节点访问，触发迁移策略。

---

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
