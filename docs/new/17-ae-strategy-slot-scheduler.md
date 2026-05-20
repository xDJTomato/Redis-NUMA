# Redis-NUMA 策略 Slot 接入 AE 异步事件循环方案

## 1. 背景问题

当前 Redis-NUMA 的策略框架通过 `serverCron` 周期性执行策略 slot。现阶段 slot 数量较少，Composite LRU 与 TinyLFU 的执行逻辑也相对可控，因此阻塞问题并不明显。

但未来如果用户继续扩展策略插槽，例如：

- 大范围 key scan；
- 多阶段冷热判定；
- 复杂统计计算；
- 带采样窗口的迁移策略；
- 混合 CXL / NUMA / bandwidth pressure 策略；
- 外部 telemetry 驱动的策略；

如果所有 slot 仍然集中挂在 `serverCron` 中串行执行，就会引入明显风险。

### 1.1 主要风险

#### serverCron 被慢策略拖长

`serverCron` 负责 Redis 的多类核心维护任务，包括过期、超时、统计更新、后台任务调度等。如果某个 NUMA slot 执行时间过长，会拖慢整个 `serverCron` 周期，间接影响 Redis 服务稳定性。

#### 所有策略共享同一个调度周期

当前 slot 虽然有 `execute_interval_us`，但如果仍依赖 `serverCron` 轮询，调度粒度会受到 Redis `hz` 与 `serverCron` 周期限制。不同策略无法自然拥有独立调度节奏。

#### 扩展者容易写出阻塞型策略

如果策略接口只是简单的：

```c
int (*execute)(numa_strategy_t *strategy);
```

扩展者很容易在一次 `execute()` 中完成大量扫描、统计或迁移，从而在 Redis 主线程中制造 tail latency 抖动。

#### 缺少统一 backoff / budget / deadline 机制

当前框架缺少统一的策略执行限时、预算控制、错误隔离与降频机制。某个 slot 如果连续超时，目前没有统一的调度降频或自动熔断逻辑。

---

## 2. 核心结论

可以将每个策略 slot 注册为 Redis AE time event，并纳入 Redis 原生事件循环模型。

但推荐设计不是简单地把：

```text
serverCron -> numa_strategy_slots_execute()
```

替换为：

```text
aeCreateTimeEvent(...) -> slot_execute()
```

而是引入一个完整的 **NUMA Strategy AE Scheduler**。

核心原则是：

> 每个 slot 拥有独立 AE time event；每次执行必须是小步、限时、可续跑的 incremental step；长耗时迁移应进一步进入统一 migration queue，AE 主线程只做轻量决策和任务提交。

---

## 3. Redis AE 事件模型适配方式

Redis AE 主要有两类事件：

1. **file event**
   - socket 可读 / 可写；
   - 客户端请求处理；
   - replication、cluster bus 等。

2. **time event**
   - 周期任务；
   - `serverCron` 本身也是 time event；
   - 适合用于 NUMA 策略 slot 调度。

NUMA 策略 slot 更适合挂在 `aeCreateTimeEvent()` 上，而不是 file event。

目标模型：

```text
Redis main loop
  ├── file events
  │     ├── client read
  │     ├── command execution
  │     └── client write
  │
  ├── time events
  │     ├── serverCron
  │     ├── NUMA slot 1: Composite LRU
  │     ├── NUMA slot 2: TinyLFU
  │     ├── NUMA slot 3: future policy
  │     └── NUMA slot N: future policy
  │
  └── beforeSleep / afterSleep hooks
```

---

## 4. 当前模型与目标模型

### 4.1 当前模型

```text
serverCron
  └── numa_strategy_slots_execute_all()
        ├── slot 1 execute()
        ├── slot 2 execute()
        ├── slot 3 execute()
        └── ...
```

问题是一个 slot 执行过慢，会同时拖住所有 slot 与 `serverCron`。

### 4.2 目标模型

