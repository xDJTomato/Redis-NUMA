# TinyLFU 热点数据迁移策略

## 模块概述

`numa_tinylfu.c/h` 实现了基于 **TinyLFU** 算法的 NUMA 热点数据快速发现与迁移策略，注册在策略插槽 **Slot 2**。算法参考 Caffeine（Ben Manes）的 Window-TinyLFU 设计，使用 Count-Min Sketch + Doorkeeper Bloom Filter 以固定 ~56KB 内存实现 O(1) 的访问频率估计。

**定位**：相比 Slot 1 的 Composite LRU（热度阶梯 + 惰性衰减），TinyLFU 提供更激进、更快速的热点发现能力，适用于访问模式变化剧烈的场景。

**默认状态**：注册后立即禁用，需手动启用以避免与 Composite LRU 冲突。

## 与 Composite LRU (Slot 1) 的对比

| 维度 | Composite LRU (Slot 1) | TinyLFU (Slot 2) |
|------|----------------------|------------------|
| **频率估计** | 8 级热度阶梯（0-7），per-key 惰性衰减 | 4-bit CMS 计数器（0-15），全局周期性减半 |
| **过滤机制** | 无（所有访问都更新热度） | Doorkeeper Bloom Filter 过滤一次性访问 |
| **内存开销** | O(n)（key_heat_map 字典，随 Key 数增长） | O(1)（固定 ~56KB，与 Key 数无关） |
| **衰减方式** | 阶梯式惰性衰减（按 Key 空闲时间） | 全局减半（每 100K 次操作，所有计数器右移 1 位） |
| **迁移触发** | 热度首次越过阈值时入队 | 频率达到阈值且数据在远程节点时入队 |
| **优先级** | HIGH | HIGH |
| **默认状态** | 启用 | 禁用 |

## 算法原理

### Window-TinyLFU 简化版

```
访问 Key
    │
    ▼
┌─────────────────────┐
│  Doorkeeper (Bloom)  │ ── 第一次见？ ──► 记录到 Bloom，跳过 CMS
│  65536 位, 2 哈希    │
└─────────┬───────────┘
          │ 已见过
          ▼
┌─────────────────────┐
│  Count-Min Sketch    │ ── 4 行 × 16384 列
│  4-bit 计数器        │ ── 递增对应位置
│  (64-bit hash 分段)  │
└─────────┬───────────┘
          │
          ▼
    freq = min(CMS 4 行) + 1
          │
          ├── freq ≥ 阈值 且 数据在远程节点
          │       └── 入队迁移候选环形缓冲区
          │
          └── total_ops ≥ reset_interval
                  └── CMS 全局减半 + Doorkeeper 清零
```

### 为什么需要 Doorkeeper？

在真实 Redis 负载中，大量 Key 只被访问一次（扫描操作、临时 Key 等）。如果直接递增 CMS 计数器，会造成「计数器污染」——稀有 Key 占用了本应属于热点 Key 的计数器空间。

Doorkeeper Bloom Filter 作为第一道防线：
- 首次访问：仅在 Bloom Filter 中标记，不触碰 CMS
- 二次访问：通过 Bloom Filter 检查后才递增 CMS

这使得 CMS 的有限计数器空间只分配给至少被访问两次的 Key。

## 核心数据结构

### Count-Min Sketch

```c
typedef struct {
    uint8_t *rows[4];       // 4 行，每行 8192 字节
    uint32_t width;         // 16384（列数，必须是 2 的幂）
    uint32_t width_mask;    // 16383（快速取模）
    uint32_t bytes_per_row; // 8192（每行字节 = width / 2）
} tinylfu_cms_t;
```

**4-bit packed 编码**：每个字节存储 2 个计数器（低 4 位 + 高 4 位），计数器上限为 15。

```c
// 读取：偶数列取低 nibble，奇数列取高 nibble
uint8_t cms_get(row, col) {
    byte = row[col >> 1];
    return (col & 1) ? (byte >> 4) : (byte & 0x0F);
}

// 全局减半：(byte >> 1) & 0x77 同时右移两个 nibble
void cms_halve(cms) {
    for each byte: row[j] = (row[j] >> 1) & 0x77;
}
```

**哈希分段**：从 64-bit SipHash 中按 16-bit 段导出各行列索引（Caffeine 风格）：

```c
uint32_t cms_index(hash, row, mask) {
    return ((uint32_t)(hash >> (row * 16))) & mask;
}
```

### Doorkeeper Bloom Filter

```c
typedef struct {
    uint8_t *bits;          // 位数组
    uint32_t num_bits;      // 65536 位 (CMS_DEPTH × width)
    uint32_t num_bytes;     // 8192 字节
} tinylfu_doorkeeper_t;
```

使用 2 个哈希函数（64-bit hash 的高/低 32 位），支持 `dk_test` / `dk_add` / `dk_clear` 操作。

### 迁移候选环形缓冲区

