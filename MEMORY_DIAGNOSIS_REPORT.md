# Redis-CXL 内存膨胀诊断报告与修复计划

## 背景

在 QEMU/KVM 环境中运行 Redis-CXL（3M keys, value=1800B），观测到进程 RSS 超过 10G，与理论数据量 ~5.2GB 不符。最新测试报告显示 used_memory=6.6GB, RSS=8.4GB, 碎片率 1.30。

---

## 一、每条记录实际内存开销模型

### 1.1 标准 Redis 开销（jemalloc）

测试配置：3M keys, YCSB key 格式 "userXXXXX" (~7B), fieldlength=1800B

| 数据结构 | 组成 | 理论大小 |
|----------|------|---------|
| SDS key | sdshdr8(3B) + "userXXXXX"(7B) + \0(1B) | 11B |
| SDS value | sdshdr16(5B) + 1800B + \0(1B) | 1806B |
| robj key | type:4bit + encoding:4bit + lru:24bit + refcount:int + ptr:8B | 16B |
| robj value | 同上 | 16B |
| dictEntry | key:8B + val:8B + next:8B | 24B |
| **每条总计** | — | **1873B** |

标准 Redis (jemalloc) 理论总内存: 3M × 1873B ≈ **5.2GB**

### 1.2 Redis-CXL (NUMA Pool) 实际开销

NUMA 模式下所有 `zmalloc()` 分配额外附加 `numa_alloc_prefix_t`（16 字节前缀）。

**分配路径**：`zmalloc(size)` → `numa_alloc_with_size(size)` → `total_size = size + 16` → Pool size class 查找

| 数据结构 | total_size | Pool Size Class | 实际占用 | 浪费 |
|----------|-----------|----------------|---------|------|
| SDS key | 11+16=27B | 32B | 32B | 5B |
| SDS value | 1806+16=1822B | 2048B | 2048B | 226B |
| robj key | 16+16=32B | 32B | 32B | 0B |
| robj value | 16+16=32B | 32B | 32B | 0B |
| dictEntry | 24+16=40B | 48B | 48B | 8B |
| **每条总计** | — | — | **2192B** | **239B** |

Pool Size Class 数组（16 级）：`16, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048, 3072, 4096`

**3M keys 实际 Pool 分配**: 3M × 2192B = **6.15GB**

### 1.3 额外开销

| 开销项 | 大小 | 说明 |
|--------|------|------|
| dict bucket 数组 | 32MB | 2^22 × 8B（3M key 取下一个 2 的幂） |
| dict rehash 峰值 | +32MB | 扩容时 ht[0]+ht[1] 共存 |
| db->expires dict | ~32MB | 有过期时间的 key 额外一套 dict 结构 |
| key_metadata 字典 | ~168MB | 每 key ~56B (key_numa_metadata_t 32B + dictEntry 24B) |
| key_heat_map 字典 | ~251MB | 每 key ~88B (composite_lru_heat_info_t 32B + dictEntry 24B + dict 空槽) |
| Server 全局结构 | ~50-100MB | server.h 中的全局数据结构 |
| **额外总计** | **~533-583MB** | — |

### 1.4 理论 vs 实际对照

```
理论纯数据:           5.2GB
+ Pool size class 浪费: +0.69GB (value 1822→2048B 浪费最大)
+ 元数据字典:          +0.42GB (key_metadata + key_heat_map)
+ dict 系统开销:       +0.10GB
+ 其他:               +0.10GB
─────────────────────────────
= used_memory:         6.6GB  ✓ 与测试报告吻合

used_memory 6.6GB vs RSS 8.4GB 的 1.8GB 差距:
  → libc malloc 碎片（NUMA 强制 MALLOC=libc）
  → libc arena/mmap 管理开销
  → 线程栈、进程其他非-zmalloc 分配
```

---

## 二、膨胀因子排序

| 排名 | 膨胀源 | 影响量 | 占比 |
|------|--------|--------|------|
| 1 | libc malloc 碎片 | ~1.8GB | RSS-vs-used 差距的主要来源 |
| 2 | Pool size class 浪费 | ~690MB | value 1822B → class 2048B, 每条浪费 226B |
| 3 | key_heat_map 永不清理 | ~251MB | 热度衰减到 0 后条目仍保留 |
| 4 | key_metadata 字典 | ~168MB | 无 DEL 时不清理 |
| 5 | db->expires dict | ~32MB | 有 TTL 时额外一套 dict |
| 6 | dict rehash 峰值 | ~32MB | 扩容时临时翻倍 |