```text
initServer / NUMA init
  └── numa_strategy_scheduler_init(server.el)

slot enable
  └── aeCreateTimeEvent(slot->interval_ms, numaStrategySlotTimeProc, slot)

AE event loop
  ├── slot 1 time event
  │     └── execute_step(deadline, budget)
  ├── slot 2 time event
  │     └── execute_step(deadline, budget)
  └── slot 3 time event
        └── execute_step(deadline, budget)
```

`serverCron` 只保留：

- 策略调度器健康检查；
- 统计输出；
- slot event 丢失后的重注册；
- 兼容旧策略的 fallback；
- 全局 enable / disable 控制。

---

## 5. 策略接口改造

当前 vtable 大致类似：

```c
typedef struct numa_strategy_vtable {
    int  (*init)(numa_strategy_t *strategy);
    int  (*execute)(numa_strategy_t *strategy);
    void (*cleanup)(numa_strategy_t *strategy);
} numa_strategy_vtable_t;
```

建议扩展为：

```c
typedef struct numa_strategy_vtable {
    int  (*init)(numa_strategy_t *strategy);

    int  (*execute)(numa_strategy_t *strategy);

    int  (*execute_step)(numa_strategy_t *strategy,
                         uint64_t deadline_us,
                         uint32_t budget);

    void (*on_enable)(numa_strategy_t *strategy);
    void (*on_disable)(numa_strategy_t *strategy);

    void (*cleanup)(numa_strategy_t *strategy);

    const char *(*get_name)(numa_strategy_t *strategy);
    const char *(*get_description)(numa_strategy_t *strategy);
    int  (*set_config)(numa_strategy_t *strategy, const char *key, const char *value);
    int  (*get_config)(numa_strategy_t *strategy, const char *key, char *buf, size_t len);
} numa_strategy_vtable_t;
```

兼容规则：

- 如果策略实现了 `execute_step()`，AE scheduler 调用 `execute_step()`；
- 如果没有实现，则调用旧的 `execute()`；
- 后续新增策略应优先实现 `execute_step()`。

---

## 6. execute_step 语义

`execute_step()` 必须满足以下约束。

### 6.1 非阻塞

不允许：

- 完整扫描大字典；
- 一次性迁移大量对象；
- 长时间持锁；
- sleep；
- blocking I/O。

### 6.2 有时间 deadline

调度器传入 `deadline_us`，策略内部每处理一批 item 后检查当前时间。

### 6.3 有 budget

例如：

- 本轮最多处理 64 个候选；
- 本轮最多迁移 8 个 key；
- 本轮最多扫描 1024 个 dict bucket。

### 6.4 可续跑

策略必须保存 cursor / ring index / scan state，下次 time event 从上次位置继续。

### 6.5 返回值定义

建议定义：

```c
#define NUMA_STRATEGY_STEP_IDLE       0
#define NUMA_STRATEGY_STEP_PROGRESS   1
#define NUMA_STRATEGY_STEP_DONE       2
#define NUMA_STRATEGY_STEP_AGAIN      3
#define NUMA_STRATEGY_STEP_ERROR     -1
#define NUMA_STRATEGY_STEP_TIMEOUT   -2
```

含义：

| 返回值 | 含义 |
|---|---|
| `IDLE` | 本轮无事可做 |
| `PROGRESS` | 本轮处理了一些工作 |
| `DONE` | 当前积压已清空 |
| `AGAIN` | 还有 backlog，希望尽快再次调度 |
| `ERROR` | 策略内部错误 |
| `TIMEOUT` | 触及 deadline，主动让出 |

---

## 7. slot 结构扩展

建议在 `numa_strategy_t` 或 slot wrapper 中新增调度字段：

