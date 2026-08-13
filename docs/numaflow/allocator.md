# NUMAflow 独立内存分配器（`nf_alloc`）

> 针对原 `numa_pool`（libnuma 临时方案）的性能与碎片率优化。

## 1. 现状诊断

原 `numa_pool.c` 的底层 `numa_alloc_onnode` 本质是 `mmap + mbind`，其内置 slab 分配器存在
三个相对 jemalloc 的短板：

1. **每对象 16 字节 PREFIX**：8B 对象实占 24B（+200%），32B 对象实占 48B（+50%）；
2. **位图分配 O(n)**：`bitmap_find_and_set` 线性扫位 + CAS，无 free-list 的 O(1) 弹出；
3. **无线程本地缓存**：所有分配竞争全局 slab 位图与锁。

## 2. 设计（综合 4 类最新方案）

| 来源 | 借鉴要点 |
| --- | --- |
| llalloc (USENIX ATC'23) | NUMA 节点身份由地址区间/段元数据承载，而非逐对象头部 |
| snmalloc | **无 per-object header**：size/node 从 segment 元数据 O(1) 反查 |
| mimalloc | **per-thread tcache** + 批量 refill（free-list 分片思想） |
| jemalloc | 精确 size class + segment 复用，约束内部碎片 |

架构（纯 C11，`numaflow/src/nf_alloc.c`）：

```text
nf_alloc_t（后端抽象：Linux+libnuma 用 numa_alloc_onnode/mmap+mbind，其它平台用 malloc）
  ├─ 40 个精确 size class（8B..16KB；>16KB 走 large，header 前缀）
  ├─ segment（64KB）：切 slot，LIFO free-list，metamap 有序数组二分定位
  ├─ metamap：地址 -> segment，O(log n) 反查 size/node（无逐对象 header）
  └─ tcache（_Thread_local）：alloc 快路径无锁；refill 从共享 free-list 批量搬运
```

关键收益：**小对象零 header 开销**（节点/大小在 segment 元数据里），分配 O(1)，多线程
alloc 无锁。

## 3. 基准（本机 Windows/MinGW，Redis 风格对象分布）

`numaflow/bench/bench_alloc.c`（`N=200000`，55% 16..112B + 30% 128..1KB + 12% 1..16KB + 3% >16KB）：

| 指标 | nf_alloc | malloc | |
| --- | --- | --- | --- |
| 单线程吞吐 | **3.0M ops/s** | 1.6M ops/s | **1.85×** |
| 2/4/8 线程 | 1.3M / 0.3M / 0.2M | 1.0M / 0.2M / 0.1M | 1.3–1.8× |
| 内部碎片 | **3.82%** | 0%（_msize） | |
| 外部开销（header 摊销） | 6.01% | — | |

说明：内部碎片来自 size-class 对齐；相对 `numa_pool` 的 +16B PREFIX（8B 对象 +200%），
`nf_alloc` 去掉了这一固定开销，对小对象密集的 Redis 键值负载是显著的内存节省。

## 4. Redis 集成路径

`nf_alloc` 通过 `nf_alloc_backend_t` 抽象底层：在 Linux + libnuma 上把 `chunk_alloc` 指向
`numa_alloc_onnode`（或 `mmap + mbind`），即可获得与 `numa_pool` 相同的 NUMA 绑定能力，
同时享受无 header + tcache 的性能。替换/增强 `numa_pool` 的入口在 `src/zmalloc.c` 的
NUMA 分配路径（`numa_zmalloc_onnode`），把 slab 层换成 `nf_alloc_malloc_onnode` 即可；
需在 Linux + libnuma 环境完成最终编译验证（本机无 libnuma，仅用 malloc 后端做了等价评测）。

## 5. 测试

```bash
cd numaflow && make test          # 含 tests/test_alloc.c（功能）
cd numaflow && make              # 后运行 bench/bench_alloc（需手动编译或 make）
```
