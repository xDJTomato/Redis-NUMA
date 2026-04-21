# 11 - 分配路径埋点与 RSS 差异探索

## 背景

QEMU 双 NUMA 节点环境下 Redis-CXL 实例 RSS 超过 9GB，而理论数据负载仅 ~5.3GB（300万 key × 1800B），RSS - used_memory 差距达 ~2.9GB。单 NUMA 节点仅 ~190MB。

---

## Phase 1：分配路径埋点

### 方法

在 `zmalloc.c` 的 NUMA 分配入口新增 6 个全局原子计数器，按分配路径分别记录：

- `numa_alloc_slab_bytes/count` — Slab 路径（≤128B 小对象）
- `numa_alloc_pool_bytes/count` — Pool 路径（129B–4KB）
- `numa_alloc_direct_bytes/count` — Direct 路径（>4KB）

通过 `NUMA CONFIG STATS` 命令暴露，采集脚本每秒写入 CSV。

### 结果对比

#### 本机（单 NUMA 节点）

| 指标 | Phase 1 末 | Phase 2 末 | Phase 3 末 |
|------|-----------|-----------|-----------|
| used_memory | 6624 MB | 6656 MB | 6656 MB |
| RSS | 6813 MB | 6846 MB | 6846 MB |
| RSS - used | **189 MB** | **190 MB** | **190 MB** |
| Slab | 1.37 GB | 1.44 GB | 1.44 GB |
| Pool | 5.54 GB | 5.85 GB | 5.92 GB |
| Direct | 260 MB | 312 MB | 380 MB |

#### QEMU（双 NUMA 节点，distance=50）— 原始 libc 基线

| 指标 | Phase 1 末 | Phase 2 末 | Phase 3 末 |
|------|-----------|-----------|-----------|
| used_memory | 6649 MB | 6656 MB | - |
| RSS | 9574 MB | 9481 MB | - |
| RSS - used | **2925 MB** | **2825 MB** | - |
| Slab | 1.37 GB | 1.44 GB | - |
| Pool | 5.80 GB | 6.30 GB | - |
| Direct | 522 MB | 732 MB | - |

两侧 used_memory 几乎一致（~6.6GB），slab/pool/direct 占比也一致。

---

## Phase 2：Pool+Slab 预分配

### 方案

将 Pool chunk（256KB/512KB/1MB）和 Slab（16KB）改为单次大 mmap 预分配，内部用 bump-pointer 切片。

- `numa_pool_prealloc()`: 为每个 NUMA 节点预分配 512MB 大块
- `numa_pool_alloc_from_prealloc()`: 从预分配块中 bump-pointer 切片
- 修改 `alloc_new_chunk()` 和 `alloc_new_slab()` 优先使用预分配块

### 结果

| 指标 | 单节点（预分配） | 单节点（原版） | 双节点（预分配） |
|------|----------------|-------------|----------------|
| Phase 1 gap | - | 189 MB | 1626 MB |
| Phase 3 gap | - | 190 MB | - |

差距从 190MB 恶化至 1626MB，反而更差。

---

## Phase 3：jemalloc per-node Arena

### 方案

利用 jemalloc `extent_hooks_t` API 为每个 NUMA 节点创建独立 arena：

1. `je_mallctl("arenas.create", ...)` 创建 arena
2. 安装自定义 `extent_hooks_t`，其中 `alloc` 回调使用 `numa_alloc_onnode(64MB, node)` 获取大块
3. jemalloc 在 arena 内部做小对象切片
4. 分配时通过 `je_mallocx(size, MALLOCX_ARENA(arena_ind))` 指定节点

### 实现

**新文件**：`src/numa_jemalloc.h` / `src/numa_jemalloc.c`

**修改文件**：`src/Makefile`、`src/numa_pool.c`（6 处替换）、`src/server.c`、`src/server.h`

### 编译问题

1. `zmalloc.h` 中 `zmalloc_size` 宏与 `zmalloc.c` 函数定义冲突 → `#undef` 修复
2. `redis-cli`/`redis-benchmark` 链接 undefined reference → `numa_jemalloc.o` 加入所有 OBJ 列表
3. `server.c` 多余的 `#endif` → 删除

### 结果

| 指标 | jemalloc per-node arena | vanilla jemalloc（对照） | 原始 libc（基线） |
|------|------------------------|----------------------|----------------|
| Phase 1 gap | 2784 MB | 2927 MB | 2925 MB |
| Phase 2 gap | 2561 MB | 2713 MB | 2825 MB |
| Phase 3 gap | **2543 MB** | **2723 MB** | - |

Direct 分配计数从 302 万降到 2.8 万（减少 99%），RSS 差距从 2723MB 降到 2543MB（仅降 6.6%）。

### 回滚

所有改动已通过 `git checkout -- . && git clean -fd src/numa_jemalloc.h src/numa_jemalloc.c` 完全回滚。

---

## 现象记录

### 现象 1：单双节点对比

单 NUMA 节点 RSS - used ≈ 190MB，双 NUMA 节点 RSS - used ≈ 2.9GB。used_memory 相同，分配路径占比相同。

### 现象 2：预分配使差距恶化

将大量小 mmap 合并为单次大 mmap（512MB）后，单节点差距从 190MB 恶化至 1626MB。

### 现象 3：mmap 调用减少 99% 几乎无效

jemalloc per-node arena 将 mmap 调用从 300 万级降到 2.8 万（减少 99%），RSS 差距仅从 2723MB 降到 2543MB（降 6.6%）。

### 现象 4：纯直接分配路径不膨胀

早期代码版本中跳过 pool/slab，全部走纯直接分配路径（`numa_alloc_onnode()` 对每个小对象单独调用），在 QEMU 双节点环境下未出现 RSS 膨胀。

### 现象 5：vanilla Redis 无膨胀

原版 Redis（jemalloc，无 NUMA 绑定）在 QEMU 双节点下 RSS < used_memory。

### 现象 6：QEMU NUMA distance=50

QEMU 虚拟 NUMA 拓扑中跨节点 distance 为 50（真实硬件通常为 10-20）。