```c
typedef struct numa_strategy_t {
    const numa_strategy_vtable_t *vtable;

    int enabled;
    int type;
    int priority;

    const char *name;
    const char *description;

    uint64_t execute_interval_us;

    void *private_data;

    long long ae_time_event_id;

    uint64_t last_run_us;
    uint64_t next_run_us;

    uint64_t total_runs;
    uint64_t total_runtime_us;
    uint64_t max_runtime_us;
    uint64_t timeout_count;
    uint64_t error_count;

    uint32_t step_budget;
    uint32_t max_runtime_us_per_step;

    int scheduler_state;
    int backoff_level;
} numa_strategy_t;
```

字段说明：

| 字段 | 作用 |
|---|---|
| `ae_time_event_id` | 当前注册到 AE 的 time event id |
| `step_budget` | 每轮最多处理多少工作 |
| `max_runtime_us_per_step` | 每轮最大主线程占用时间 |
| `backoff_level` | 连续空转或失败时的降频等级 |
| `scheduler_state` | active / paused / disabled / failed / backoff |

---

## 8. AE time event 调度逻辑

### 8.1 注册

slot enable 时注册 AE time event：

```c
int numa_strategy_slot_schedule(int slot_id) {
    numa_strategy_t *s = numa_strategy_slot_get(slot_id);
    if (!s || !s->enabled) return C_ERR;

    s->ae_time_event_id = aeCreateTimeEvent(
        server.el,
        s->execute_interval_us / 1000,
        numaStrategySlotTimeProc,
        (void*)(intptr_t)slot_id,
        NULL
    );

    return s->ae_time_event_id == AE_ERR ? C_ERR : C_OK;
}
```

### 8.2 执行

```c
int numaStrategySlotTimeProc(struct aeEventLoop *eventLoop,
                             long long id,
                             void *clientData) {
    int slot_id = (int)(intptr_t)clientData;
    numa_strategy_t *s = numa_strategy_slot_get(slot_id);

    if (!s || !s->enabled) {
        return AE_NOMORE;
    }

    uint64_t start = ustime();
    uint64_t deadline = start + s->max_runtime_us_per_step;

    int ret;
    if (s->vtable->execute_step) {
        ret = s->vtable->execute_step(s, deadline, s->step_budget);
    } else if (s->vtable->execute) {
        ret = s->vtable->execute(s);
    } else {
        ret = NUMA_STRATEGY_STEP_IDLE;
    }

    uint64_t runtime = ustime() - start;

    s->last_run_us = start;
    s->total_runs++;
    s->total_runtime_us += runtime;
    if (runtime > s->max_runtime_us)
        s->max_runtime_us = runtime;

    if (runtime >= s->max_runtime_us_per_step)
        s->timeout_count++;

    return numa_strategy_slot_next_delay_ms(s, ret, runtime);
}
```

### 8.3 下一次调度时间

调度间隔不应固定死，而应根据状态动态调整。

```text
有 backlog 且本轮没超时：
  next = min_interval，例如 1ms / 5ms

正常完成：
  next = execute_interval_us

空转多次：
  next = interval * backoff_factor

错误：
  next = interval * error_backoff

slot disabled：
  AE_NOMORE
```

示例：

```c
static int numa_strategy_slot_next_delay_ms(numa_strategy_t *s,
                                            int ret,
                                            uint64_t runtime_us) {
    if (!s->enabled)
        return AE_NOMORE;

    if (ret == NUMA_STRATEGY_STEP_AGAIN)
        return 1;

    if (ret == NUMA_STRATEGY_STEP_TIMEOUT)
        return 1;

    if (ret == NUMA_STRATEGY_STEP_ERROR)
        return min(60000, (s->execute_interval_us / 1000) << s->backoff_level);

    if (ret == NUMA_STRATEGY_STEP_IDLE)
        return s->execute_interval_us / 1000 * 2;

    return s->execute_interval_us / 1000;
}
```

---

## 9. Composite LRU 适配方式

Composite LRU 当前有两个主要工作：

1. 热路径 access record；
2. 周期执行 candidate ring 消费、迁移与 progressive scan。

热路径仍保留在 `lookupKey()` 中：

