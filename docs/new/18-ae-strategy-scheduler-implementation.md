# Redis-NUMA AE Strategy Scheduler 初步实现说明

## 1. 实现目标

本次改动在现有 `serverCron` 策略 slot 调度路径之外，增加一套可选的 Redis AE time event 调度骨架。目标不是一次性把 Composite LRU 和 TinyLFU 全部改成真正异步迁移，而是先把调度入口接入 Redis 原生事件循环，为后续 `execute_step(deadline_us, budget)` 小步执行和迁移预算控制提供基础。

当前默认行为仍保持 `serverCron` 调度，避免影响已有 benchmark 结果。AE 调度能力通过新增接口显式启用。

## 2. 本次代码改动

### 2.1 vtable 扩展

`numa_strategy_vtable_t` 新增可选接口：

```c
int (*execute_step)(numa_strategy_t *strategy,
                    uint64_t deadline_us,
                    uint32_t budget);
```

调度器优先调用 `execute_step()`。如果策略尚未实现该接口，则回退调用原有 `execute()`，保证 Composite LRU 和 TinyLFU 在未改造前仍可运行。

### 2.2 slot 结构扩展

`numa_strategy_t` 增加 AE 调度和执行统计字段：

```c
int scheduler_mode;
long long ae_time_event_id;
uint32_t step_budget;
uint32_t max_runtime_us_per_step;
uint64_t max_execution_time_us;
uint64_t timeout_count;
uint64_t last_ae_run_us;
```

默认值为：

```text
scheduler_mode = SERVERCRON
ae_time_event_id = AE_DELETED_EVENT_ID
step_budget = 64
max_runtime_us_per_step = 500
```

### 2.3 AE 调度接口

`numa_strategy_slots.c` 新增：

```c
int numa_strategy_scheduler_init(aeEventLoop *el);
int numa_strategy_slot_schedule_ae(int slot_id);
int numa_strategy_slot_unschedule_ae(int slot_id);
void numa_strategy_scheduler_cron(void);
```

其中：

- `numa_strategy_scheduler_init(server.el)` 保存 Redis 主事件循环指针；
- `numa_strategy_slot_schedule_ae(slot_id)` 将指定 slot 注册为 AE time event；
- `numa_strategy_slot_unschedule_ae(slot_id)` 删除 slot 的 AE time event，并切回 `serverCron` 模式；
- `numa_strategy_scheduler_cron()` 只做轻量健康检查，发现 AE 模式 slot 的 event 丢失时重新注册。

### 2.4 serverCron 兼容路径

`numa_strategy_run_all()` 现在只执行 `scheduler_mode == SERVERCRON` 的 slot，避免同一个策略被 `serverCron` 和 AE time event 双路径重复执行。

`serverCron` 仍每秒调用：

```c
numa_strategy_run_all();
numa_strategy_scheduler_cron();
```

因此默认策略仍按原有方式执行；只有显式调用 `numa_strategy_slot_schedule_ae()` 的 slot 才会进入 AE 调度。

### 2.5 命令层开关

为了便于本地验证，`NUMA STRATEGY SLOT` 增加两个子命令：

```text
NUMA STRATEGY SLOT SCHEDULE <slot> ae|servercron
NUMA STRATEGY SLOT STATUS <slot>
```

其中 `SCHEDULE` 用于在运行时把指定 slot 切换到 AE 或 `serverCron` 调度；`STATUS` 用于查看 scheduler mode、AE event id、执行次数、最大执行时间和 timeout 计数。

## 3. AE time event 执行逻辑

AE callback 的执行流程为：

```text
AE time event fired
  ├── 根据 slot_id 找到 strategy
  ├── 检查 enabled 和 ae_time_event_id
  ├── 生成 deadline_us = now + max_runtime_us_per_step
  ├── 优先调用 execute_step(strategy, deadline_us, step_budget)
  ├── 否则回退 execute(strategy)
  ├── 更新 total_executions / total_time / max_time / timeout_count
  └── 返回下一次调度延迟
```

当前下一次调度规则较保守：

```text
AGAIN / TIMEOUT -> 1ms 后再次调度
其他结果        -> execute_interval_us
```

后续可以在此基础上加入 backoff、priority 和全局 per-loop runtime budget。

## 4. 当前边界

本次实现只是调度骨架，不改变迁移语义：

1. Composite LRU 和 TinyLFU 尚未实现真正的 `execute_step()`；
2. 大对象迁移仍可能在主线程同步执行；
3. AE time event 本身仍运行在 Redis 主线程，不等价于后台并行；
4. 默认不自动把 slot 1 或 slot 2 切到 AE 模式；
5. 暂未新增 Redis 命令暴露 `SCHEDULER SET` 或 `SLOT CONFIG scheduler ae`。

这样做的原因是避免一次性改变 benchmark 行为，并把风险集中在可编译、可回退的调度框架上。

## 5. 后续实现顺序

### Step 1：验证命令层开关

当前已经提供：

```text
NUMA STRATEGY SLOT SCHEDULE <slot> ae|servercron
NUMA STRATEGY SLOT STATUS <slot>
```

可先用 slot 0 或 slot 2 做低风险验证，确认 AE event 注册、注销和状态统计符合预期。

### Step 2：TinyLFU execute_step

TinyLFU 已经有 candidate ring 和迁移预算，更适合作为第一批改造对象。下一步应将一次 `execute()` 中的 ring 消费改为按 `budget` 分批处理，并在处理若干候选后检查 `deadline_us`。

目标返回值：

```text
IDLE      本轮无候选
PROGRESS  本轮完成部分处理
AGAIN     ring 中仍有积压，希望尽快继续
TIMEOUT   达到 deadline，主动让出
ERROR     策略执行失败
```

### Step 3：Composite LRU execute_step

Composite LRU 需要将 candidate ring 消费、渐进 scan 和迁移分别小步化。迁移操作仍在主线程执行，但每轮必须限制迁移数量和执行时间。

### Step 4：统一 migration queue

当两个策略都支持 step 后，再把迁移动作进一步收敛到统一 migration queue：

```text
strategy execute_step -> enqueue migration task
AE migration executor -> process limited tasks
```

这样可以避免多策略同时迁移导致主线程单轮占用过长。

## 6. 验证标准

当前骨架阶段的验证标准：

1. `cd src && make -j$(nproc)` 编译通过；
2. 默认配置启动 Redis 后，slot 仍通过 `serverCron` 执行；
3. 显式调用 `numa_strategy_slot_schedule_ae(slot)` 后，该 slot 不再被 `numa_strategy_run_all()` 重复执行；
4. `numa_strategy_slot_status()` 能输出 scheduler、AE event id、step budget、max runtime、timeout 等统计；
5. 禁用或移除 slot 时，已注册的 AE time event 会被删除；
6. 未实现 `execute_step()` 的策略能够回退到 `execute()`。

## 7. 预期收益

该骨架完成后，Redis-NUMA 策略框架具备从“所有策略挤在 `serverCron` 中串行执行”演进到“每个策略 slot 独立 AE time event 调度”的基础能力。真正的性能收益需要在 TinyLFU / Composite LRU 小步化之后体现：策略执行可以被 deadline 和 budget 限制，减少单次主线程阻塞，并为后续异步迁移队列提供调度入口。
