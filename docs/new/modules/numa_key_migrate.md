# numa_key_migrate

> Building Block 详情表 · 对应 arc42 §5 Building Block View
> 源文件：`src/numa_key_migrate.c`（1168 行）/ `src/numa_key_migrate.h`（154 行）

## 1. 职责（Responsibility）

`numa_key_migrate` 把迁移的最小粒度从"一整块内存"提升到"一个 Redis key"：给定
一个 `robj`，把它当前占用的全部内存（包括 value 内部所有子结构）搬到指定的
NUMA 节点上，对客户端与其它模块完全透明。它是 [numa_migrate](numa_migrate.md)
（裸的 `numa_alloc_onnode` + `memcpy` 块迁移原语）与 NUMAflow（[ADR-08](../09-architecture-decisions.md)
之后，唯一的迁移策略来源，通过 Redis 桥接 `src/numa_flow.c` 驱动）之间的桥梁：
策略只负责"决定迁移哪个 key、迁到哪个节点"，具体"怎么把这个 key 安全地搬过
去"完全由本模块承担。

核心职责边界：

- 以 `robj`/key 为单位，覆盖 Redis 全部 5 种数据类型（STRING/HASH/LIST/SET/ZSET）
  在当前内核里出现的**全部**内部编码；
- 维护 key 的 NUMA 元数据（当前节点、热度、访问计数）及其在 PREFIX 与兼容字典
  两条路径间的一致性；
- 保证迁移过程中的原子性——单线程执行模型下用一次指针赋值完成"切换"；
- 提供单 key / 按 key 名 / 批量 / 通配符模式 / 全库 五种粒度的迁移入口，以及
  完整的迁移统计。

## 2. 接口（Interface）

```c
/* 初始化与清理 */
int  numa_key_migrate_init(void);
void numa_key_migrate_cleanup(void);

/* 迁移入口 */
int numa_migrate_single_key(redisDb *db, robj *key, int target_node);
int numa_migrate_key_by_name(redisDb *db, const char *keyname, int target_node);
int numa_migrate_multiple_keys(redisDb *db, list *key_list, int target_node);
int numa_migrate_keys_by_pattern(redisDb *db, const char *pattern, int target_node);
int numa_migrate_entire_database(redisDb *db, int target_node);

/* 元数据 / 采样 */
void *numa_object_sample_alloc_ptr(robj *val);
key_numa_metadata_t *numa_get_key_metadata(robj *key);
int   numa_get_key_current_node(robj *key);
void  numa_on_key_delete(robj *key);

/* 统计 */
void numa_get_migration_statistics(numa_key_migrate_stats_t *stats);
void numa_reset_migration_statistics(void);
```

错误码（`src/numa_key_migrate.h`）：

| 常量 | 值 | 含义 |
| --- | --- | --- |
| `NUMA_KEY_MIGRATE_OK` | 0 | 成功 |
| `NUMA_KEY_MIGRATE_ERR` | -1 | 一般错误 |
| `NUMA_KEY_MIGRATE_ENOENT` | -2 | key 不存在 |
| `NUMA_KEY_MIGRATE_EINVAL` | -3 | 参数无效 |
| `NUMA_KEY_MIGRATE_ENOMEM` | -4 | 内存不足 |
| `NUMA_KEY_MIGRATE_ETYPE` | -5 | 不支持的数据类型/编码 |

调用方：`numa_migrate_single_key`（按 `robj*`）供 `NUMA MIGRATE KEY` 手动命令
使用，内部走 `dictFind`；`numa_migrate_key_by_name`（按 key 名字符串）供
NUMAflow 通过 `src/numa_flow.c` 桥接触发的自动迁移使用——桥接层的 `apply()`
回调按 NUMAflow DAG 跑出来的迁移决策（`caat`/`composite_lru`/`tinylfu` 预设，
均定义在 `numaflow/src/nf_strategy.c`）调用这个入口，两条入口共享同一套类型适
配器，区别只在查找方式。