```text
lookupKey()
  └── composite_lru_record_access()
        └── ring push only
```

周期执行改为 AE step：

```text
AE slot event
  └── composite_lru_execute_step()
        ├── process up to N candidates
        ├── migrate up to M keys
        ├── scan up to K dict buckets
        └── save cursor
```

关键改造：

- `composite_lru_execute()` 保留为 wrapper；
- 新增 `composite_lru_execute_step()`；
- ring 消费不再一次清空；
- progressive scan 保存 cursor；
- 每迁移若干 key 检查 `deadline_us`。

---

## 10. TinyLFU 适配方式

TinyLFU 更适合 AE slot，因为它已经有 candidate ring 与 migration budget。

当前逻辑：

```text
tinylfu_record_access()
  ├── doorkeeper
  ├── cms record
  └── ring_push candidate

serverCron
  └── tinylfu_execute()
        └── process entire ring until budget
```

目标逻辑：

```text
tinylfu_record_access()
  ├── doorkeeper
  ├── cms record
  └── ring_push candidate

AE slot event
  └── tinylfu_execute_step(deadline, budget)
        ├── process partial ring
        ├── migrate limited keys
        ├── keep ring cursor
        └── return AGAIN if backlog remains
```

需要新增状态：

```c
uint32_t execute_cursor;
uint32_t pending_count;
```

或将 ring 从“每轮清空”改成“逐项消费”。当前 `tinylfu_execute()` 末尾会清空 ring：

```c
d->ring_count = 0;
d->ring_head = 0;
```

未来不应一次性清空，而应该：

- 处理一个 candidate 后释放该 slot；
- 更新 cursor；
- 只在 ring 为空时 reset；
- ring 满时覆盖旧 candidate 仍可接受。

---

## 11. 迁移操作是否也放进 AE

AE time event 仍运行在 Redis 主线程中，所以它只能解决：

- 调度分散；
- 小步执行；
- 减少 `serverCron` 抖动；
- 避免多个 slot 挤在同一个 cron 周期。

它不能自动解决：

- 大对象 `memcpy` 阻塞；
- `numa_alloc_onnode()` 慢路径阻塞；
- 批量迁移造成主线程停顿。

因此推荐分两层：

```text
AE time event
  ├── 轻量判定
  ├── 生成 migration task
  └── 提交到 migration queue

background migration worker
  ├── 分配目标节点内存
  ├── memcpy
  └── 返回结果

main thread apply
  ├── 安全替换对象指针
  ├── 更新 metadata
  └── 释放旧对象
```

不过 Redis 对象迁移涉及对象生命周期、dict entry、引用计数、并发安全。后台线程直接操作 Redis 对象风险较高，因此应分阶段推进。

---

## 12. 分阶段路线

### Phase A：AE time event 调度 slot，但迁移仍在主线程

目标：

- 将 slot 从 `serverCron` 中拆出来；
- 每个 slot 独立调度；
- 每次执行严格限时限量；
- 不改变对象迁移线程模型。

优点：

- 改动小；
- 风险低；
- 易验证；
- 不破坏 Redis 单线程对象模型。

实现内容：

1. `numa_strategy_t` 增加 AE 调度字段；
2. slot enable 时注册 AE time event；
3. slot disable 时删除 AE time event；
4. 新增 `execute_step()` vtable；
5. Composite LRU / TinyLFU 适配 step；
6. `serverCron` 不再直接执行所有 slot，只做 fallback / health check；
7. `NUMA STRATEGY STATS` 输出 runtime / timeout / backoff。

这是最推荐的第一阶段。

### Phase B：主线程 migration queue，小步迁移

目标：

- 即使仍在主线程迁移，也通过 queue + budget 控制每次迁移数量；
- 策略只负责 enqueue；
- 统一 migration executor 消费。

结构：

```text
slot execute_step()
  └── numa_migration_enqueue(key, target_node)

AE migration executor
  └── process up to M migration tasks
```

优点：

