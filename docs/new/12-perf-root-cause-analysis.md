# 12 - 性能根因分析：吞吐腰斩与 RSS 膨胀

## 测试环境

- **平台**: QEMU VM，6 核 i7-12700H，11GB RAM
- **NUMA**: 双节点（distance=50），Node 0=DRAM 4GB，Node 1=CXL 8GB
- **编译**: 两者均为 `MALLOC=libc`
- **负载**: YCSB bw_saturate，80 万条 × 5KB 值，Phase 2 64 线程 80%READ/20%UPDATE

---

## Phase 1 — 数据加载（纯 SET）

| 指标 | CXL 版本 | Vanilla | 差距 |
|------|---------|---------|------|
| 吞吐 | 8,528 ops/s | 15,914 ops/s | **vanilla 1.87x** |
| INSERT avg | 934 us | 499 us | vanilla 1.87x |
| INSERT p99 | 5,295 us | 2,849 us | vanilla 1.86x |
| 总耗时 | 93.8 秒 | 50.3 秒 | vanilla 1.87x |
| frag_ratio | 1.58–1.59 | 1.02 | CXL RSS 多 56% |

---

## Phase 2 — 热点读写

| 指标 | CXL 版本 | Vanilla | 差距 |
|------|---------|---------|------|
| 吞吐 | 8K–12K ops/s | 48K–59K ops/s | **vanilla 4–5x** |
| READ avg | 5,100–8,900 us | 1,270–1,450 us | vanilla 4–6x |
| READ p99 | 15,000–36,000 us | 5,400–9,000 us | vanilla 3–5x |
| RSS | ~6.5 GB | ~4.2 GB | CXL 多 2.3GB |
| frag_ratio | 1.66–1.72 | 1.02–1.30 | 持续膨胀 |
| 迁移数/秒 | 200–400 | 0 | CXL 额外 CPU 开销 |

---

## 根因分析

### 1. 分配路径双重全局锁（最大瓶颈）

`zmalloc()` 热路径调用链：

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

一次 SET 触发 4+ 次分配（SDS key + SDS value + robj + dictEntry），每次都经过**双重加锁**。64 线程并发时争用严重。

**文件**: `src/numa_configurable_strategy.c:85-170`（`select_best_node`），`src/numa_pool.c:222-320`（`numa_pool_alloc`）

### 2. PREFIX 膨胀 + Chunk 预分配

每条分配带 16 字节 `numa_alloc_prefix_t` 头。Pool 分配器预分配 chunk（256KB/512KB/1MB），bump-pointer 分配后 chunk 剩余空间形成内部碎片。`frag_ratio = 1.59` 恒定说明不是渐进泄漏，而是**chunk 粒度 + free_list 不归还 OS** 的结构性开销。

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

## 修复优先级

| 优先级 | 问题 | 位置 | 方案 |
|--------|------|------|------|
| **P0** | 分配路径全局锁 | `numa_configurable_strategy.c:90-167` | 改为 per-thread 或 atomic 无锁选择 |
| **P0** | Pool size-class 锁 | `numa_pool.c:254-292` | 考虑 per-thread cache 或减少锁粒度 |
| **P1** | free_list 不归还 OS | `numa_pool.c:495-503` | free_list 清理时补 `numa_free(ptr, size)` |
| **P1** | chunk used_bytes 不递减 | `numa_pool.c:271-274` | 释放时追踪 ptr→chunk，递减 used_bytes |
| **P2** | 迁移 CPU 开销 | `numa_composite_lru.c:590-713` | 迁移限速或临时关闭对比验证 |

---

## 验证方式

1. `perf top -p $(pidof redis-server)` 查看热点函数分布
2. `NUMA CONFIG STATS` 查看分配路径计数器
3. 分阶段修复：先修 P0 锁 → 测 Phase 1 → 再修 P1 → 测 RSS → 最后 P2