`numa_migrate_key_by_name` 内部通过静态辅助函数 `numa_key_migrate_dict_find()` 将入参 keyname 统一归一化为 SDS 后再查询 `db->dict`，能够无缝兼容标准 SDS 键和普通 C 字符串两种调用方（见文末第 7 节的设计背景）。

## 3. 内部结构与关键路径（Internal Structure & Key Paths）

### 3.1 核心数据结构

```c
/* key 的 NUMA 元数据（兼容回退路径用，主路径见下方 PREFIX 说明） */
typedef struct {
    int current_node;
    uint8_t hotness_level;      /* 0-7 */
    uint16_t last_access_time;  /* LRU 时钟 */
    size_t memory_footprint;
    uint64_t access_count;
} key_numa_metadata_t;

/* 迁移统计（NUMA MIGRATE STATS 读取的就是这份结构） */
typedef struct {
    uint64_t total_migrations;
    uint64_t successful_migrations;
    uint64_t failed_migrations;
    uint64_t total_bytes_migrated;
    uint64_t total_migration_time_us;
    uint64_t peak_concurrent_migrations;
} numa_key_migrate_stats_t;

/* 模块全局上下文 */
typedef struct {
    int initialized;
    dict *key_metadata;         /* robj* -> key_numa_metadata_t，兼容回退用 */
    pthread_mutex_t mutex;
    numa_key_migrate_stats_t stats;
} numa_key_migrate_ctx_t;
```

热度追踪的**主路径**并不经过 `key_numa_metadata_t` 字典，而是直接读写分配对象
头部内联的 16 字节 PREFIX（详见 [zmalloc_numa.md](zmalloc_numa.md)）——
`key_numa_metadata_t` 字典只在 value 指针不可用的少数场景下作兼容回退，零额外
内存、O(1) 访问的 PREFIX 才是所有正常访问的入口。

### 3.2 五种数据类型 × 全部编码的迁移适配器

这是本模块最核心的部分——**全部实现，没有占位**（`CLAUDE.md` 特别强调的一点）：

```text
┌─────────────────────────────────────────────────────────────────────────────┐
│                       Redis 5 种数据类型跨节点迁移路径                      │
├───────────┬──────────────────────────────┬──────────────────────────────────┤
│ 数据类型  │ 内部编码 (Encoding)          │ 迁移策略与内存操作               │
├───────────┼──────────────────────────────┼──────────────────────────────────┤
│ STRING    │ RAW / EMBSTR                 │ 目标节点分配新 SDS -> memcpy 数据 │
│           │ INT (整数对象)               │ 共享整数对象，零内存开销，直接跳过 │
├───────────┼──────────────────────────────┼──────────────────────────────────┤
│ HASH      │ LISTPACK / ZIPLIST (紧凑块)  │ 计算总长度 -> 目标节点分配 -> 复制 │
│           │ HT (Hashtable 散列表)        │ 遍历 dict 节点 -> 目标节点递归重建│
├───────────┼──────────────────────────────┼──────────────────────────────────┤
│ LIST      │ QUICKLIST                    │ 逐 quicklistNode 遍历：          │
│           │ (包含 LZF压缩 与 RAW 原始容器)│ 复制 quicklistLZF 或整块 entry   │
├───────────┼──────────────────────────────┼──────────────────────────────────┤
│ SET       │ INTSET (整型数组)            │ 计算 blob 长度 -> 目标节点整块复制│
│           │ HT (Hashtable 散列表)        │ 逐元素 sdsdup 重建新哈希表       │
├───────────┼──────────────────────────────┼──────────────────────────────────┤
│ ZSET      │ LISTPACK / ZIPLIST           │ 整块连续内存分配并复制            │
│           │ SKIPLIST (跳表 + Dict 双索引)│ 从尾至头遍历，新跳表插入并建索引 │
└───────────┴──────────────────────────────┴──────────────────────────────────┘

 [原子指针切换模型 (Single Thread Execution)]
 ┌─────────────────┐      1. 在目标节点分配并拷贝
 │ robj *val       │ ───────────────────────────────────► ┌────────────────────┐
 │  type=STRING    │                                      │ 新内存块 (Target)  │
 │  ptr ─────────┐ │      2. val->ptr = new_ptr (原子切换)│ (Node 1)           │
 └───────────────┼─┘ ───────────────────────────────────► └────────────────────┘
                 ▼
        ┌────────────────────┐
        │ 旧内存块 (Source)  │ ──► 3. zfree(old_ptr) 释放旧空间
        │ (Node 0)           │
        └────────────────────┘
```

