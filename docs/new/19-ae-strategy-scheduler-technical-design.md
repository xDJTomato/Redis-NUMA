# Redis-NUMA AE 策略调度器

## 1. 模块定位

AE 策略调度器是 `numa_strategy_slots` 的调度层扩展，用于将 NUMA 迁移策略从单一 `serverCron` 周期执行模型扩展为可选的 Redis AE time event 执行模型。

该模块不改变策略 slot 的注册、启用、禁用和优先级模型，只扩展策略的触发方式。默认情况下，所有策略仍保持 `serverCron` 调度；只有显式切换到 AE 模式的 slot 才会注册独立 time event。

## 2. 设计目标

1. **兼容现有策略框架**：保留原有 `execute()` 接口和 `serverCron` 默认路径。
2. **支持小步执行**：新增 `execute_step(deadline_us, budget)`，允许策略按时间和工作量预算分批执行。
3. **避免重复调度**：同一 slot 在任意时刻只属于一种调度模式。
4. **限制主线程占用**：通过 per-step deadline 和 budget 降低策略执行造成的长尾延迟。
5. **提供可观测状态**：记录调度模式、AE event id、执行次数、最大耗时和 timeout 次数。

## 3. 调度模型

每个策略 slot 有两种调度模式：

```text
SERVERCRON
    由 serverCron 周期调用 numa_strategy_run_all()

AE
    由 Redis AE time event 单独触发该 slot
```

`numa_strategy_run_all()` 只执行 `scheduler_mode == SERVERCRON` 的策略。切换到 AE 模式时，调度器为该 slot 创建 time event；切回 `serverCron` 时删除对应 time event。

## 4. 数据结构扩展

`numa_strategy_t` 增加以下字段：

```c
int scheduler_mode;
long long ae_time_event_id;
uint32_t step_budget;
uint32_t max_runtime_us_per_step;
uint64_t max_execution_time_us;
uint64_t timeout_count;
uint64_t last_ae_run_us;
```

字段含义：

| 字段 | 含义 |
| --- | --- |
| `scheduler_mode` | 当前调度模式，`SERVERCRON` 或 `AE` |
| `ae_time_event_id` | Redis AE time event id，未注册时为删除态 |
| `step_budget` | 单次小步执行最多处理的工作量 |
| `max_runtime_us_per_step` | 单次小步执行目标时间上限 |
| `max_execution_time_us` | 观测到的最大单次执行时间 |
| `timeout_count` | 因达到 deadline 主动让出的次数 |
| `last_ae_run_us` | 最近一次 AE 执行时间 |

## 5. 策略接口

策略 vtable 增加可选接口：

```c
int (*execute_step)(numa_strategy_t *strategy,
                    uint64_t deadline_us,
                    uint32_t budget);
```

调用规则：

1. 调度器优先调用 `execute_step()`。
2. 未实现 `execute_step()` 的策略回退调用 `execute()`。
3. `deadline_us == 0` 表示不使用时间截止点。
4. `budget == 0` 表示由策略使用自身默认预算。

返回值用于区分执行状态：

| 返回值 | 含义 |
| --- | --- |
| `IDLE` | 无可处理工作 |
| `PROGRESS` | 本轮完成部分工作 |
| `DONE` | 本轮完成全部工作 |
| `AGAIN` | 仍有积压，需要尽快继续 |
| `TIMEOUT` | 达到 deadline，主动让出 |
| `ERROR` | 执行失败 |

只有负返回值计入 failure；`PROGRESS`、`AGAIN`、`TIMEOUT` 都属于正常调度状态。

## 6. AE time event 执行流程

AE callback 的单轮流程如下：

```text
1. 根据 slot id 查找 strategy
2. 检查 strategy 是否仍启用且仍处于 AE 模式
3. 计算 deadline_us = now + max_runtime_us_per_step
4. 调用 execute_step(strategy, deadline_us, step_budget)
5. 更新执行次数、耗时、最大耗时和 timeout 统计
6. 根据返回值决定下一次触发间隔
```

下一次触发规则：

| 返回值 | 下一次触发 |
| --- | --- |
| `AGAIN` / `TIMEOUT` | 短间隔继续执行 |
| 其他非错误状态 | 使用策略配置的执行间隔 |
| `ERROR` | 记录失败后使用策略配置的执行间隔 |

## 7. Composite LRU 接入方式

Composite LRU 是默认迁移策略，包含热点候选池和渐进扫描两个通道。

接入小步执行后：

1. 候选池维护 `head/tail/count`，作为真正的环形队列。
2. 访问路径只写入候选，不执行迁移。
3. 执行路径从 `tail` 消费候选，每轮最多处理 `budget` 个候选。
4. 迁移前重新查询 key 当前热度和所在 NUMA 节点，避免依赖入队时快照。
5. 候选池处理后仍有剩余预算时，推进有限数量的渐进扫描。

`composite_lru_execute()` 保留为兼容入口，内部调用 `composite_lru_execute_step(strategy, 0, 0)`。

## 8. TinyLFU 接入方式

TinyLFU 使用 Count-Min Sketch 和 Doorkeeper 识别高频访问 key。

接入小步执行后：

1. candidate ring 维护 `head/tail/count`。
2. `record_access()` 只更新频率结构并写入候选。
3. `execute_step()` 按预算从 ring 中弹出候选。
4. 迁移前重新估计当前频率，低于阈值的候选会被丢弃。
5. ring 仍有积压时返回 `AGAIN`，达到 deadline 时返回 `TIMEOUT`。

## 9. 运行时控制

策略调度模式通过 `NUMA STRATEGY SLOT` 子命令控制：

```text
NUMA STRATEGY SLOT SCHEDULE <slot> ae
NUMA STRATEGY SLOT SCHEDULE <slot> servercron
NUMA STRATEGY SLOT STATUS <slot>
```

`STATUS` 用于观察 slot 的调度模式、AE event id、小步预算、单轮时间上限、最大执行时间和 timeout 计数。

## 10. 约束与边界

1. AE time event 仍运行在 Redis 主线程，不提供并行迁移能力。
2. `deadline_us` 只在策略循环边界检查，不能中断已经开始的单个 key 迁移。
3. 大对象迁移仍可能产生单次主线程阻塞。
4. 多个策略同时启用时，尚未共享全局迁移预算。
5. 当前调度器只负责任务触发，不改变策略的迁移决策语义。

## 11. 后续方向

后续可将策略识别和实际迁移进一步解耦为统一迁移队列：

```text
strategy execute_step -> enqueue migration task
migration executor -> process tasks under global budget
```

统一迁移队列可以在多个策略之间共享主线程迁移预算，并为更细粒度的优先级控制、限速和延迟保护提供基础。
