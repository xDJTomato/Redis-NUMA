# 大对象路径性能问题与优化方案

## 背景

最新 Size Sweep 结果显示，Redis-NUMA 在小对象区间能够保持与 Vanilla Redis 接近的吞吐表现，但当 value size 增大到 64KB 及以上时，吞吐开始明显下降；当对象超过当前 Slab 管理上限后，Redis-NUMA 回退到 direct NUMA allocation 路径，性能进一步低于本地内存绑定的 Vanilla Redis。

该现象说明当前原型的主要性能边界已经从小对象 Slab 分配转移到大对象分配、节点绑定、迁移复制和内存复用机制上。Size Sweep 测试因此不仅用于衡量不同 value size 下的吞吐变化，也揭示了 Redis-NUMA 在大对象负载下需要进一步优化的关键路径。

## 实验观察

在最新的 size sweep 测试中，Redis-NUMA 与 Vanilla Redis 的吞吐比例大致如下：

| Value 大小 | Redis-NUMA / Vanilla Redis |
| ---: | ---: |
| 32B | 0.98 |
| 64B | 1.00 |
| 128B | 0.94 |
| 256B | 0.90 |
| 512B | 0.91 |
| 1KiB | 0.58 |
| 2KiB | 0.92 |
| 4KiB | 0.87 |
| 8KiB | 0.83 |
| 16KiB | 0.86 |
| 32KiB | 0.87 |
| 64KiB | 0.53 |
| 128KiB | 0.50 |
| 256KiB | 0.45 |
| 512KiB | 0.45 |
| 1MiB | 0.41 |

该结果可以分为三个区间解释：

1. 32B 至 512B 区间，Redis-NUMA 与 Vanilla Redis 基本接近，说明小对象 Slab 路径能够有效摊薄 NUMA 元数据、节点统计和分配器管理开销。
2. 1KiB 至 32KiB 区间，Redis-NUMA 大体保持在 Vanilla Redis 的 80% 至 90% 左右，说明扩展后的 Slab 路径已经缓解了中等对象频繁走 direct path 的问题，但仍存在 prefix、统计和访问追踪带来的额外成本。
3. 64KiB 及以上区间，Redis-NUMA 性能快速下降，128KiB 至 1MiB 区间仅为 Vanilla Redis 的约 40% 至 50%。这说明当前 direct NUMA allocation 路径已经成为大对象场景下的主要瓶颈。

## 当前分配路径边界

当前 NUMA 分配器采用分层路径：

```text
8B–64KB      -> Slab 路径
>64KB        -> Direct NUMA allocation 路径
```

Slab 路径通过 size class、节点独立 Slab 池、位图槽位和 tcache 缓存减少频繁系统调用。Direct 路径则直接调用 NUMA 节点绑定分配接口，为单个对象申请目标节点上的内存。该路径实现简单，但在大对象高频分配、更新和迁移场景下开销较大。

## 性能下降原因

### 1. Direct NUMA allocation 频繁进入内核态

Direct 路径依赖 `numa_alloc_onnode` 完成节点绑定分配，底层可能涉及 `mmap`、`mbind`、页表建立、VMA 管理和 page fault 等内核态操作。对于 128KB、256KB、512KB 和 1MiB value，YCSB 的 load、update、replace 和迁移流程会频繁触发大对象分配。与 Vanilla Redis 使用的成熟 allocator arena/extent 复用机制相比，Redis-NUMA 的 direct path 更容易产生高额系统调用成本。

### 2. 大对象缺少 extent 级复用

Vanilla Redis 的分配器能够通过 arena、extent、dirty page、muzzy page 和 thread cache 等机制复用大块内存区域。当前 Redis-NUMA direct path 更接近逐对象分配模式：对象分配时申请 NUMA 绑定内存，释放时归还对应区域，缺少 per-node large extent pool、per-size-class freelist 和延迟归还操作系统机制。因此，大对象负载下系统会更频繁地触碰内核内存管理路径。

### 3. 对象越大，迁移和更新复制成本越高

Redis-NUMA 的对象迁移通常需要在目标节点上重新分配内存、复制旧对象内容、更新对象指针和释放旧内存。对于 128KB 至 1MiB 的 value，单次迁移的 `memcpy` 成本明显高于小对象。如果迁移触发阈值没有随对象大小调整，系统可能在热点收益尚未覆盖迁移成本前就执行大对象迁移，从而降低前台吞吐。

### 4. NUMA 元数据和统计开销无法被 direct path 摊薄

Redis-NUMA 每个对象均带有 prefix 元数据，并在分配、释放、访问和迁移时维护节点归属、热度、访问次数、内存占用和路径统计。小对象路径可以通过 Slab 和 tcache 将这类开销摊薄，而 direct path 缺少批量复用机制，使元数据维护成本与系统调用成本同时叠加。

### 5. 64KB 上限仍然不足以覆盖大对象测试区间

当前 Slab 上限扩展到 64KB 后，已经解决了 8KB、16KB 和 32KB 对象逐对象 page 级分配的问题，但 Size Sweep 覆盖到 128KB、256KB、512KB 和 1MiB。对于这些 value size，系统仍会进入 direct path，因此性能差距在超过 64KB 后重新扩大。

## 优化方案

