# 09-memory-fragmentation-analysis.md - NUMA内存池碎片问题深度分析

## 文档概述

本文档基于实际压力测试数据，深入分析NUMA简单内存池的碎片问题，为优化工作提供数据支撑和技术方向。

**相关文档**: [01-numa-pool.md](./01-numa-pool.md) - NUMA内存池基础设计

---

## 问题发现

### 测试环境

- **测试时间**: 2026-02-14
- **测试模式**: 快速测试 (1.5GB目标)
- **系统环境**: 单NUMA节点, 15.6GB总内存
- **测试参数**: 20并发客户端, 管道深度8, 批次50K keys

### 关键数据对比

#### 测试轮次1 (11:46 - 目标2GB)

| 指标 | 值 |
|------|-----|
| 目标内存 | 2048MB |
| 实际RSS | 2426MB |
| 有效数据 | 1026MB |
| 碎片率 | **2.36** |
| 内存效率 | 42% |
| 浪费内存 | 1400MB (58%) |
| 填充成功 | 49万keys (24%) |

#### 测试轮次2 (12:24 - 目标1.5GB)

**填充阶段**:
| 指标 | 值 |
|------|-----|
| 目标内存 | 1536MB |
| 实际RSS | 1560MB |
| 有效数据 | 470MB |
| 碎片率 | **3.31** |
| 内存效率 | 30% |
| 浪费内存 | 1090MB (70%) |

**性能测试后**:
| 指标 | 值 |
|------|-----|
| 实际RSS | 1677MB |
| 有效数据 | 464MB |
| 碎片率 | **3.61** |
| 内存效率 | 27% |
| 浪费内存 | 1213MB (73%) |

### 严重性评估 🔴🔴🔴

```
正常碎片率: < 1.3
可接受范围: 1.3 - 1.5
需要优化: 1.5 - 2.0
严重问题: > 2.0

当前碎片率: 3.61 (严重超标178%)
```

---

## 根因分析

### 1. 固定64KB Chunk的不适应性

#### 问题描述

当前所有size_class统一使用64KB chunk，对不同大小对象的适应性差异巨大。

#### 数据验证

**2KB对象场景** (测试主要场景):
```
64KB chunk ÷ 2KB对象 = 32个对象/chunk
利用率理论值: 100%
实际测量值: 27-30%

差距原因:
1. 对齐填充: 每个2KB对象实际占用2048+16(PREFIX) = 2064B
2. 内部碎片: 跨size_class对齐 (512→1024→2048)
3. chunk未满即废弃: bump pointer算法无法回填
```

**实际chunk利用率分布** (从memory_blocks.txt):
```
大内存块(>64MB): 5个  → 多chunk合并或直接mmap
中等块(1-64MB): 大量  → chunk部分使用
小块(<1MB): 散落    → 严重碎片化
```

#### 设计缺陷

```c
// 当前设计 (zmalloc.c)
#define NUMA_POOL_CHUNK_SIZE (64 * 1024)  // 所有size_class统一

// 问题:
// 16B对象: 64KB/16 = 4096个 → 过大,可能永远填不满
// 2KB对象: 64KB/2KB = 32个 → 不匹配,浪费严重
// 4KB对象: 64KB/4KB = 16个 → 勉强够用
```

### 2. 8级Size Class分级太粗

#### 当前分级

```c
const size_t size_classes[8] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};
```

#### 跨级浪费分析

**测试场景: 2KB SET操作**

Redis对象组成:
```
robj(16B) + sds_header(变长) + value(2048B) = 总计约2064-2100B
```

分配过程:
```
请求: ~2064B
匹配: size_class[7] = 2048B
实际: 向上对齐到下一级 (无下一级,直接2048B)

内部碎片: 2048 - 实际需要 = 可能为负!
→ 跨chunk分配或触发大对象路径
```

#### 覆盖不足的区间

```
16-32:   100% 增长 (浪费50%)
32-64:   100% 增长 (浪费50%)
64-128:  100% 增长 (浪费50%)
128-256: 100% 增长 (浪费50%)
256-512: 100% 增长 (浪费50%)
512-1024:100% 增长 (浪费50%)
1024-2048:100% 增长 (浪费50%)

缺失区间:
48, 96, 192, 384, 768, 1536, 3072, 4096 等
```

**实例**: 
- 请求100B → 分配128B → 浪费28B (22%)
- 请求300B → 分配512B → 浪费212B (41%)
- 请求1500B → 分配2048B → 浪费548B (27%)

### 3. 不释放策略导致碎片累积

#### 设计初衷

```c
/* 简化设计理念 */
// 1. Redis长期运行,内存会被快速重用
// 2. 避免复杂的free list管理
// 3. 减少并发控制复杂度
```

#### 实际问题

**碎片演化过程** (基于memory_timeline.csv):

```
阶段1 - 快速填充 (0-5秒):
  RSS: 598MB → 1560MB
  碎片率: 稳定在3.31
  特征: 大量新chunk分配,利用率尚可

阶段2 - 性能测试 (5-8秒):
  RSS: 1560MB → 1677MB (+117MB)
  碎片率: 3.31 → 3.61 (+9%)
  特征: 混合操作引入更多碎片

阶段3 - 稳定期 (8秒后):
  RSS: 稳定在1677MB
  碎片率: 稳定在3.61
  特征: 新分配少,碎片永久存在
```