| 类型 | 涉及编码 | 迁移方式 |
| --- | --- | --- |
| STRING | `OBJ_ENCODING_RAW` / `OBJ_ENCODING_EMBSTR` | 整块 SDS（含 header）`memcpy` 后原子切换 `val_obj->ptr`；整数编码（`OBJ_ENCODING_INT`）无需迁移，直接跳过 |
| HASH | `OBJ_ENCODING_LISTPACK` / `OBJ_ENCODING_ZIPLIST`（RDB 兼容保留）/ `OBJ_ENCODING_HT` | listpack 用 `lpBytes()`、ziplist 用 `ziplistBlobLen()` 取整块长度后 `memcpy`；hashtable 编码逐条 `dictNext` 遍历，在目标节点上下文里 `sdsdup` 出新 key/val 重建整个 `dict` |
| LIST | `OBJ_ENCODING_QUICKLIST`（LZF 压缩节点 / 原始节点，`quicklistNode->entry` 字段——Redis 7 把原来的 `->zl` 改名为 `->entry`） | 逐 quicklist 节点迁移：`QUICKLIST_NODE_ENCODING_LZF` 的节点复制 `quicklistLZF` 结构体 + 压缩数据；非 LZF 节点按 `node->sz` 整块复制。节点级元数据（`count`/`sz`/`encoding`/`container` 等）原样带过去 |
| SET | `OBJ_ENCODING_INTSET` / `OBJ_ENCODING_HT` | intset 用 `intsetBlobLen()` 整块 `memcpy`；hashtable 逐元素 `sdsnewlen` 重建 |
| ZSET | `OBJ_ENCODING_LISTPACK` / `OBJ_ENCODING_ZIPLIST`（RDB 兼容保留）/ `OBJ_ENCODING_SKIPLIST` | listpack/ziplist 同 HASH 的整块复制；skiplist 从 `zsl->tail` 向前遍历，逐节点 `zslInsert` 到新跳表并同步重建 `dict` |

> **一个容易搞混的点**：HASH 与 ZSET 的 listpack/ziplist 分支看起来是"新增的
> listpack 支持"，但迁移本身两条分支做的事完全一样（整块 `memcpy`），差异只在
> 用哪个函数取长度——这是 Redis 6.2.21 → 7.2.6 迁移时刻意保留的低风险路径，
> 详见 [`docs/redis7-migration.md`](../../redis7-migration.md)。

所有分支都通过 `numa_alloc_push_node(target_node)` / `numa_alloc_pop_node()`
把"在目标节点上分配"这件事变成一个作用域内的隐式上下文，而不需要每一处
`zmalloc`/`dictCreate` 调用都手工传节点号。

### 3.3 单 key 迁移主流程

```
numa_migrate_single_key(db, key, target_node)
  1. lookupKeyRead(db, key)                — 不存在 → NUMA_KEY_MIGRATE_ENOENT
  2. numa_get_key_current_node(val)         — 已在目标节点 → 直接返回 OK
  3. getObjectSize(val)                     — 用于统计 total_bytes_migrated
  4. 按 val->type 分派到对应类型适配器
  5. 适配器内部：目标节点分配 → 复制 → 原子指针切换 → 释放旧内存
  6. 更新 PREFIX / key_numa_metadata_t 的节点号
  7. stats.successful_migrations++ / total_bytes_migrated += size
```

### 3.4 无条件的热度追踪与阶梯式惰性衰减

