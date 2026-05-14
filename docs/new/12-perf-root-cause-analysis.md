# 12 - 性能根因分析：吞吐腰斩与 RSS 膨胀

## 测试环境

### 初始测试（QEMU 虚拟 NUMA）

- **平台**: QEMU VM，6 核 i7-12700H，11GB RAM
- **NUMA**: 双节点（distance=50），Node 0=DRAM 4GB，Node 1=CXL 8GB
- **编译**: 两者均为 `MALLOC=libc`
- **负载**: YCSB bw_saturate，80 万条 × 5KB 值，Phase 2 64 线程 80%READ/20%UPDATE

### 当前测试（真实双路 NUMA 服务器）

- **平台**: Dell 双路服务器，192.168.12.204
- **NUMA**: 真实双路 NUMA 拓扑
- **编译**: 两者均为 `MALLOC=libc`
- **负载**: YCSB bw_saturate，100 万条，Phase 2 64 线程 Zipfian α=0.99

---

## 初始结果（QEMU，优化前）

### Phase 1 — 数据加载（纯 SET）

| 指标 | CXL 版本 | Vanilla | 差距 |
|------|---------|---------|------|
| 吞吐 | 8,528 ops/s | 15,914 ops/s | **vanilla 1.87x** |
| INSERT avg | 934 us | 499 us | vanilla 1.87x |
| INSERT p99 | 5,295 us | 2,849 us | vanilla 1.86x |
| 总耗时 | 93.8 秒 | 50.3 秒 | vanilla 1.87x |
| frag_ratio | 1.58–1.59 | 1.02 | CXL RSS 多 56% |

### Phase 2 — 热点读写

| 指标 | CXL 版本 | Vanilla | 差距 |
|------|---------|---------|------|
| 吞吐 | 8K–12K ops/s | 48K–59K ops/s | **vanilla 4–5x** |
| READ avg | 5,100–8,900 us | 1,270–1,450 us | vanilla 4–6x |
| READ p99 | 15,000–36,000 us | 5,400–9,000 us | vanilla 3–5x |
| RSS | ~6.5 GB | ~4.2 GB | CXL 多 2.3GB |
| frag_ratio | 1.66–1.72 | 1.02–1.30 | 持续膨胀 |
| 迁移数/秒 | 200–400 | 0 | CXL 额外 CPU 开销 |

---

## 最新结果（真实 NUMA 服务器，全部优化后）

### 基准对比（2026-05-15，tcache 优化后）

| Phase | NUMA 版 | Vanilla | 差距 |
|-------|---------|---------|------|
| P1 Fill | 36,031 ops/s | 33,566 ops/s | **NUMA +7.3%** |
| P2 Hotspot | 77,721 ops/s | 80,054 ops/s | vanilla +3.0% |
| P3 Sustain | 76,377 ops/s | 86,014 ops/s | vanilla +12.6% |

**关键改善**：
- Phase 1（纯写入）NUMA 版已稳定反超 vanilla（+7.3%），得益于 local_first 策略的 NUMA 亲和分配
- Phase 2（热点读写）差距从初始的 4-5x 缩小到仅 3.0%，tcache 消除了 Slab 分配器竞争
- Phase 3（持续负载）差距 12.6%，主要来自 NUMA PREFIX 开销和热度追踪逻辑

### NUMA 本地命中率

| 配置 | node-loads | node-load-misses | 命中率 | L3→local DRAM |
|------|-----------|------------------|--------|---------------|
| local_first | 60.8M | 11.8M | 80.6% | 97.0% |
| interleave | 47.2M | 35.0M | 26.0% | 78.7% |
| vanilla | 57.3M | 16.8M | 70.8% | 95.9% |

local_first 策略在 L3→Local DRAM 命中率上达到 97.0%，超过 vanilla 的 95.9%，验证了 NUMA 感知分配的有效性。interleave 策略因数据分散到两个节点，命中率显著低于其他配置。

---

## 根因分析

### 1. 分配路径全局锁（最大瓶颈）**[已修复]**

`zmalloc()` 热路径调用链（修复前）：

```
zmalloc() → numa_alloc_with_size() → numa_config_get_best_node()
    → select_best_node()
        → pthread_mutex_lock(&g_config_mutex)     ← 全局锁 #1
        → 策略选择（LOCAL_FIRST/INTERLEAVE/...）
        → pthread_mutex_unlock(&g_config_mutex)
    → numa_pool_alloc() / numa_slab_alloc()
        → pthread_mutex_lock(&pool->lock)          ← size-class 锁 #2
        → Bump Pointer / Free List
        → pthread_mutex_unlock(&pool->lock)
```