```c
typedef struct {
    sds      key;           // Key 名称（SDS 副本）
    void    *val;           // Value 指针
    void    *data_ptr;      // 数据分配指针（用于节点检测）
    int      target_node;   // 迁移目标节点
    uint8_t  freq_snapshot; // 入队时的频率快照
} tinylfu_candidate_t;
```

默认大小 512 条目，环形覆写。

### 可配置参数

```c
typedef struct {
    uint32_t cms_width;              // CMS 列数，默认 16384
    uint8_t  migrate_threshold;      // 触发迁移的最低频率，默认 3
    uint32_t reset_interval;         // 全局衰减间隔（操作次数），默认 100000
    uint32_t ring_size;              // 候选环形缓冲区大小，默认 512
    uint32_t migration_budget;       // 每次 serverCron 最多迁移数，默认 256
    int      auto_migrate_enabled;   // 1=开启自动迁移
    int      debug_logging_enabled;  // 1=打印调试日志
} tinylfu_config_t;
```

### 策略私有数据

```c
typedef struct {
    redisDb *db;                         // 数据库上下文（lookupKey 动态绑定）

    tinylfu_config_t config;             // 运行时配置
    tinylfu_cms_t cms;                   // Count-Min Sketch
    tinylfu_doorkeeper_t doorkeeper;     // Doorkeeper Bloom Filter

    uint64_t total_ops;                  // 全局操作计数（触发衰减）

    tinylfu_candidate_t *ring;           // 候选环形缓冲区
    uint32_t ring_head;                  // 写入游标
    uint32_t ring_count;                 // 当前有效数量

    // 统计
    uint64_t stat_accesses;              // 总访问次数
    uint64_t stat_doorkeeper_filtered;   // 被 doorkeeper 过滤次数
    uint64_t stat_candidates_enqueued;   // 入队候选数
    uint64_t stat_migrations_done;       // 完成迁移数
    uint64_t stat_migrations_failed;     // 迁移失败数
    uint64_t stat_resets;                // 全局衰减次数
    uint64_t stat_accesses_local;        // 本地访问次数
    uint64_t stat_accesses_remote;       // 远程访问次数
} tinylfu_data_t;
```

## 内存布局

| 组件 | 大小 | 说明 |
|------|------|------|
| CMS 行数据 | 4 × 8192 = 32 KB | 4 行 × 16384 列 × 0.5 字节 |
| Doorkeeper 位数组 | 65536 / 8 = 8 KB | 65536 位 |
| 候选环形缓冲区 | 512 × ~32 = ~16 KB | 512 条目 × sizeof(candidate) |
| **总计** | **~56 KB** | 固定大小，与 Key 数量无关 |

## 访问路径：tinylfu_record_access()

每次 `lookupKey()` 命中时调用（在 Slot 1 Composite LRU hook 之后）。

```c
void tinylfu_record_access(strategy, key_sds, val, data_ptr) {
    hash = dictGenHashFunction(key, sdslen(key));

    // 1. Doorkeeper 检查
    if (!dk_test(&doorkeeper, hash)) {
        dk_add(&doorkeeper, hash);    // 首次访问：仅标记
        stat_doorkeeper_filtered++;
        goto check_locality;          // 跳过 CMS，但统计本地/远程
    }

    // 2. 通过 Doorkeeper，递增 CMS
    cms_record(&cms, hash);           // 4 行各自递增

    // 3. 估计频率
    freq = cms_estimate(&cms, hash);  // 4 行最小值
    if (freq < 15) freq++;            // +1（doorkeeper 吸收了第一次）

    // 4. 迁移条件检查
    if (freq >= migrate_threshold && data_ptr != NULL) {
        data_node = numa_get_node_id(data_ptr);
        if (data_node >= 0 && data_node != main_thread_node) {
            ring_push(key, val, data_ptr, main_thread_node, freq);
            stat_candidates_enqueued++;
        }
    }

    // 5. 全局衰减检查
    total_ops++;
    if (total_ops >= reset_interval) {
        cms_halve(&cms);              // 所有计数器右移 1 位
        dk_clear(&doorkeeper);        // 清空 Bloom Filter
        total_ops = 0;
        stat_resets++;
    }
}
```

**热路径开销**：1 次 SipHash + 4 次位操作（Doorkeeper）+ 4 次数组读取（CMS），无字典查找、无内存分配。

## 执行路径：tinylfu_execute()

每秒由 `serverCron` → `numa_strategy_run_all()` 调用。

```c
int tinylfu_execute(strategy) {
    if (!auto_migrate_enabled || ring_count == 0) return 0;

    // 从最旧的候选开始处理
    for each candidate (up to migration_budget):
        // 重新估计频率（确认仍然是热点）
        hash = dictGenHashFunction(c->key, sdslen(c->key));
        current_freq = cms_estimate(&cms, hash);
        if (dk_test(&doorkeeper, hash) && current_freq < 15)
            current_freq++;

        if (current_freq >= migrate_threshold) {
            ret = numa_migrate_key_by_name(db, c->key, c->target_node);
            if (ret == 0) stat_migrations_done++;
            else stat_migrations_failed++;
        }

        sdsfree(c->key);   // 释放 SDS 副本
        c->key = NULL;

    // 处理完毕，清空环形缓冲区
    ring_count = 0;
    ring_head = 0;
}
```