热度追踪不是本模块自己独立跑的定时任务，而是**挂在每次 key 访问的路径上**，
由 `numa_key_migrate_touch(data_ptr, current_time)` 驱动——`src/db.c` 的
`lookupKeyReadWithFlags()` 每次真实命中都无条件调用它：

```
lookupKeyReadWithFlags() 命中
  → numa_key_migrate_touch()                无条件调用，不判断任何策略是否启用
      1. numa_get_hotness(data_ptr)              读 PREFIX 热度
      2. 按空闲时长做阶梯衰减（见下）
      3. 热度 +1，上限 7（HOTNESS_MAX_LEVEL）
      4. 写回 PREFIX（hotness / access_count / last_access）
  → numa_flow_observe_access(key->ptr)        单独喂 NUMAflow 的 CMS+Doorkeeper 频率估计器
```

**[ADR-08](../09-architecture-decisions.md) 之后的两个关键变化**：

1. `numa_key_migrate_touch()` 原来内联在已删除的 `composite_lru_record_access()`
   里，被"槽位 1/2 是否 enabled"的判断锁着；现在提取成中立函数，从 `db.c` 无条
   件调用，是 NUMAflow 的 `enumerate()`（通过桥接 `src/numa_flow.c`）读取的唯一
   热度 ground truth，不再依赖任何迁移策略是否启用。上文"5. 同步
   `key_numa_metadata_t` 兼容字典"/"6. 写入候选池"两步（原设计里紧跟在热度更新
   之后）已经随 `numa_composite_lru.c` 一起删除——候选池是该模块自己的影子状态，
   不是本模块的职责。
2. 光更新 PREFIX 的热度/访问计数还不够——`cms_estimate`（TinyLFU/CAAT 用的
   Count-Min Sketch 频率读，定义在 `numaflow/src/nf_ops.c`）读的是另一个信号
   （频率而非热度），此前整条 Redis 桥接路径都没有调用过它唯一的写入口
   `nf_tracker_observe()`，导致 `freq_est` 永远是 0（见
   [ADR-09](../09-architecture-decisions.md)）。修复方式是新增
   `numa_flow_observe_access()`（`src/numa_flow.c`/`.h`），从 `db.c` 里与
   `numa_key_migrate_touch()` 完全同一个真实访问路径调用，两者现在总是成对触发。

**阶梯式惰性衰减**（`src/numa_key_migrate.h` 常量）：

| 空闲时长（LRU 时钟秒） | 衰减量 |
| --- | --- |
| < 10s（`KEY_LAZY_DECAY_STEP1_SECS`） | 0（短暂停顿，免衰减） |
| < 60s（`STEP2`） | 1 |
| < 300s / 5min（`STEP3`） | 2 |
| < 1800s / 30min（`STEP4`） | 3 |
| ≥ 1800s | 直接清零 |

"惰性"体现在：衰减量不是靠后台定时器主动扣减，而是在**下一次访问发生时**一次
性补算——这避免了为 keyspace 里每一个 key 单独维护衰减定时器的开销。

## 4. 质量与性能特性（Quality & Performance Characteristics）

**原子性保证**：Redis 主线程单线程处理所有客户端命令，迁移操作全程在主线程
执行，期间不存在其它命令并发访问同一个 key 的可能；"切换"的关键步骤是一次
`val_obj->ptr = new_ptr` 赋值，赋值后续访问立即看到新内存，旧内存随后释放
——不需要锁、不需要双写窗口。

**时间复杂度**：

| 操作 | 复杂度 | 主要开销来源 |
| --- | --- | --- |
| 单 key 迁移 | O(数据大小) | `memcpy`（整块类型）或逐元素重建（hashtable/skiplist 类型） |
| 批量迁移 | O(n × 平均大小) | n = key 数量 |
| 模式迁移 | O(N) | N = 匹配 key 数（需要扫描 keyspace） |
| 全库迁移 | O(db_size) | 全库 key 数量 |