- 策略逻辑和迁移执行解耦；
- 多策略共享统一 migration budget；
- 避免每个策略各自迁移导致总迁移量失控。

建议新增全局配置：

```text
numa-migration-max-per-event
numa-migration-max-runtime-us
numa-migration-global-budget-per-sec
```

### Phase C：后台线程预拷贝，大对象异步迁移

目标：

- 对大对象或高成本迁移使用后台线程；
- 主线程只做最后的安全提交。

这是高风险阶段，因为 Redis 对象不是天然线程安全的。

可选安全模型：

1. 主线程生成 immutable migration task；
2. 后台线程只处理裸内存块，不碰 Redis dict；
3. 后台线程完成后把新内存返回 completion queue；
4. 主线程在 AE event 中检查对象是否仍有效；
5. 主线程执行 pointer swap / robj replace；
6. 主线程释放旧内存。

需要解决：

- key 是否被删除；
- value 是否被改写；
- 对象 encoding 是否变化；
- refcount 是否变化；
- 迁移期间是否被再次迁移；
- 如何验证对象版本。

可增加对象或 key 级 generation/version：

```c
uint64_t numa_object_version;
```

或 migration task 捕获：

```c
dictEntry *de;
robj *expected_val;
void *expected_ptr;
uint64_t expected_version;
```

提交时验证：

```c
if (dictGetVal(de) != expected_val) abort_migration;
if (current_ptr != expected_ptr) abort_migration;
if (version != expected_version) abort_migration;
```

Phase C 可作为未来优化，不建议第一步直接做。

---

## 13. serverCron 与 AE slot 的职责边界

### 13.1 serverCron 保留职责

`serverCron` 不再执行策略主体，但保留：

1. 全局 NUMA health check；
2. scheduler watchdog；
3. slot event 丢失重注册；
4. 统计采样；
5. 全局 bandwidth monitor tick；
6. 配置热更新；
7. 兼容模式 fallback；
8. 输出 warning。

例如：

```c
void serverCron(...) {
    ...
#ifdef HAVE_NUMA
    numa_strategy_scheduler_cron();
#endif
    ...
}
```

`numa_strategy_scheduler_cron()` 只做轻量检查：

```text
for each slot:
  if enabled && ae_time_event_id == AE_DELETED_EVENT_ID:
      reschedule
  if last_run too old:
      warning / reschedule
  if too many errors:
      disable or backoff
```

---

## 14. 新增配置建议

### 14.1 全局配置

```text
NUMA STRATEGY SCHEDULER SET mode ae|servercron|off
NUMA STRATEGY SCHEDULER SET max_runtime_us 500
NUMA STRATEGY SCHEDULER SET default_budget 64
NUMA STRATEGY SCHEDULER SET backoff_enabled 1
```

### 14.2 per-slot 配置

```text
NUMA STRATEGY SLOT CONFIG 2 interval_us 1000000
NUMA STRATEGY SLOT CONFIG 2 max_runtime_us 500
NUMA STRATEGY SLOT CONFIG 2 step_budget 128
NUMA STRATEGY SLOT CONFIG 2 backoff_enabled 1
NUMA STRATEGY SLOT CONFIG 2 scheduler ae
```

### 14.3 stats 输出

```text
NUMA STRATEGY SLOT STATS 2
```

示例输出：

```text
slot_id
2
name
tinylfu
enabled
1
scheduler
ae
ae_time_event_id
123
execute_interval_us
1000000
step_budget
128
max_runtime_us_per_step
500
total_runs
1024
total_runtime_us
184500
avg_runtime_us
180
max_runtime_us
820
timeout_count
3
error_count
0
backoff_level
0
last_run_us
...
next_delay_ms
...
```

---

## 15. 公平性设计

多个 slot 都注册 AE time event 后，仍可能在同一时间点触发。

需要避免：

```text
tick T:
  slot 1 runs 500us
  slot 2 runs 500us
  slot 3 runs 500us
  slot 4 runs 500us
=> main loop 被 NUMA 策略连续占用 2ms
```