## 全局衰减机制

每 `reset_interval`（默认 100000）次操作，执行一次全局衰减：

1. **CMS 减半**：对每个字节执行 `(byte >> 1) & 0x77`，同时将两个 4-bit 计数器右移 1 位
2. **Doorkeeper 清零**：`memset(bits, 0, num_bytes)`，允许之前被过滤的 Key 重新进入 CMS

`& 0x77` 的作用：右移 1 位后，高 nibble 的最低位会溢出到低 nibble 的最高位。`0x77 = 0111_0111` 掩码清除了第 3 位和第 7 位，防止 nibble 间串扰。

## 统计信息

通过 `NUMA MIGRATE STATS` 命令查看：

| 字段 | 说明 |
|------|------|
| `tinylfu_enabled` | 策略是否启用（0/1） |
| `tinylfu_accesses` | 总访问次数 |
| `tinylfu_doorkeeper_filtered` | 被 Doorkeeper 过滤的一次性访问次数 |
| `tinylfu_candidates_enqueued` | 入队候选数 |
| `tinylfu_migrations_done` | 成功迁移数 |
| `tinylfu_migrations_failed` | 迁移失败数 |
| `tinylfu_resets` | 全局衰减次数 |

## 配置接口

通过策略插槽框架的 `set_config`/`get_config` 虚函数操作：

| 参数 | 类型 | 范围 | 默认值 | 说明 |
|------|------|------|--------|------|
| `migrate_threshold` | int | 1-15 | 3 | 触发迁移的最低频率 |
| `reset_interval` | int | ≥1000 | 100000 | 全局衰减间隔（操作次数） |
| `migration_budget` | int | ≥1 | 256 | 每次 serverCron 最多迁移数 |
| `auto_migrate_enabled` | bool | 0/1 | 1 | 自动迁移开关 |
| `debug_logging_enabled` | bool | 0/1 | 0 | 调试日志开关 |

只读查询参数：`cms_width`、`ring_size`、`total_ops`、`stat_accesses`、`stat_doorkeeper_filtered`、`stat_candidates_enqueued`、`stat_migrations_done`、`stat_migrations_failed`、`stat_resets`、`stat_accesses_local`、`stat_accesses_remote`。

## 启用方式

TinyLFU 默认禁用。启用方式：

```bash
# 通过策略插槽框架启用 Slot 2
redis-cli NUMA STRATEGY SLOT 2 tinylfu
```

也可在代码中修改 `numa_strategy_slots.c` 的 `numa_strategy_init()` 函数，删除 `numa_strategy_slot_disable(2)` 调用。

## 与其他模块的关系

```
lookupKey() [db.c]
    │
    ├── Slot 1: composite_lru_record_access()   (热度阶梯)
    │
    └── Slot 2: tinylfu_record_access()          (CMS 频率估计)
              │
              └── 入队 ring buffer ──► tinylfu_execute() [serverCron]
                                            │
                                            └── numa_migrate_key_by_name()
```

### db 绑定机制

与 Composite LRU 相同，`tinylfu_data_t->db` 由 `lookupKey()` 路径动态绑定：

```c
// db.c: lookupKey()
numa_strategy_t *tlfu = numa_strategy_slot_get(2);
if (tlfu && tlfu->enabled && tlfu->private_data) {
    tinylfu_data_t *tlfu_data = tlfu->private_data;
    tlfu_data->db = db;  // 无条件绑定
    tinylfu_record_access(tlfu, key->ptr, val, data_ptr);
}
```

### 主线程节点检测

初始化时通过 `numa_node_of_cpu(sched_getcpu())` 检测主线程所在 NUMA 节点，所有迁移目标默认为该节点（将远程数据拉回本地）。

## vtable 注册

```c
static const numa_strategy_vtable_t tinylfu_vtable = {
    .init        = tinylfu_init,
    .execute     = tinylfu_execute,
    .cleanup     = tinylfu_cleanup,
    .get_name    = tinylfu_get_name,
    .get_description = tinylfu_get_description,
    .set_config  = tinylfu_set_config,
    .get_config  = tinylfu_get_config,
};

static const numa_strategy_factory_t tinylfu_factory = {
    .name               = "tinylfu",
    .description        = "TinyLFU frequency-based hot data migration (CMS + Doorkeeper)",
    .type               = STRATEGY_TYPE_PERIODIC,
    .default_priority   = STRATEGY_PRIORITY_HIGH,
    .default_interval_us = 1000000,    // 1 秒
    .create             = tinylfu_create,
    .destroy            = tinylfu_destroy,
};
```
