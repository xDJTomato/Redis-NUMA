# AE 策略调度器

> **已退役（[ADR-08](../09-architecture-decisions.md)）**：本文档描述的调度机制
> 是 `numa_strategy_slots`（[numa_strategy_slots.md](numa_strategy_slots.md)）的
> 调度层扩展，调度对象——策略槽位——本身已随 `src/numa_strategy_slots.{c,h}` 一起
> 从代码库删除，这套 AE/servercron 逐槽位调度开关随之整体失效，没有留下替代的
> "内核原生"调度层。三个迁移策略现在统一由 NUMAflow 的原子操作框架实现
> （`numaflow/src/nf_strategy.c`），调度模型是 `numa_flow_cron()` 按每个工作流
> 各自的 `interval_sec` 判断是否该跑——单线程 `serverCron` 驱动，没有 AE
> time-event 变体，也没有"某个工作流单独切到 AE"这样的旋钮。以下内容保留作为
> 该调度机制曾经存在过的设计记录。

## 背景

`numa_strategy_slots`（见 [numa_strategy_slots.md](numa_strategy_slots.md)）最初只有一种调度方式：`serverCron` 每秒轮询、串行执行全部已启用的策略槽位。槽位数量少、Composite LRU / TinyLFU 的单次执行开销可控时，这个模型没有明显问题。但它有三个结构性风险：

- **慢策略拖累全局**：`serverCron` 同时承担过期、超时、统计等大量核心维护任务，一个槽位执行过久会拖长整个 `serverCron` 周期，间接影响服务稳定性；
- **调度粒度被绑死**：所有槽位共享同一个 `serverCron` 节奏，无法有独立的执行频率；
- **接口鼓励写出阻塞式策略**：原始 `execute()` 接口没有时间/工作量上限，扩展者很容易在一次调用里做完整表扫描或批量迁移，制造主线程尾延迟。

AE 策略调度器就是为解决这三个问题而设计并落地的：把槽位的触发方式从"挂在 `serverCron` 上"改造成"可选挂到 Redis 自己的 AE（异步事件循环）time event 上"，并强制新接口按小步、限时、可续跑的方式执行。

## 职责

AE 策略调度器是 `numa_strategy_slots` 的**调度层扩展**，不改变槽位的注册、启用/禁用、优先级模型，只扩展"槽位什么时候被触发执行"这一件事。默认情况下所有槽位仍走 `serverCron`；只有显式切到 AE 模式的槽位才会注册独立的 time event。

## 接口

### `execute_step` vtable 扩展

在原有 `init()` / `execute()` / `cleanup()` 之外，`numa_strategy_vtable_t` 新增一个可选接口（`src/numa_strategy_slots.h`）：

```c
int (*execute_step)(numa_strategy_t *strategy, uint64_t deadline_us, uint32_t budget);
```

调用规则：调度器优先调用 `execute_step()`；策略若未实现该接口，回退调用旧的 `execute()`——这保证了框架升级不会破坏尚未适配的策略。`deadline_us == 0` 表示不设时间截止点，`budget == 0` 表示策略使用自身默认预算。

返回值（`src/numa_strategy_slots.h`）：

| 返回值 | 含义 |
| --- | --- |
| `NUMA_STRATEGY_STEP_IDLE` (0) | 本轮无事可做 |
| `NUMA_STRATEGY_STEP_PROGRESS` (1) | 本轮处理了一部分工作 |
| `NUMA_STRATEGY_STEP_DONE` (2) | 当前积压已清空 |
| `NUMA_STRATEGY_STEP_AGAIN` (3) | 仍有积压，希望尽快再次调度 |
| `NUMA_STRATEGY_STEP_ERROR` (-1) | 策略内部错误 |
| `NUMA_STRATEGY_STEP_TIMEOUT` (-2) | 触及 deadline，主动让出 |

只有负值计入失败统计（`total_failures`）；`PROGRESS`/`AGAIN`/`TIMEOUT` 都是正常调度状态，不会被误判为策略出错。

### 命令接口

```text
NUMA STRATEGY SLOT SCHEDULE <slot> ae|servercron   # 运行时切换该槽位的调度模式
NUMA STRATEGY SLOT STATUS <slot>                   # 查看调度模式、AE event id、执行统计
```