**碎片类型**:

1. **外部碎片** (inter-chunk):
   - 已释放的对象空间无法跨chunk合并
   - chunk间存在大量空隙

2. **内部碎片** (intra-chunk):
   - size_class对齐浪费
   - chunk内对象间padding

3. **结构性碎片** (structural):
   - PREFIX开销 (16B * 对象数)
   - chunk元数据开销

### 4. Bump Pointer算法的局限

#### 算法描述

```c
// 当前分配逻辑 (简化)
void *allocate(size_t size) {
    chunk = find_chunk_with_space(size);
    if (!chunk) {
        chunk = allocate_new_chunk();
    }
    ptr = chunk->memory + chunk->offset;
    chunk->offset += aligned_size;
    return ptr;
}
```

#### 问题

1. **无法回填**:
   ```
   Chunk状态: [obj1][freed][obj2][obj3][free_space]
              ↑不能利用
   
   Bump pointer只能向后移动,中间freed空间永久浪费
   ```

2. **早期chunk废弃**:
   ```
   Chunk A: [obj][freed][freed][free_space] ← 80%空闲但不用
   Chunk B: [obj][obj][obj][free_space]     ← 新分配到这里
   ```

3. **热点chunk压力**:
   ```
   所有新分配集中在最新chunk
   → 该chunk频繁访问
   → 可能跨NUMA节点
   ```

---

## 碎片恶化机制

### 场景1: 2KB对象密集填充

```
初始状态: 空pool

分配过程:
1. 分配chunk1 (64KB)
2. 填充32个2KB对象 → chunk满
3. 分配chunk2 (64KB)
4. 填充32个2KB对象 → chunk满
...

问题:
- 如果对象大小略有变化 (2000B, 2100B)
- size_class跨级 → chunk利用率下降
- 2000B → 2048B分配 → 48B浪费
- 2100B → 可能跨chunk → 整个chunk浪费
```

**实测数据**:
```
计划填充: 480K个2KB对象
实际填充: 225K对象 (47%)
RSS占用: 1560MB
有效数据: 470MB (30%)
```

### 场景2: 混合操作加剧碎片

```
初始: 已有N个2KB对象

性能测试:
- SET: 新增小对象
- GET: 不影响
- LPUSH/RPOP: 新增list节点 (小对象)
- ZADD: 新增zset节点 (中等对象)

结果:
- 不同size_class对象混合
- chunk利用率进一步下降
- 碎片率: 3.31 → 3.61
```

### 场景3: 长期运行碎片累积

```
模拟1小时运行:
- 对象新增/删除交替
- chunk逐渐碎片化
- 无compact机制
- 碎片永久存在

预测:
- 1小时后碎片率: > 4.0
- 内存效率: < 20%
- 可用内存严重不足
```

---

## 数据驱动的发现

### 发现1: 碎片率随操作类型变化

```
纯填充: 3.31
+性能测试: 3.61 (+9%)
+混合操作: 预计3.8-4.0
```

**含义**: 不同操作对碎片的影响不同，需要针对性优化

### 发现2: 大内存块数量变化

```
填充后: 5个大块(>64MB)
测试后: 0个大块

推断:
1. libc可能有内存块合并
2. 或者被分裂成更小块
3. 碎片化程度加剧
```

### 发现3: 内存效率临界点

```
目标内存 vs 实际效率:
2048MB → 42% 效率
1536MB → 27% 效率

趋势: 目标越小,效率越低
原因: 固定开销(chunk元数据)占比更大
```

### 发现4: 填充成功率与碎片关系

```
第1轮: 目标2GB, 实际24%填充, 碎片2.36
第2轮: 目标1.5GB, 实际101%填充但碎片3.61

结论: 
- 不是填充不进去
- 而是浪费太严重导致提前达到物理内存上限
```

---

## 优化方向与优先级

### P0 - 立即实施 (1周内)

#### 1. 动态Chunk大小策略

**目标**: 根据size_class选择最优chunk大小

```c
// 优化方案
size_t get_chunk_size(size_t obj_size) {
    if (obj_size <= 256) {
        return 16 * 1024;   // 16KB: 64-256个对象
    } else if (obj_size <= 1024) {
        return 64 * 1024;   // 64KB: 64-256个对象
    } else if (obj_size <= 4096) {
        return 256 * 1024;  // 256KB: 64-256个对象
    } else {
        return 0;  // 直接mmap,不用pool
    }
}
```

**预期效果**:
- 16B对象: 16KB chunk, 利用率提升4倍
- 2KB对象: 64KB→256KB, 浪费减少75%
- 碎片率: 3.6 → 预计2.0以下

#### 2. 扩展Size Class到16级

**当前**: 8级, 跨度100%
**优化**: 16级, 跨度50%

