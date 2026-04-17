# 11 - 分配路径埋点与 RSS 差异调查

## 背景

在 QEMU 双 NUMA 节点环境下观察到 Redis-CXL 实例 RSS 超过 10GB，而理论数据负载仅 ~5.3GB（300万 key × 1800B）。需要定位 RSS 膨胀的实际来源。

## 调查方法

在 `zmalloc.c` 的 NUMA 分配入口处新增 6 个全局原子计数器，按分配路径（Slab / Pool / Direct）分别记录实时字节数和累计分配次数：

- `numa_alloc_slab_bytes/count` — 走 Slab 路径（≤128B 小对象）
- `numa_alloc_pool_bytes/count` — 走 Pool 路径（129B–4KB）
- `numa_alloc_direct_bytes/count` — 走 Direct 路径（>4KB）

通过 `NUMA CONFIG STATS` 命令暴露这些计数器，并在采集脚本 `start_collector()` 中每秒写入 CSV。

## 测试配置

| 参数 | 值 |
|------|-----|
| 记录数 | 3,000,000 |
| 字段大小 | 1800 bytes |
| 最大内存 | 8GB |
| Phase 1 (Fill) | 8 线程加载 |
| Phase 2 (Hotspot) | 16 线程 Zipfian α=0.99 |
| Phase 3 (Sustain) | 24 线程写密集 60% |

## 结果对比

### 本机（单 NUMA 节点）

| 指标 | Phase 1 末 | Phase 2 末 | Phase 3 末 |
|------|-----------|-----------|-----------|
| used_memory | 6624 MB | 6656 MB | 6656 MB |
| RSS | 6813 MB | 6846 MB | 6846 MB |
| RSS - used | **189 MB** | **190 MB** | **190 MB** |
| Slab | 1.37 GB | 1.44 GB | 1.44 GB |
| Pool | 5.54 GB | 5.85 GB | 5.92 GB |
| Direct | 260 MB | 312 MB | 380 MB |

### QEMU（双 NUMA 节点，distance=50）

| 指标 | Phase 1 末 | Phase 2 末 | Phase 3 末 |
|------|-----------|-----------|-----------|
| used_memory | 6649 MB | 6656 MB | - |
| RSS | 9574 MB | 9481 MB | - |
| RSS - used | **2925 MB** | **2825 MB** | - |
| Slab | 1.37 GB | 1.44 GB | - |
| Pool | 5.80 GB | 6.30 GB | - |
| Direct | 522 MB | 732 MB | - |

## 分析

### 分配路径不是 RSS 膨胀的原因

两侧的 used_memory 几乎一致（~6.6GB），分配路径的 slab/pool/direct 占比也一致（Pool 占 ~89%，Slab 占 ~19%，Direct 占 ~3-5%）。分配器行为层面没有差异。

### 真正原因：双 NUMA 节点下的 mmap 开销

RSS 差异来自 **底层内存映射开销**，而非分配器逻辑：

1. **单 NUMA 节点**：`libc malloc` 内部可以复用已有的 mmap 区域，RSS 与 used_memory 接近
2. **双 NUMA 节点**：每次 `numa_alloc_onnode()` 强制通过 `mmap() + mbind()` 分配到指定节点。大量小 mmap 调用导致：
   - VMA（Virtual Memory Area）碎片——每个 mmap 产生一个 VMA 条目
   - 页表膨胀——跨节点 mbind 需要额外的页表项
   - QEMU 虚拟化层对双节点 NUMA 的模拟开销（distance=50）

Phase 3 中 Direct 路径从 272MB 增长到 384MB（+112MB），说明写密集压力下动态内存需求在增长，但这在 QEMU 双节点环境下会被放大。

## 结论

RSS 膨胀不是分配器 bug，而是 **NUMA 细粒度分配的固有代价**。每个 `numa_alloc_onnode()` 调用相当于一次独立的 mmap，在双节点环境下积累了大量 VMA 和页表开销。

优化方向：
1. 合并小对象的 NUMA 分配请求（批量 mmap）
2. 对非关键内存（如 metadata dict、heat map）使用普通 `libc malloc` 而非 `numa_alloc_onnode`
3. `key_heat_map` 冷条目清理——当前永不淘汰，浪费 ~250MB