### 15.1 slot 初始注册加 jitter

启用 slot 时加微小错峰：

```text
slot 1: interval 1000ms + 0ms
slot 2: interval 1000ms + 7ms
slot 3: interval 1000ms + 13ms
slot 4: interval 1000ms + 19ms
```

### 15.2 全局每轮预算

维护全局 scheduler runtime budget：

```c
server.numa_strategy_runtime_budget_us_per_loop = 1000;
```

如果本轮 AE 已经执行过多 NUMA 策略，则后续 slot 返回短延迟重试。

### 15.3 priority

已有 `priority` 字段可以用于决定：

- 高优先级 slot 更短 backoff；
- 低优先级 slot 空转后更快降频；
- 全局 budget 紧张时先执行高优先级。

---

## 16. 失败隔离机制

每个 slot 必须独立失败隔离，不能因为一个用户扩展策略异常影响整个 NUMA 框架。

建议：

1. 连续错误计数；
2. 超时计数；
3. 自动 backoff；
4. 达到阈值自动 disable；
5. `NUMA STRATEGY SLOT ENABLE` 可手动恢复。

示例策略：

```text
error_count 连续 >= 3:
  backoff_level++

error_count 连续 >= 10:
  disable slot

timeout_count / total_runs > 5%:
  warning

max_runtime_us > 10 * budget:
  warning
```

---

## 17. 与 Redis 单线程模型的关系

需要明确一点：

> 接入 AE time event 不等于真正异步并行。

它的意义是：

- 从 `serverCron` 拆出来；
- 独立调度；
- 限时执行；
- 分散工作；
- 减少单次 cron 峰值；
- 改善 tail latency。

它不能自动让策略并行执行。

如果策略本身执行 10ms 的同步 memcpy，即使放进 AE time event，也仍然会阻塞 Redis 主线程 10ms。

所以必须配合：

- incremental step；
- migration budget；
- deadline；
- cursor；
- backoff；
- 后续可选后台迁移。

---

## 18. 对当前代码的具体落点

### 18.1 `src/numa_strategy_slots.h`

新增 scheduler mode：

```c
#define NUMA_STRATEGY_SCHED_SERVERCRON 0
#define NUMA_STRATEGY_SCHED_AE         1
```

新增调度统计结构：

```c
typedef struct numa_strategy_scheduler_stats {
    long long ae_time_event_id;
    uint64_t total_runs;
    uint64_t total_runtime_us;
    uint64_t max_runtime_us;
    uint64_t timeout_count;
    uint64_t error_count;
    uint64_t last_run_us;
    uint32_t step_budget;
    uint32_t max_runtime_us;
    int backoff_level;
    int scheduler_mode;
} numa_strategy_scheduler_stats_t;
```

并扩展 `numa_strategy_t`。

### 18.2 `src/numa_strategy_slots.c`

新增：

```c
int numa_strategy_slot_schedule_ae(int slot_id);
int numa_strategy_slot_unschedule_ae(int slot_id);
int numa_strategy_slot_reschedule_ae(int slot_id);
int numa_strategy_scheduler_init(aeEventLoop *el);
void numa_strategy_scheduler_cron(void);
```

新增 AE callback：

```c
static int numaStrategySlotTimeProc(aeEventLoop *eventLoop,
                                    long long id,
                                    void *clientData);
```

### 18.3 `src/server.c`

初始化 NUMA 策略后：

```c
numa_strategy_scheduler_init(server.el);
```

`serverCron` 中：

```c
numa_strategy_scheduler_cron();
```

逐步移除或默认关闭旧的：

```c
numa_strategy_slots_execute();
```

### 18.4 `src/numa_tinylfu.c`

新增：

```c
int tinylfu_execute_step(numa_strategy_t *strategy,
                         uint64_t deadline_us,
                         uint32_t budget);
```

vtable 增加：