**统计口径**：`numa_key_migrate_stats_t` 由 `pthread_mutex_t mutex` 保护，
`NUMA MIGRATE STATS` / `NUMA MIGRATE RESET` 读写的正是这份结构；虽然当前迁移
路径本身完全在主线程串行执行，加锁是为将来可能的并发迁移路径预留的防御性设计，
不是当前的性能瓶颈来源。

## 5. 与其他模块的关系（Relations to Other Modules）

- **被 NUMAflow 桥接（`src/numa_flow.c`）调用**：`caat`/`composite_lru`/
  `tinylfu` 三个预设（`numaflow/src/nf_strategy.c`）跑出的迁移决策，经桥接层的
  `apply()` 回调，通过 `numa_migrate_key_by_name()`（按 SDS key 名）触发实际迁
  移；策略只做"决定"，本模块做"执行"。
- **被 [numa_command](numa_command.md) 调用**：`NUMA MIGRATE KEY` →
  `numa_migrate_single_key()`；`NUMA MIGRATE DB` →
  `numa_migrate_entire_database()`；`NUMA MIGRATE SCAN` 触发的是 NUMAflow
  `default` 工作流跑一次，间接落到本模块的迁移入口。
- **依赖 [zmalloc_numa](zmalloc_numa.md)**：所有目标节点上的新内存分配都经过
  `zmalloc_onnode()` / `numa_zmalloc_onnode()`，由 `numa_alloc_push_node`/
  `pop_node` 建立的节点上下文决定实际落在哪个节点。
- **与 key 删除路径的交互**：`db.c` 的 `dbDelete()` 会调用
  `numa_on_key_delete(key)`，从 `key_metadata` 字典中移除对应条目，避免内存
  泄漏——这是唯一需要 Redis 核心主动"通知"本模块的地方。

## 6. 未解决问题与已知限制（Open Issues & Known Limitations）

- `key_numa_metadata_t` 兼容字典路径与 PREFIX 主路径存在两套热度状态，仅在
  value 指针不可用的边缘场景下才会读到字典路径的值——两者理论上应保持同步，
  但没有单一测试专门覆盖"两条路径读到不一致热度"的场景。
- HASH/ZSET 的 listpack 与 ziplist 分支目前迁移逻辑完全相同（整块
  `memcpy`），如果未来两种编码的内部布局出现语义差异（而不仅仅是长度计算函数
  不同），这里需要重新审视。
- 大对象（如超大 hashtable/skiplist）的逐元素重建是同步阻塞操作，发生在
  Redis 主线程内——没有分片/让出机制。这曾是已退役的
  [AE 策略调度器](ae_strategy_scheduler.md)（[ADR-08](../09-architecture-decisions.md)
  之后随槽位框架一起失效）想解决的同一类问题的下一层："单次大 key 迁移内部可
  中断"；NUMAflow 目前的调度模型（`numa_flow_cron()` 按 `interval_sec` 判断是否
  该跑一次工作流）同样没有下探到这一层，一次 `apply()` 回调内部仍是不可中断的。

## 7. 设计考量与历史经验（Historical Context & Lessons）

- **SDS 键与普通字符串的查找兼容性（ADR-11）**：
  历史版本中，NUMAflow 桥接回调 `apply()` 传入的是 `nf_item_t.key`（普通 `char[]`），而 `db->dict` 内部由 `dictSdsHash` 处理键计算并调用 `sdslen()`。普通 C 字符串指针缺少 SDS Header，导致前置读取长度错误并引发越界和 100% 查找 miss。通过引入 `numa_key_migrate_dict_find()`，在函数内部自动规整为临时 SDS 键，使得接口能安全容纳各种格式的调用方。
- **类型适配器在 Redis 7 中的 API 兼容**：
  在 Redis 7.2.6 升级中，`quicklistNode` 将旧版的 `zl` 属性重命名为 `entry`，同时 `dictEntry` 变为 opaque 不透明结构。本模块在迁移适配器中全面遵循了新的迭代器与访问器 API，确保在无冲突标记的合并下依然保证内存安全。