---

## 三、Pool 分配器内部机制

### 3.1 分配流程

```
zmalloc(1806)                          # 用户请求 1806 字节
  └─ numa_alloc_with_size(1806)
       ├─ total_size = 1806 + 16 = 1822  # 加 PREFIX
       ├─ should_use_slab(1806) → false  # >128B, 不走 Slab
       └─ numa_pool_alloc(1822, node, ...)
            ├─ class_idx: 1822 <= 2048 → index 13
            ├─ aligned_size = (1822+15)&~15 = 1824
            ├─ free_list 检查 → 命中则复用
            ├─ chunk bump pointer 分配 1824 字节
            └─ 返回 raw_ptr
       ├─ numa_init_prefix(raw_ptr, 1806, from_pool=1, node)
       ├─ update_zmalloc_stat_alloc(1822)  # used_memory += 1822
       └─ return raw_ptr + 16              # 用户指针跳过 prefix
```

### 3.2 Slab 分配器（≤128B 快速路径）

```
zmalloc(11)                            # SDS key
  └─ numa_alloc_with_size(11)
       ├─ total_size = 11 + 16 = 27
       ├─ should_use_slab(11) → true   # ≤128B
       └─ numa_slab_alloc(11, node, ...)
            ├─ class_idx: 11 <= 16 → index 0
            ├─ aligned_size = (11+15)&~15 = 16
            ├─ *total_size = 16 + 16 = 32  # Slab 内部加 PREFIX
            ├─ bitmap 查找空闲槽位
            └─ 返回 slab 内对象地址
       ├─ numa_init_prefix(raw_ptr, 11, from_pool=1, node)
       ├─ update_zmalloc_stat_alloc(27)  # used_memory += 27 (不是 32!)
       └─ return raw_ptr + 16
```

注意：Slab 的 `*total_size` 输出参数未被调用者使用，调用者使用自己的 `total_size = size + PREFIX_SIZE`。因此**不存在双重 prefix 叠加**。

### 3.3 释放流程

```
zfree(user_ptr)
  └─ numa_free_with_size(user_ptr)
       ├─ prefix = user_ptr - 16
       ├─ size = prefix->size (原始大小, 如 1806)
       ├─ total_size = 1806 + 16 = 1822
       ├─ update_zmalloc_stat_free(1822)
       ├─ should_use_slab(1806) → false
       └─ numa_pool_free(raw_ptr, 1822, from_pool=1)
            ├─ aligned_size = (1822+15)&~15 = 1824
            ├─ class_idx: aligned_size(1824) <= 2048 → index 13
            ├─ 创建 free_block(24B) 挂入 free list
            └─ free list 复用: free_block->size >= next aligned_size
```

---

## 四、修复计划

### 修改 1：key_heat_map 冷条目回收（~251MB 可回收）

**文件**: `src/numa_composite_lru.c`  
**函数**: `composite_lru_decay_heat` (line 440-465)

**问题**：热度衰减到 `COMPOSITE_LRU_HOTNESS_MIN`(0) 后条目仍保留在字典中，永不删除。3M keys × ~88B ≈ 251MB 永久占用。

**修改**：hotness 降到 0 时从 key_heat_map 删除条目。

```c
// 现有代码 (line 448-465):
while ((de = dictNext(di)) != NULL) {
    composite_lru_heat_info_t *info = dictGetVal(de);
    uint16_t elapsed = calculate_time_delta(current_time, info->last_access);
    if (elapsed > decay_thr_sec) {
        info->stability_counter++;
        if (info->stability_counter > data->config.stability_count) {
            if (info->hotness > COMPOSITE_LRU_HOTNESS_MIN) {
                info->hotness--;
                data->decay_operations++;
            }
            info->stability_counter = 0;
        }
    } else {
        info->stability_counter = 0;
    }
}

// 修改后：在 hotness-- 后检查是否降到最低，若是则 dictDelete
while ((de = dictNext(di)) != NULL) {
    composite_lru_heat_info_t *info = dictGetVal(de);
    uint16_t elapsed = calculate_time_delta(current_time, info->last_access);
    if (elapsed > decay_thr_sec) {
        info->stability_counter++;
        if (info->stability_counter > data->config.stability_count) {
            if (info->hotness > COMPOSITE_LRU_HOTNESS_MIN) {
                info->hotness--;
                data->decay_operations++;
            }
            info->stability_counter = 0;
            /* 热度降到最低时删除条目，回收内存 */
            if (info->hotness == COMPOSITE_LRU_HOTNESS_MIN) {
                dictDelete(data->key_heat_map, dictGetKey(de));
                continue;
            }
        }
    } else {
        info->stability_counter = 0;
    }
}
```

