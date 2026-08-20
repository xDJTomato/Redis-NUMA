# numa_strategy_slots — 策略插槽框架

> **已退役（[ADR-08](../09-architecture-decisions.md)）**：`src/numa_strategy_slots.{c,h}`
> 已从代码库删除。三个迁移策略（`caat`/`composite_lru`/`tinylfu`）现在统一由
> NUMAflow 的原子操作框架实现（`numaflow/src/nf_strategy.c`），通过
> `NUMA FLOW` 命令管理，不再有内核原生的槽位/vtable 框架。以下内容保留作为该
> 框架曾经存在过的设计记录。

> 文件：`src/numa_strategy_slots.c` / `src/numa_strategy_slots.h`（已删除）

## 1. 职责

`numa_strategy_slots` 是一个**基于虚函数表（vtable）的插件化策略框架**：它本身不
包含任何"该迁移哪个 key"的判断逻辑，只负责三件事——

1. 提供一个统一的策略接口（vtable），让不同的迁移/热度算法可以用同一种方式被注册、
   调度、查询、配置；
2. 管理最多 **16 个插槽**（slot），每个插槽装载一个策略实例，可以独立启用/禁用；
3. 按优先级调度所有已启用插槽的执行——默认走 `serverCron` 每秒轮询，也可以让单个
   插槽改用 Redis 的 AE（`aeEventLoop`）时间事件独立调度（见第 5 节及
   [`ae_strategy_scheduler.md`](ae_strategy_scheduler.md)）。

槽位 0 固定是空操作（no-op）兜底策略；槽位 1（Composite LRU）和槽位 2（TinyLFU）
是内置的两个具体迁移策略实现，槽位 3–15 留给自定义策略。

## 2. 接口

### 2.1 策略虚函数表 `numa_strategy_vtable_t`

```c
typedef struct {
    int (*init)(numa_strategy_t *strategy);
    int (*execute)(numa_strategy_t *strategy);
    int (*execute_step)(numa_strategy_t *strategy, uint64_t deadline_us, uint32_t budget);
    void (*cleanup)(numa_strategy_t *strategy);
    const char* (*get_name)(numa_strategy_t *strategy);
    const char* (*get_description)(numa_strategy_t *strategy);
    int (*set_config)(numa_strategy_t *strategy, const char *key, const char *value);
    int (*get_config)(numa_strategy_t *strategy, const char *key, char *buf, size_t buf_len);
} numa_strategy_vtable_t;
```

一个新策略要接入这个框架，只需要实现这张表——框架不关心策略内部做什么，只关心
`init`/`execute`/`cleanup` 这几个生命周期回调，以及可选的 `execute_step`（AE 调度
模式下用来做"可中断、有预算、有截止时间"的分步执行，见第 5 节）。

### 2.2 生命周期与调度接口

```c
int  numa_strategy_init(void);                      /* 初始化管理器 + 注册内置策略 */
void numa_strategy_cleanup(void);

int  numa_strategy_register_factory(const numa_strategy_factory_t *factory);
numa_strategy_t* numa_strategy_create(const char *name);
void numa_strategy_destroy(numa_strategy_t *strategy);

int  numa_strategy_slot_insert(int slot_id, const char *strategy_name);
int  numa_strategy_slot_remove(int slot_id);
int  numa_strategy_slot_enable(int slot_id);
int  numa_strategy_slot_disable(int slot_id);
int  numa_strategy_slot_configure(int slot_id, const char *key, const char *value);

numa_strategy_t* numa_strategy_slot_get(int slot_id);
int  numa_strategy_slot_list(char *buf, size_t buf_len);
int  numa_strategy_slot_status(int slot_id, char *buf, size_t buf_len);

void numa_strategy_run_all(void);                    /* serverCron 每秒调用一次 */
int  numa_strategy_run_slot(int slot_id);

int  numa_strategy_scheduler_init(aeEventLoop *el);   /* 初始化 AE 调度器 */
int  numa_strategy_slot_schedule_ae(int slot_id);     /* 把某槽位改成 AE 调度 */
int  numa_strategy_slot_unschedule_ae(int slot_id);
void numa_strategy_scheduler_cron(void);               /* AE 调度器健康检查，serverCron 调用 */
```

### 2.3 错误码

```c
#define NUMA_STRATEGY_OK       0
#define NUMA_STRATEGY_ERR     -1
#define NUMA_STRATEGY_ENOENT  -2   /* 策略不存在 */
#define NUMA_STRATEGY_EINVAL  -3   /* 参数无效 */
#define NUMA_STRATEGY_EEXIST  -4   /* 插槽已被占用 */
```

分步执行（`execute_step`）额外定义了一组返回值：

```c
#define NUMA_STRATEGY_STEP_IDLE       0   /* 本轮无事可做 */
#define NUMA_STRATEGY_STEP_PROGRESS   1   /* 有进展，但还没做完 */
#define NUMA_STRATEGY_STEP_DONE       2   /* 本轮工作全部完成 */
#define NUMA_STRATEGY_STEP_AGAIN      3   /* 需要立即再跑一轮 */
#define NUMA_STRATEGY_STEP_ERROR     -1
#define NUMA_STRATEGY_STEP_TIMEOUT   -2   /* 超出 deadline，被中断 */
```

## 3. 内部结构与关键路径

### 3.1 全局管理器

一个进程内唯一的 `strategy_manager`（静态全局变量），持有 16 个插槽指针、工厂注
册表和全局统计：

```c
typedef struct {
    int initialized;
    numa_strategy_t *slots[NUMA_MAX_STRATEGY_SLOTS];       /* 16 个插槽 */
    pthread_mutex_t lock;
    numa_strategy_factory_t *factories[NUMA_MAX_STRATEGY_SLOTS];
    int factory_count;
    uint64_t total_runs;
    uint64_t total_strategy_executions;
} numa_strategy_manager_t;
```