```c
const size_t size_classes[16] = {
    16, 32, 48, 64,          // 细粒度小对象
    96, 128, 192, 256,       // 中小对象
    384, 512, 768, 1024,     // 中等对象
    1536, 2048, 3072, 4096   // 大对象
};
```

**预期效果**:
- 平均浪费: 50% → 25%
- 碎片率: 减少30-40%

#### 3. 碎片率监控与告警

```c
// 添加到INFO memory
mem_pool_fragmentation_ratio: 3.61
mem_pool_waste_bytes: 1268426710
mem_pool_efficiency: 27%

// 告警阈值
if (fragmentation_ratio > 2.5) {
    serverLog(LL_WARNING, "High memory fragmentation: %.2f", 
              fragmentation_ratio);
}
```

### P1 - 短期优化 (2-4周)

#### 4. 简单Compact机制

```c
// 阈值触发
if (fragmentation_ratio > 2.5) {
    compact_pool();
}

// Compact策略
void compact_pool() {
    // 1. 识别低利用率chunk (<30%)
    // 2. 迁移活跃对象到新chunk
    // 3. 释放旧chunk
    // 4. 更新引用
}
```

**复杂度**: 需要对象引用追踪
**预期效果**: 碎片率降低20-30%

#### 5. Free List管理

```c
// 记录freed空间
struct free_block {
    void *ptr;
    size_t size;
    struct free_block *next;
};

// 分配时优先使用free list
void *allocate(size_t size) {
    // 1. 查找free list
    block = find_free_block(size);
    if (block) return block;
    
    // 2. 使用bump pointer
    return bump_pointer_alloc(size);
}
```

**预期效果**: 降低30-40%碎片

### P2 - 中期重构 (1-2月)

#### 6. Slab Allocator引入

参考jemalloc/tcmalloc的slab设计:

```c
struct slab_class {
    size_t size;          // 对象大小
    size_t slab_size;     // slab大小
    void *free_list;      // 空闲对象链表
    size_t objects_per_slab;
};

// 每个size_class独立slab
slab_class pools[16];
```

**优势**:
- O(1)分配/释放
- 无碎片(同大小对象)
- 高缓存局部性

#### 7. Per-Thread Cache

```c
// 线程本地缓存
__thread struct {
    void *small_cache[8];   // 小对象cache
    size_t cache_count[8];
} tls_cache;

// 减少锁竞争
void *fast_alloc(size_t size) {
    if (tls_cache_available(size)) {
        return tls_cache_pop(size);
    }
    return pool_alloc(size);
}
```

---

## 测试与验证计划

### 阶段1: P0优化验证

**测试用例**:
```bash
# 1. 2KB对象纯填充
./numa_cxl_stress_test.sh fill 2048

# 验证指标:
碎片率目标: < 2.0 (当前3.61)
内存效率: > 60% (当前27%)
```

**成功标准**:
- ✅ 碎片率下降40%以上
- ✅ 内存效率提升100%以上
- ✅ 性能不下降

### 阶段2: 混合场景验证

```bash
# 2. 混合大小对象
# 256B, 1KB, 2KB, 4KB混合填充

# 验证指标:
碎片率目标: < 1.8
各size_class利用率: > 70%
```

### 阶段3: 长期稳定性验证

```bash
# 3. 长时间运行 (24小时)
# 模拟真实负载

# 验证指标:
碎片率增长: < 10%/天
内存泄漏: 0
OOM次数: 0
```

---

## 参考数据

### 诊断文件位置

```
/tests/reports/diagnosis_20260214_122356/
├── DIAGNOSIS_SUMMARY.txt    # 诊断摘要
├── fragmentation.txt         # 碎片率记录
├── memory_timeline.csv       # 内存使用曲线
├── memory_blocks.txt         # 内存块分布
├── numa_distribution.txt     # NUMA分布
└── redis_memory_info.txt     # Redis内存详情
```

### 关键数据摘录

**碎片率演化**:
```
Timestamp       Fragmentation
填充前          45.57 (空库)
填充后(5s)      3.31
测试后(8s)      3.61
```

**内存曲线** (memory_timeline.csv):
```
Time,RSS_MB,VSZ_MB,CPU%,Keys,UsedMem_MB
开始,598,630,5.8,196K,411
5秒,1560,1600,8.8,225K,470
8秒,1677,1708,4.6,311K,464
```

---

## 总结

当前NUMA简单内存池存在**严重的内存碎片问题**，碎片率高达3.61，内存效率仅27%，远低于可接受水平。

**核心问题**:
1. 固定64KB chunk不适应不同大小对象
2. 8级size_class分级太粗，跨级浪费严重
3. 不释放策略导致碎片永久累积
4. Bump pointer算法无法回填空间

**优化方向**:
- P0: 动态chunk大小 + 扩展size_class (立即)
- P1: Compact机制 + Free list (短期)
- P2: Slab allocator + Thread cache (中期)

**预期收益**:
- 碎片率: 3.61 → <1.3 (降低63%)
- 内存效率: 27% → >80% (提升196%)
- 内存浪费: 1.2GB → <300MB (减少75%)

优化完成后，NUMA内存池将从"可用但低效"提升到"生产级高效"水平。