**修复方案**：`select_best_node()` 已改为无锁设计（详见 [13-lockfree-alloc-design.md](13-lockfree-alloc-design.md)）：
- 配置字段 plain read（写入频率极低，最坏情况仅影响一次分配的节点选择）
- 统计计数器使用 `atomicIncr` 无锁更新
- WEIGHTED_INTERLEAVE 策略通过 `atomicGet` 读取压力权重
- Pool 路径已移除，Slab 分配器使用原子 CAS 无锁操作

一次 SET 触发 4+ 次分配（SDS key + SDS value + robj + dictEntry），修复前每次都经过双重加锁。修复后分配路径完全无锁。

### 2. PREFIX 膨胀 **[已优化]**

每条分配带 16 字节 `numa_alloc_prefix_t` 头。Slab 分配器使用 64KB slab + 原子位图管理，碎片率已优化至 ~1%。`frag_ratio` 从 1.59 降至 1.04-1.17。

### 3. 后台迁移引擎消耗 CPU

Phase 2 中 CXL 版本每秒迁移 200–400 个 key，每次迁移触发：
- `numa_zmalloc_onnode()` 新分配
- `memcpy()` 大块数据（5KB 值）
- `zfree()` 旧内存
- PREFIX 更新

在高并发下直接与业务请求竞争 CPU 和内存带宽。

### 4. Per-access 热度追踪（已部分优化）

`lookupKey()` → `composite_lru_record_access()` 在每次 key 访问时执行 PREFIX 读写、衰减计算。已通过以下优化部分缓解：
- CPU node 缓存（每 64 次刷新）
- `server.lruclock` 复用
- 单节点提前返回
- 本地 MAX 热度跳过写入
但双节点场景仍执行这些逻辑。

### 5. QEMU NUMA distance=50

QEMU 虚拟 NUMA 跨节点访问延迟远高于真实硬件（10-20）。Interleave 策略将数据分散到两个节点，导致约半数访问跨节点。

---

## 修复状态

| 优先级 | 问题 | 位置 | 方案 | 状态 |
|--------|------|------|------|------|
| **P0** | 分配路径全局锁 | `numa_configurable_strategy.c` | 改为 atomic 无锁选择 | ✅ 已修复 |
| **P0** | 分配器锁 | `numa_pool.c` | Pool 路径移除，Slab 原子 CAS 无锁 | ✅ 已修复 |
| **P0** | 默认策略 | `server.c` | INTERLEAVE → WEIGHTED_INTERLEAVE | ✅ 已修复 |
| **P1** | 路径合并 | `zmalloc.c` | Pool+Slab 合并为纯 Slab（≤4KB） | ✅ 已修复 |
| **P0** | Slab 分配器竞争 | `zmalloc.c` | Thread-Local Cache (tcache)，per-thread bin 避免 CAS/mutex | ✅ 已修复 |
| **P2** | Size class 线性查找 | `zmalloc.c` | O(1) 查找表 `g_class_lookup[]` | ✅ 已修复 |
| **P2** | 迁移 CPU 开销 | `numa_composite_lru.c` | 带宽感知限速 + overload 阻断 | ✅ 已实现 |

---

## 验证方式

1. `perf top -p $(pidof redis-server)` 查看热点函数分布
2. `NUMA CONFIG STATS` 查看分配路径计数器及 tcache 命中率
3. 分阶段修复：先修 P0 锁 → 测 Phase 1 → 再修 P1 → 测 RSS → 最后 P2
4. `tests/ycsb/run_full_test.sh` 一键全量测试（含基准对比、perf 火焰图、NUMA 本地命中率）

## NUMA 本地命中率可视化

测试流水线通过 `numa_locality_measure.sh` + `visualize_locality.py` 生成两份报告：
- **locality_report.png** — 汇总柱状图：Node Load 命中率、L3→Local DRAM 比例、内存分布、吞吐量
- **locality_report_timeseries.png** — 逐秒时序图：吞吐量、命中率、内存用量、碎片率、Key 迁移

数据来源：
- PMU 计数器：`perf stat -I 1000 -e node-loads,node-load-misses,mem_load_l3_miss_retired.{local,remote}_dram`
- Redis 指标：`INFO memory` (used_memory, RSS, frag_ratio) + `NUMA MIGRATE STATS` (migration counts)