```c
.execute_step = tinylfu_execute_step,
```

### 18.5 `src/numa_composite_lru.c`

新增：

```c
int composite_lru_execute_step(numa_strategy_t *strategy,
                               uint64_t deadline_us,
                               uint32_t budget);
```

保留旧 `execute()` 作为兼容 wrapper。

### 18.6 `src/numa_command.c`

扩展命令：

```text
NUMA STRATEGY SCHEDULER GET
NUMA STRATEGY SCHEDULER SET
NUMA STRATEGY SLOT STATS <slot>
NUMA STRATEGY SLOT CONFIG <slot> max_runtime_us <value>
NUMA STRATEGY SLOT CONFIG <slot> step_budget <value>
NUMA STRATEGY SLOT CONFIG <slot> scheduler ae|servercron
```

---

## 19. 实施顺序建议

### Step 1：添加 scheduler 字段，不改变行为

- 扩展结构体；
- 初始化 stats；
- 默认 `scheduler_mode = SERVERCRON`；
- 保持当前逻辑不变；
- 编译通过。

### Step 2：实现 AE scheduler 基础框架

- 添加注册 / 注销函数；
- slot enable 时注册；
- slot disable 时注销；
- callback 中先调用旧 `execute()`。

### Step 3：serverCron 只做 health check

- 加配置开关；
- 默认仍可 serverCron；
- 用 `NUMA STRATEGY SCHEDULER SET mode ae` 启用 AE。

### Step 4：TinyLFU 改为 `execute_step()`

- 增加 ring cursor；
- 支持 budget；
- 支持 deadline；
- 返回 `AGAIN` / `IDLE`。

### Step 5：Composite LRU 改为 `execute_step()`

- candidate ring 分批；
- scan cursor 分批；
- migration 分批。

### Step 6：stats / benchmark

增加 scheduler runtime 统计，对比：

- serverCron 模式；
- AE 模式；
- AE + deadline；
- AE + backoff。

### Step 7：可选 migration queue

- 统一迁移预算；
- 多策略共享 migration executor。

---

## 20. 预期收益

### 20.1 降低 serverCron 抖动

策略执行不再集中挤在 cron 中。

### 20.2 策略独立调度

不同策略可以拥有不同调度节奏：

- Composite LRU：1s 一次；
- TinyLFU：100ms 小步消费；
- 带宽策略：2s 一次；
- 用户策略：自定义。

### 20.3 更适合用户扩展

slot 有明确的 budget / deadline / stats，用户写慢策略时更容易被监控、限制和隔离。

### 20.4 更好的 tail latency 控制

每次主线程占用时间可以限制在 100–1000us 级别。

### 20.5 为未来后台迁移铺路

AE scheduler 可作为任务生成器，migration queue 可作为统一执行层。

---

## 21. 风险点

### 21.1 AE time event 仍阻塞主线程

必须强制策略小步化，不能把长任务原样搬进 AE。

### 21.2 time event 太多会增加调度开销

但当前策略 slot 最多 16 个，整体风险较低。

### 21.3 策略状态机复杂度增加

每个策略需要保存 cursor / pending state。

### 21.4 迁移语义仍需谨慎

主线程迁移安全；后台迁移复杂，需要版本校验和提交验证。

### 21.5 serverCron 与 AE 双路径兼容

初期需要避免同一个 slot 被 AE 和 serverCron 同时执行。

---

## 22. 推荐最终设计

最终建议采用：

> `serverCron` 只负责 NUMA scheduler 的健康维护；每个策略 slot 通过独立 AE time event 调度；策略主体实现 `execute_step(deadline_us, budget)`，每轮只做小批量、可续跑工作；迁移执行再进一步收敛到统一 migration queue，未来可演进到后台预拷贝 + 主线程提交。

这个方案与 Redis 原生 AE 模型兼容，也不会破坏 Redis 单线程对象安全模型，适合作为下一阶段 Redis-NUMA 策略框架演进方向。