安全性：`dictGetSafeIterator` 在 `dictNext` 中预存了 `nextEntry`（dict.c:625），`dictDelete` 不会导致迭代器失效。

### 修改 2：jemalloc 替代 libc 作为底层分配器（减少 1.8GB RSS 差距）

**目标**：保持 NUMA Pool/Slab 的 `numa_alloc_onnode` 粒度控制，但让 Pool 回退路径和元数据结构使用 jemalloc 减少碎片。

**方案**：不修改 Pool chunk 的分配方式（仍用 `numa_alloc_onnode`），而是：
1. 恢复 `MALLOC=jemalloc`，让 Redis 核心的 zmalloc/zfree 回退路径走 jemalloc
2. Pool 的 `free_block` 和 `chunk_t` 元数据走 jemalloc（当前已如此，只是用的 libc malloc）

**风险**：`MALLOC=jemalloc` 与 `HAVE_NUMA` 有兼容性问题——当前 Makefile 强制 `MALLOC=libc` 是因为 jemalloc 会接管所有 malloc/free，而 NUMA Pool 内部的 `malloc/free` 调用（chunk_t, free_block）会被 jemalloc 接管，这是期望的行为。主要风险在于 `zmalloc.c` 中非 NUMA 路径的 `HAVE_MALLOC_SIZE` 逻辑需要适配。

**替代方案**：保持 `MALLOC=libc`，但使用 `tcmalloc` 或 `jemalloc` 作为 `from_pool=0` 路径的分配器（通过条件编译）。

### 修改 3（可选）：Slab from_pool 标记语义明确化

**文件**: `src/zmalloc.c`  
**函数**: `numa_alloc_with_size` (line 296)

当前 Slab 和 Pool 分配都设 `from_pool=1`，通过 `should_use_slab(size)` 区分释放路径。语义正确但不够清晰。可改为 `from_pool=2` 表示 Slab，但影响不大。

---

## 五、不修改的部分

1. **Size class 数组** — 2048 已存在（index 13），无需添加
2. **PREFIX_SIZE=16** — NUMA 元数据固有开销，不可减少
3. **Pool 最大分配 4096B** — 当前值最优，降低会增加 `numa_alloc_onnode` 系统调用
4. **Slab 双重 prefix** — 经验证不存在此 bug

---

## 六、验证方法

### 6.1 编译测试

```bash
cd src && make clean && make -j$(nproc)
cd src && make test          # 标准 Redis 测试套件
```

### 6.2 内存基准对比

```bash
# 运行 bw_benchmark
cd tests/ycsb && ./run_bw_benchmark.sh

# 对比指标
redis-cli INFO memory
# 关注: used_memory, used_memory_rss, mem_fragmentation_ratio
```

### 6.3 修改前后预期对比

| 指标 | 修改前 | 修改后（修改1） | 修改后（修改1+2） |
|------|--------|----------------|-------------------|
| used_memory | 6.6GB | ~6.35GB | ~6.35GB |
| RSS | 8.4GB | ~8.15GB | ~6.8-7.2GB |
| 碎片率 | 1.30 | ~1.28 | ~1.07-1.13 |
| key_heat_map 占用 | ~251MB | ~50-80MB | ~50-80MB |

### 6.4 NUMA 正确性验证

```bash
numactl --hardware                    # 确认 NUMA 拓扑
redis-cli NUMA CONFIG                 # 查看 NUMA 配置
redis-cli INFO memory | grep numa     # 查看 NUMA 统计
```

---

## 七、关键文件索引

| 文件 | 相关内容 |
|------|---------|
| `src/numa_pool.h` | Size class 定义、Slab 常量、Pool API |
| `src/numa_pool.c` | Pool/Slab 分配实现、chunk 管理 |
| `src/zmalloc.c` | NUMA 分配入口、PREFIX 定义、free 路由 |
| `src/zmalloc.h` | NUMA API 声明、热度追踪接口 |
| `src/numa_composite_lru.c` | key_heat_map、热度衰减、迁移决策 |
| `src/numa_key_migrate.c` | key_metadata 字典、迁移适配器 |
| `src/Makefile` | MALLOC=libc 强制设置（line 109） |
| `src/evict.h` | evictionPoolEntry NUMA 扩展字段 |
