# Building Block: numa_tinylfu

> **已退役（[ADR-08](../09-architecture-decisions.md)）**：`src/numa_tinylfu.{c,h}`
> 已从代码库删除——它在实践中从未被真正启用过（没有 config/redis.conf 入口，
> 唯一路径是运维手敲已被移除的 `NUMA STRATEGY ENABLE 2`）。TinyLFU 的算法
> （Count-Min Sketch + Doorkeeper）现在只作为 NUMAflow 的原子操作预设存在
> （`numaflow/src/nf_strategy.c` 的 `build_tinylfu`），通过
> `NUMA FLOW DEFAULT tinylfu` 或 `NUMA FLOW LOAD` 使用。以下内容保留作为该
> 模块曾经存在过的设计记录。

`src/numa_tinylfu.c` / `src/numa_tinylfu.h`（已删除） — 频率驱动的热点数据迁移策略，
曾注册在策略插槽 **Slot 2**，默认禁用。

## 1. 职责 (Responsibility)

用固定大小、与 keyspace 无关的内存，尽可能便宜地回答一个问题：「这个 key 历史上
被访问得够不够频繁，值得把它从远端节点搬到本地？」它是 Slot 1 Composite LRU
（[modules/numa_composite_lru.md](numa_composite_lru.md)）之外的第二种迁移决策
来源——Composite LRU 看的是「最近有没有访问」（热度阶梯 + 惰性衰减），TinyLFU 看
的是「历史上访问了多少次」（Count-Min Sketch 频率估计），二者提供不同的语义，因此
框架层把它们做成互斥的策略槽位，而不是合并成一个策略。

算法参考 Caffeine（Ben Manes）的 Window-TinyLFU 设计的简化版：Count-Min Sketch
（CMS）+ Doorkeeper 布隆过滤器，固定约 40KB 内存（CMS 32KB + Doorkeeper 8KB +
候选环形缓冲区约 16KB），O(1) 完成一次频率估计与更新。

## 2. 接口 (Interface)

通过标准的 `numa_strategy_vtable_t`（[modules/numa_strategy_slots.md](numa_strategy_slots.md)
定义的插槽框架接口）暴露给上层：

```c
static const numa_strategy_vtable_t tinylfu_vtable = {
    .init             = tinylfu_init,
    .execute          = tinylfu_execute,       /* serverCron 路径 */
    .execute_step     = tinylfu_execute_step,  /* AE 调度路径，见 §3.3 */
    .cleanup          = tinylfu_cleanup,
    .get_name         = tinylfu_get_name,
    .get_description  = tinylfu_get_description,
    .set_config       = tinylfu_set_config,
    .get_config       = tinylfu_get_config,
};
```

热路径钩子（不经过 vtable，由 `lookupKey()` 直接调用，见 §5）：

```c
void tinylfu_record_access(numa_strategy_t *strategy, void *key_sds,
                            void *val, void *data_ptr, ...);
```

配置通过 `set_config`/`get_config` 读写，可写参数与只读统计字段见下表：

| 参数 | 类型 | 范围 | 默认值 | 说明 |
|---|---|---|---|---|
| `migrate_threshold` | int | 1-15 | 3 | 触发迁移的最低估计频率 |
| `reset_interval` | int | ≥1000 | 100000 | 全局衰减间隔（操作次数） |
| `migration_budget` | int | ≥1 | 256 | 每次执行最多处理的候选数 |
| `auto_migrate_enabled` | bool | 0/1 | 1 | 自动迁移开关 |
| `debug_logging_enabled` | bool | 0/1 | 0 | 调试日志开关 |

只读统计：`cms_width`、`ring_size`、`total_ops`、`stat_accesses`、
`stat_doorkeeper_filtered`、`stat_candidates_enqueued`、`stat_migrations_done`、
`stat_migrations_failed`、`stat_resets`、`stat_accesses_local`、
`stat_accesses_remote`——都可通过 `NUMA MIGRATE STATS` 查看。

启用方式（默认禁用，需与 Composite LRU 手动切换）：

```bash
redis-cli NUMA STRATEGY SLOT 2 tinylfu
redis-cli NUMA STRATEGY SLOT ENABLE 2
```

## 3. 内部结构与关键路径 (Internal Structure & Key Paths)

### 3.1 两级过滤：Doorkeeper → Count-Min Sketch

```
访问 Key
    │
    ▼
Doorkeeper (65536 位布隆过滤器, 2 个哈希)
    │
    ├── 第一次见 ──► 仅记录到 Bloom，不碰 CMS，计入 stat_doorkeeper_filtered
    │
    └── 已见过 ──► CMS 对应 4 行各自递增 ──► freq = min(4 行) + 1
                        │
                        ├── freq ≥ migrate_threshold 且数据在远端节点
                        │       └── 推入候选环形缓冲区
                        │
                        └── total_ops ≥ reset_interval
                                └── CMS 全局减半 + Doorkeeper 清零
```

真实 Redis 负载中大量 key 只被访问一次（扫描、临时 key）。若直接递增 CMS，会造
成「计数器污染」——稀有 key 占用本应属于热点 key 的计数器位。Doorkeeper 作为第
一道防线，让 CMS 的有限计数器空间只分配给**至少被访问两次**的 key。

CMS 用 4-bit packed 编码（每字节存 2 个计数器，上限 15），4 行 × 16384 列，行列
索引从 64-bit SipHash 按 16-bit 段切出（Caffeine 风格）。全局减半用
`(byte >> 1) & 0x77` 同时右移两个 nibble，掩码防止 nibble 间串扰。

### 3.2 `tinylfu_record_access()` — 热路径