### 3.2 插槽布局

```text
Slot 0:  Noop（兜底策略，默认启用）
Slot 1:  Composite LRU（默认迁移策略，启用）
Slot 2:  TinyLFU（CMS + Doorkeeper，默认禁用，需手动启用避免与 Slot 1 冲突）
Slot 3–15: 空闲，留给自定义策略
```

### 3.3 serverCron 调度路径（默认路径）

`server.c` 的 `serverCron` 每秒调用一次：

```c
numa_strategy_run_all();          /* 遍历并执行所有启用的槽位 */
numa_strategy_scheduler_cron();   /* AE 调度器的健康检查（见第 5 节） */
```

`numa_strategy_run_all()` 按优先级从高到低（HIGH → NORMAL → LOW）遍历全部 16 个
槽位：跳过空槽位、未启用的槽位、优先级不匹配的槽位、还没到执行间隔的槽位，对剩下
的槽位调用 `strategy->vtable->execute(strategy)`，并更新执行次数/耗时/失败次数等
统计。当前内置的 Composite LRU（Slot 1）与 TinyLFU（Slot 2）优先级都是 HIGH，
No-op（Slot 0）是 LOW；同优先级按槽位 ID 顺序执行。

### 3.4 内置策略注册

```c
int numa_strategy_register_noop(void);           /* Slot 0 */
int numa_strategy_register_composite_lru(void);  /* Slot 1，默认启用 */
int numa_strategy_register_tinylfu(void);        /* Slot 2，注册后立即 disable */
```

`numa_strategy_init()` 依次调用上面三个注册函数并把它们插入对应槽位；TinyLFU 注
册后会立即被 `numa_strategy_slot_disable(2)` 禁用，用户需要通过
`NUMA STRATEGY SLOT ENABLE 2` 手动打开，避免它和 Composite LRU 同时决定同一批
key 的迁移，互相打架。

## 4. 质量与性能特性

- **O(1) 空间的槽位数组**：16 个槽位是编译期常量数组，没有动态扩容，遍历成本恒定
  且极小。
- **互斥锁保护，但执行阶段不是无锁的**：所有插槽插入/移除/启用/禁用、以及执行调
  度本身都通过 `strategy_manager.lock` 保护，防止并发注册和并发执行同一策略互相
  踩踏；这意味着策略的 `execute`/`execute_step` 本身应当保持轻量，不能在持锁期间
  做长时间阻塞操作。
- **失败隔离**：单个策略执行返回非 `NUMA_STRATEGY_OK` 只会累加该策略自己的
  `total_failures`，不会影响其他槽位继续执行——一个写坏的自定义策略不会拖累整个
  框架。
- **两种调度粒度可以共存**：同一时刻，一部分槽位可以走 serverCron 的固定 1 秒节
  奏，另一部分槽位可以切到 AE 时间事件、拥有自己独立的调度周期和执行预算——这是
  为了让"重"策略不拖慢"轻"策略，详见第 5 节。

## 5. 与其他模块的关系

- **承载 [`numa_composite_lru`](numa_composite_lru.md)（Slot 1）和
  [`numa_tinylfu`](numa_tinylfu.md)（Slot 2）**——这两个模块都只需要实现
  `numa_strategy_vtable_t`，具体的"迁移哪个 key"的算法完全在各自模块内部，本框架
  对此一无所知。
- **被 [`numa_command`](numa_command.md) 消费**：`NUMA STRATEGY LIST` 调
  `numa_strategy_slot_list()`；`NUMA STRATEGY SLOT <id> <name>` 调
  `numa_strategy_slot_insert()`；`NUMA STRATEGY SLOT SCHEDULE <id> ae|servercron`
  调 `numa_strategy_slot_schedule_ae()`/`numa_strategy_slot_unschedule_ae()`。
- **AE 时间事件调度**：除了默认的 serverCron 轮询，每个槽位还可以单独切换成基于
  Redis 事件循环（`aeEventLoop`）的时间事件调度模式（`scheduler_mode ==
  NUMA_STRATEGY_SCHED_AE`），拥有自己的执行预算（`step_budget`）、单步最大运行时
  间（`max_runtime_us_per_step`）和超时计数（`timeout_count`），通过
  `execute_step()` 分步执行而不是一次性跑完。这是为了避免"慢策略拖长 serverCron、
  所有策略共享同一个调度周期"的问题。完整的设计动机、调度模型和实现现状见
  [`ae_strategy_scheduler.md`](ae_strategy_scheduler.md)，本文不重复展开。

## 6. 未解决问题与已知限制

- **持锁执行**：`numa_strategy_run_all()` 在持有全局锁期间依次调用每个策略的
  `execute()`；如果某个策略的 `execute()` 意外阻塞（例如做了同步 I/O），会连带
  拖住其它所有走 serverCron 路径的槽位。AE 调度模式部分缓解了这个问题，但只对切
  换到 AE 模式的槽位生效。
- **自定义策略的插槽号是手动指定的**：`numa_strategy_slot_insert(3, ...)` 这类调
  用需要开发者自己保证不和已占用的槽位冲突（0–2 已被内置策略占用），框架本身在
  `NUMA_STRATEGY_EEXIST` 之外没有更智能的"自动分配空闲槽位"机制。
- **两套调度路径并存增加了心智负担**：同一个框架里，槽位可能处于 serverCron 模
  式或 AE 模式，两种模式下策略需要实现的回调不完全一样（`execute` vs
  `execute_step`），为框架增加了一层需要读者同时理解两套调度语义的复杂度。