`SCHEDULE` 对应 `numa_strategy_slot_schedule_ae()` / `numa_strategy_slot_unschedule_ae()`；`STATUS` 输出调度模式、AE time event id、`step_budget`、`max_runtime_us_per_step`、`total_executions`、`max_execution_time_us`、`timeout_count` 等字段（`src/numa_command.c`）。

## 内部结构与关键路径

### 槽位结构扩展

`numa_strategy_t`（`src/numa_strategy_slots.h`）新增的调度字段：

```c
int scheduler_mode;                  /* SERVERCRON(0) 或 AE(1) */
long long ae_time_event_id;          /* 当前注册的 AE time event id */
uint32_t step_budget;                /* 单次小步最多处理的工作量 */
uint32_t max_runtime_us_per_step;    /* 单次小步的时间上限 */
uint64_t max_execution_time_us;      /* 观测到的最大单次执行耗时 */
uint64_t timeout_count;              /* 触及 deadline 主动让出的次数 */
uint64_t last_ae_run_us;             /* 最近一次 AE 执行时间 */
```

### AE 注册 / 执行 / 注销

- `numa_strategy_scheduler_init(aeEventLoop *el)`：保存 Redis 主事件循环指针，供后续注册 time event 用；
- `numa_strategy_slot_schedule_ae(slot_id)`：若槽位已启用且尚未注册 AE event，调用 `aeCreateTimeEvent()` 创建一个以 `execute_interval_us` 为间隔的 time event，并把 `scheduler_mode` 置为 `AE`；
- `numa_strategy_slot_unschedule_ae(slot_id)`：调用 `aeDeleteTimeEvent()` 删除该槽位的 time event，`scheduler_mode` 切回 `SERVERCRON`；
- `numa_strategy_slot_time_proc()`（AE callback）：每次触发时计算 `deadline_us = now + max_runtime_us_per_step`，优先调用 `execute_step(strategy, deadline_us, step_budget)`，否则回退 `execute(strategy)`；更新执行次数/耗时/超时统计后，根据返回值决定下一次触发延迟：`AGAIN`/`TIMEOUT` → 1ms 后再触发，其余情况 → 按 `execute_interval_us` 正常间隔；
- `numa_strategy_scheduler_cron()`：`serverCron` 每秒调用一次的轻量健康检查——扫描所有槽位，凡是"应该在 AE 模式但 time event 已丢失"的，重新注册；
- `numa_strategy_run_all()`（原有的 `serverCron` 执行路径）现在**只执行 `scheduler_mode == SERVERCRON` 的槽位**，避免同一个策略被两条路径重复执行。

### Composite LRU / TinyLFU 的适配

两个内置策略都已经实现了 `execute_step()`（而不是仍然只有旧的 `execute()`）：

- **TinyLFU**（`src/numa_tinylfu.c`）：候选环形缓冲区维护 `head`/`tail`/`count`，`execute_step()` 按 `budget` 从 ring 中弹出候选分批处理，迁移前重新估计当前频率（低于阈值的候选会被丢弃），仍有积压返回 `AGAIN`，触及 deadline 返回 `TIMEOUT`。
- **Composite LRU**（`src/numa_composite_lru.c`）：热点候选池同样改为真正的环形队列（`head`/`tail`/`count`），`execute_step()` 从 `tail` 分批消费候选（迁移前重读 key 当前热度和所在节点，不依赖入队时的旧快照），候选处理完后若仍有预算，再推进有限步数的渐进式字典扫描。`composite_lru_execute()` 保留为 `serverCron` 兼容入口，内部直接调用 `composite_lru_execute_step(strategy, 0, 0)`。

两者的改造都遵循同一条约束：**一次 `execute_step()` 调用只能做小批量、可恢复状态（cursor/ring 位置）的工作**，不能像旧的 `execute()` 那样一次性处理到底。

## 质量与性能特性

这是这份文档最需要精确的地方——"设计提案"（原 17 号文档）里描述的很多能力，与"实际落地"（原 18/19 号文档，已在本次核对中于 `src/numa_strategy_slots.c` 逐项验证）之间是有差距的：