### 方案一：引入 per-node huge extent pool

推荐将当前分配器扩展为三级结构：

```text
small slab       : 8B–4KB
large slab       : 5KB–64KB
huge extent pool : 128KB–1MiB
direct path      : >1MiB 或 extent pool 分配失败
```

Huge extent pool 可以为 128KB、256KB、512KB 和 1MiB 建立独立 size class。每个 NUMA 节点维护独立 extent 池，每个 extent 通过一次 `mmap` 和 `mbind` 绑定到目标节点，然后在用户态按固定大小槽位切分对象。

建议配置如下：

| 对象大小 | 建议 extent 大小 | 每个 extent 可容纳对象数 |
| ---: | ---: | ---: |
| 128KB | 2MB 或 4MB | 16 或 32 |
| 256KB | 4MB | 16 |
| 512KB | 8MB | 16 |
| 1MiB | 16MB | 16 |

该方案能够将多次大对象分配合并为少量 extent 分配，减少 `mmap`、`mbind` 和 `munmap` 次数，并使释放对象时只归还 extent 内部槽位，而不是立即归还操作系统。

### 方案二：为 direct path 增加大对象缓存

作为短期优化，可以先在 direct path 上增加 per-node、per-size-class 的大对象 freelist。释放 128KB、256KB、512KB 和 1MiB 对象时，不立即调用 NUMA 释放接口，而是将其放入对应节点和大小级别的缓存链表；后续同类分配直接复用缓存对象并重置 prefix 元数据。

可设置每个 size class 的缓存上限，例如：

```text
128KB: 64 个
256KB: 32 个
512KB: 16 个
1MiB : 8 个
```

该方案实现成本低，适合快速验证 direct allocation 是否为主要瓶颈。若启用缓存后 128KB 至 1MiB 区间吞吐明显提升，则说明系统调用和节点绑定分配成本是性能下降的主要原因。

### 方案三：迁移策略引入 size-aware threshold

大对象迁移不应与小对象使用完全相同的热度阈值。对象越大，迁移成本越高，需要更多后续访问才能摊销迁移开销。因此迁移策略应引入基于对象大小的阈值调节：

```text
≤4KB        : hotness >= 1
8KB–64KB    : hotness >= 2 或 4
128KB–512KB : hotness >= 8 或 16
≥1MiB       : 默认不主动迁移，除非持续极热
```

更进一步，可以采用收益模型判断：

```text
access_count × remote_access_penalty > object_size / memory_bandwidth
```

只有当预期远端访问节省收益超过迁移复制成本时，系统才执行大对象迁移。

### 方案四：限制大对象迁移带宽

对于 128KB 以上对象，应设置后台迁移预算，例如每秒最多迁移 64MB 或 128MB 数据。这样可以避免大对象迁移线程占用过多内存带宽，降低对前台 YCSB 请求吞吐和延迟的干扰。

该机制可以与 Composite-LRU 策略结合：策略仍然负责发现热点对象，但迁移执行层根据对象大小、迁移预算和节点带宽压力决定是否立即执行、延迟执行或跳过迁移。

### 方案五：延迟迁移或分阶段迁移大对象

对于超大对象，可以采用 lazy migration：先记录其热点状态和目标节点，不立即复制对象内容，而是在后台低负载窗口或带宽压力较低时迁移。对于持续热点对象，多轮检测均满足阈值后再迁移；对于短暂热点对象，则避免无收益搬迁。

## 推荐实施顺序

1. **短期验证**：实现 direct path 大对象缓存，优先覆盖 128KB、256KB、512KB 和 1MiB，验证减少 `numa_alloc_onnode` 次数后吞吐是否提升。
2. **正式优化**：实现 per-node huge extent pool，将 128KB 至 1MiB 纳入用户态池化管理，减少系统调用和页级分配成本。
3. **策略优化**：在迁移策略中加入 size-aware threshold、迁移收益判断和迁移带宽预算，避免大对象迁移对前台业务造成过高干扰。
4. **评估验证**：重新运行 Size Sweep，重点比较 64KB、128KB、256KB、512KB 和 1MiB 区间的吞吐、平均延迟、p99 延迟、RSS 和迁移计数变化。

## 论文表述建议

Size Sweep 实验表明，Redis-NUMA 在小对象区间能够保持与 Vanilla Redis 接近的吞吐表现，说明基于 NUMA 节点划分的 Slab 分配路径在细粒度对象管理上具有可接受的运行开销。然而，随着 value size 增大，系统吞吐开始明显下降，尤其当对象大小超过当前 Slab 管理上限后，Redis-NUMA 需要回退到直接 NUMA 分配路径。该路径依赖 `numa_alloc_onnode` 完成节点绑定分配，可能频繁触发 `mmap`、`mbind`、页表建立和页故障处理等内核态操作，缺少类似 jemalloc arena/extent 的大块内存复用机制。因此，在 128KB 至 1MiB 的大对象区间，Redis-NUMA 的吞吐仅为 Vanilla Redis 的约 40% 至 50%。这一结果说明当前原型更适合小对象和中等对象场景，而大对象场景需要进一步引入 per-node large extent pool、大对象缓存以及基于对象大小的迁移收益判断机制，以降低直接分配和大块数据迁移带来的额外开销。