每次 `lookupKey()` 命中时调用（在 Slot 1 的 hook 之后）。开销固定为 1 次
SipHash + 4 次位操作（Doorkeeper）+ 4 次数组读取（CMS）——**无字典查找、无内存
分配**，这是它能挂在访问热路径上的前提。命中迁移条件（`freq >= migrate_threshold`
且数据当前不在主线程所在节点）时，把 key 的 SDS 副本、value 指针、目标节点一起
推入候选环形缓冲区，不在热路径里做迁移本身。

### 3.3 `tinylfu_execute()` / `tinylfu_execute_step()` — 执行路径

`tinylfu_execute()` 是 `serverCron` 每秒调用一次的旧路径；`tinylfu_execute_step()`
是可挂到 AE time event 上的新路径（见
[modules/ae_strategy_scheduler.md](ae_strategy_scheduler.md)），接受
`deadline_us`/`budget` 两个参数，语义上更严格：

- 每处理一个候选先检查 `deadline_us`，超时立即返回 `NUMA_STRATEGY_STEP_TIMEOUT`，
  不会让一次迁移批处理无限占用事件循环；
- 每个候选带 `cost_units`（默认 1），累计超过 `budget` 时把当前候选**放回环形
  缓冲区尾部**而不是丢弃，下一次调度继续处理——保证候选不会因为预算不够而丢失；
- 处理前会用当前的 CMS/Doorkeeper 状态**重新估计一次频率**，确认候选入队之后
  没有变冷，避免迁移一个已经不再热的 key；
- 返回值区分 `IDLE`（环形缓冲区空）/`AGAIN`（预算用完但还有候选）/`PROGRESS`
  （处理完且缓冲区已空）/`TIMEOUT`（撞到 deadline），供上层调度器判断下一次何
  时再调度这个槽位。

这个 `execute_step` 签名和状态机（deadline + budget + 可续跑 + 显式返回码）是
[modules/ae_strategy_scheduler.md](ae_strategy_scheduler.md) 里定义的策略接口
契约的具体实现之一——TinyLFU 和 Composite LRU 是目前唯一两个已经适配到这套契约
的策略。

### 3.4 全局衰减

每 `reset_interval`（默认 100000）次访问触发一次：CMS 全部字节减半（保留相对频
率关系，而不是清零重来），Doorkeeper 位数组整体清零（允许之前被过滤掉的 key 重
新有机会进入 CMS）。这让 TinyLFU 对访问模式的变化有遗忘能力，而不是无限累积历
史频率。

## 4. 质量与性能特性 (Quality & Performance Characteristics)

- **固定内存，O(1) 发现**：CMS 32KB + Doorkeeper 8KB + 环形缓冲区（默认 512 条
  目，约 16KB）——总内存约 56KB，与 keyspace 大小完全无关，这是它相对 Composite
  LRU（`key_heat_map` 字典随 key 数线性增长）的核心权衡优势。
- **有损估计**：CMS 是概率数据结构，存在哈希碰撞导致的频率高估，且全局减半意
  味着无法区分「长期稳定的中等热度」与「短期爆发后已经冷却」——这是用固定内存
  换来的代价，不是 bug。
- **主线程节点检测**：初始化时通过 `numa_node_of_cpu(sched_getcpu())` 确定主线
  程所在 NUMA 节点，所有迁移目标默认为该节点（把远端热数据拉回本地）；这意味着
  TinyLFU 目前不支持"迁到除主线程节点外的任意节点"这种更复杂的多节点热度分布
  场景。

## 5. 与其他模块的关系 (Relations to Other Modules)

```
lookupKey() [db.c]
    │
    ├── Slot 1: composite_lru_record_access()   (热度阶梯，见 numa_composite_lru.md)
    │
    └── Slot 2: tinylfu_record_access()          (CMS 频率估计，本模块)
              │
              └── 候选入队 ring buffer ──► tinylfu_execute[_step]()
                                                │
                                                └── numa_migrate_key_by_name()
                                                    （见 numa_key_migrate.md）
```

`tinylfu_data_t->db` 与 Composite LRU 采用相同的动态绑定机制：`lookupKey()` 每次
调用时把当前 `db` 无条件写入策略私有数据，而不是在初始化时固定绑定——这是因为
NUMA 模块初始化发生在数据库对象创建之前（见
[08-crosscutting-concepts.md](../08-crosscutting-concepts.md) 的初始化顺序约
定），策略层没有办法在 `init()` 时就拿到 `redisDb*`。

**与 Slot 1 的互斥关系**：框架层允许同时启用两个槽位，但语义上二者会对同一批
key 给出可能冲突的迁移决策（一个基于新近度，一个基于频率），且都会往
`numa_migrate_key_by_name()` 提交迁移请求，造成不必要的迁移抖动。默认只启用
Slot 1，Slot 2 注册后立即被 `numa_strategy_slot_disable(2)` 禁用（`src/numa_strategy_slots.c`），
需要运维显式做出「换成频率驱动」的决定，而不是两者自动叠加。

## 6. 未解决问题与已知限制 (Open Issues & Known Limitations)

- 与 Composite LRU 互斥但框架层没有强制校验——同时手动启用两个槽位不会报错，
  但会产生上面提到的迁移决策冲突，目前依赖运维自律而非代码保证。
- 只支持迁回「主线程所在节点」这一个目标，无法表达更细粒度的多节点频率分布
  策略（比如"这个 key 该去 CXL 节点 A 还是 B"）。
- CMS 减半是全局操作，无法对"仍然很热但恰好卡在衰减周期边界"的 key 做特殊处
  理，衰减粒度受 `reset_interval` 单一参数控制，没有类似 Composite LRU 惰性衰减
  那种按 key 空闲时间精细调整的机制。