**已经真实上线（Phase A，本节所有结论均已用 `grep` 核对 `src/numa_strategy_slots.c`/`.h`、`src/numa_command.c` 源码确认）：**

- 槽位可以独立挂到 AE time event 上，不再必须挤在 `serverCron` 单一周期里；
- TinyLFU、Composite LRU 均已实现 `execute_step(deadline_us, budget)`，具备小步、限时、可续跑的执行语义；
- `serverCron` 保留为默认路径与健康检查兜底（丢失的 AE event 会被 `numa_strategy_scheduler_cron()` 自动重新注册）；
- 未实现 `execute_step()` 的策略能安全回退到旧的 `execute()`，不会因框架升级而失效；
- `NUMA STRATEGY SLOT SCHEDULE`/`STATUS` 命令可以在运行时切换和观测调度模式。

**设计中提出、但目前尚未实现（不要在这两点上假设它们已经生效）：**

- **多槽位公平性机制**（原设计的注册 jitter 错峰、全局每轮 runtime budget、按 priority 动态 backoff）——当前实现里 `numa_strategy_slot_schedule_ae()` 没有加入任何错峰逻辑，`numa_strategy_slot_time_proc()` 的下一次调度延迟只有两档（`AGAIN`/`TIMEOUT` → 1ms，否则 → 配置间隔），没有 `backoff_level`、没有连续失败自动降频/自动禁用；
- **统一的内核态 migration queue**（原设计 Phase B/C：槽位只负责 enqueue，独立执行器统一消费迁移预算，甚至后台线程预拷贝大对象）——这条路径在 Redis 内核里没有实现；`src/numa_command.c` 里也没有 `NUMA STRATEGY SCHEDULER SET`/`SLOT CONFIG` 这类曾经提议的全局配置命令。等价的思路**已经在 NUMAflow 子系统里落地**：策略被拆成 36 个原子操作，由 DAG 执行器按拓扑排序调度，`budget_limit` 与 `emit_migrate` 在统一预算与容量约束下执行迁移（见 [NUMAflow 子系统](../../numaflow/README.md)）——但这是内核之外的一条独立路径，不是对内核 AE 调度器的直接升级。

换句话说：**AE 调度器本身的骨架（Phase A）是完整、真实可用的；围绕它的公平性调优和统一迁移队列（Phase B/C）仍然只是设计意图**，如果要依赖这些能力，需要先补上实现，而不能假设命令或字段已经存在。

## 与其他模块的关系

AE 策略调度器不是一个独立模块，而是 [numa_strategy_slots](numa_strategy_slots.md) 的调度方式扩展——它不改变"哪个槽位装了哪个策略"这件事，只改变"槽位什么时候被触发"。它同时依赖 Composite LRU（[numa_composite_lru.md](numa_composite_lru.md)）和 TinyLFU（[numa_tinylfu.md](numa_tinylfu.md)）各自实现的 `execute_step()`；这三份文档应该配合读，本文档只讲调度层，不重复策略本身的算法细节。

## 未解决问题与已知限制

以下是原设计文档"风险点"一节里仍然成立、需要如实保留的限制，而不是可以忽略的历史顾虑：

- **AE time event 本身仍运行在 Redis 主线程**，接入 AE 不等价于并行执行。如果 `execute_step()` 内部真的做了一次 10ms 的同步 `memcpy`，即使挂在 AE time event 上，依然会让 Redis 主线程停顿 10ms——AE 调度器只解决"调度分散、限时限量"，不解决"单步内部本身很慢"；
- **time event 数量增多会带来调度开销**，目前策略槽位上限是 16 个，整体风险可控，但如果未来槽位数量大幅增加，需要重新评估；
- **策略状态机复杂度上升**：每个适配了 `execute_step()` 的策略都必须自己维护 cursor/ring 位置等续跑状态，比原来一次性 `execute()` 的实现方式更容易出 bug；
- **大对象迁移仍可能在单次 `execute_step()` 内阻塞主线程**：`deadline_us` 只在策略的循环边界被检查，无法中断一个已经开始的单个 key 迁移；
- **多槽位之间尚未共享全局迁移预算**：如果多个槽位同时切到 AE 模式且都在积压，目前没有机制防止它们在同一时刻叠加占用主线程时间。
