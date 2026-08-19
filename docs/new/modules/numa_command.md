# numa_command — 统一命令入口

`src/numa_command.c` / `.h`，命令注册于 `src/commands/numa.json`

## 1. 职责

`numa_command` 是所有 NUMA 相关能力的唯一用户入口：一个顶层 `NUMA` 命令，内部按
"域"（domain）路由到 `MIGRATE`/`CONFIG`/`FLOW`/`HELP` 四个子系统。它本
身不实现任何迁移、分配或调度逻辑——纯粹是参数解析 + 路由 + 把其他模块的返回值
格式化成 RESP 回复。这也是为什么它在模块依赖顺序里排在最后（见
`05-building-block-view.md`）：它依赖其余全部模块已经初始化完成。

> **[ADR-08](../09-architecture-decisions.md) 之后**：原来的第五个域 `STRATEGY`
> （`NUMA STRATEGY SLOT ...`/`NUMA STRATEGY LIST`，路由到已删除的
> `numa_strategy_slots.c`）已随该模块一起整体移除。三个迁移策略现在通过 `FLOW`
> 域管理，新增 `NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>` 用于在三个
> 预设间切换。`NUMA CONFIG LOAD`（热加载 Composite-LRU JSON）以及
> `access_tracking`/`locality_stats`/`debug_logging` 三个 `CONFIG SET` 参数也一并
> 移除，因为它们是 composite-lru 私有的配置面，随原生实现一起退役。

## 2. 接口

命令注册在 `src/commands/numa.json`（Redis 7 声明式命令自省系统，供
`COMMAND INFO`/`COMMAND DOCS` 使用）：

```json
{
    "NUMA": {
        "summary": "A container for NUMA-aware memory management commands.",
        "group": "server",
        "since": "6.2.21",
        "arity": -2,
        "function": "numaCommand",
        "command_flags": ["ADMIN", "NOSCRIPT", "LOADING", "STALE"]
    }
}
```

`command_flags` 里的 `ADMIN` 意味着权限控制走 Redis 通用的 `+@admin` ACL 分类，
**没有为 `NUMA` 单独定义 ACL 类别**（见第 6 节）。

完整命令表（下表逐条核对自 `numa_cmd_help()` 里的 `addReplyBulkCString` 调用，
即 `NUMA HELP` 的真实输出，而不是转述）：

| 命令 | 说明 |
| --- | --- |
| `NUMA MIGRATE KEY <key> <node>` | 迁移单个 key 到目标节点 |
| `NUMA MIGRATE DB <node>` | 迁移整个数据库到目标节点 |
| `NUMA MIGRATE SCAN [COUNT n]` | 触发一轮渐进式扫描迁移 |
| `NUMA MIGRATE STATS` | 显示迁移统计 |
| `NUMA MIGRATE RESET` | 重置迁移统计 |
| `NUMA MIGRATE INFO <key>` | 查询某 key 的 NUMA 元数据 |
| `NUMA CONFIG GET` | 显示当前分配器配置（含 `strategy_note`，见下） |
| `NUMA CONFIG SET strategy <name>` | 设置分配策略 |
| `NUMA CONFIG SET weight <node> <w>` | 设置节点权重 |
| `NUMA CONFIG SET cxl_optimization <on\|off>` | CXL 优化开关 |
| `NUMA CONFIG SET balance_threshold <percent>` | 再平衡阈值 |
| `NUMA CONFIG SET enabled_nodes <all\|n[,m]>` | 限制可用节点集合 |
| `NUMA CONFIG REBALANCE` | 手动触发再平衡 |
| `NUMA CONFIG STATS` | 每节点分配统计 |
| `NUMA FLOW LOAD <name> <path.json> [interval_sec] [ADAPT]` | 加载自定义 NUMAflow 工作流 |
| `NUMA FLOW RUN [name]` | 立即执行指定（或全部）工作流 |
| `NUMA FLOW LIST` | 列出已加载的工作流 |
| `NUMA FLOW STATUS <name>` | 查看某工作流最近一次运行的统计/反馈 |
| `NUMA FLOW UNLOAD <name>` | 卸载工作流 |
| `NUMA FLOW ADAPT <name> <ON\|OFF>` | 开关某工作流的自适应调参 |
| `NUMA FLOW DEFAULT <caat\|composite_lru\|tinylfu\|noop>` | 在三个内置策略预设（或 `noop`）间切换默认工作流 |
| `NUMA HELP` | 打印本帮助 |

`FLOW` 域转发给 `numa_flow_command()`，即 NUMAflow 桥接（`src/numa_flow.c`），
详见 [`docs/numaflow/README.md`](../../numaflow/README.md) 第 7 节。`NUMA HELP`
的输出现在完整列出了 `FLOW` 域的全部子命令（此前一版文档记录的"`HELP` 遗漏
`FLOW`"的不一致已经修复，见第 6 节）。

`NUMA MIGRATE STATS` 的返回字段是迁移统计本身（`total_migrations`/
`successful_migrations`/`failed_migrations`/`total_bytes_migrated`/
`total_migration_time_us`/`peak_concurrent_migrations`）加三个 `dboverwrite_*`
计数器，共 9 组键值对；已随 ADR-08 一起移除的 `tinylfu_accesses_*` 按节点拆分
计数字段（composite-lru/tinylfu 私有）不再出现在回复里。

## 3. 内部结构与关键路径

```text
numaCommand(client *c)                     — 顶层入口，解析 argv[1] 作为 domain
  ├─ "MIGRATE"  → numa_cmd_migrate(c)       — 解析 argv[2] 作为子命令
  │                 ├─ KEY / DB / SCAN / STATS / RESET / INFO
  ├─ "CONFIG"   → numa_cmd_config(c)
  │                 ├─ GET / SET / REBALANCE / STATS
  ├─ "FLOW"     → numa_flow_command(c)      — NUMAflow 桥接，见 numa_flow.c
  └─ "HELP"     → numa_cmd_help(c)
```

两个域路由函数（`numa_cmd_migrate`/`numa_cmd_config`）都声明
为 `static`（仅本文件可见），逐层匹配 `argv[N]->ptr` 的子命令字符串
（`strcasecmp`），参数个数不对或子命令未识别时统一走 `addReplyError`/
`addReplyErrorFormat` 返回错误，不会崩溃。这一层本身不接触任何 NUMA 分配器/迁移
状态，只是把已解析好的参数转发给对应模块的真实函数（如
`numa_migrate_single_key()`、`numa_flow_run_default()`），这也是为什么这个模
块的代码可以整体独立于其余模块被理解——它是纯粹的适配层。

## 4. 质量与性能特性

- **参数校验先行**：每个子命令处理函数在做任何实际工作之前，先校验 `c->argc`
  是否匹配预期个数，不匹配直接返回 `Wrong number of arguments` 类错误。
- **NUMA 不可用时的降级**：若 NUMA 分配器未初始化（`numa_pool_available()` 为
  假），相关命令返回 `NUMA is not available` 而不是崩溃或返回垂悬数据。
- **复杂度**：`MIGRATE KEY` 是 O(key 大小)；`MIGRATE SCAN` 委托给 NUMAflow
  `default` 工作流一次执行，复杂度取决于该工作流图本身（`COUNT` 参数只为 CLI
  兼容性保留，实际被忽略）；`CONFIG GET/SET` 是 O(1)；`FLOW LIST` 是
  O(已加载工作流数)。
- **单线程执行**：所有子命令都在 Redis 主线程同步执行，会阻塞其他命令——这对
  `MIGRATE DB`（整库迁移）和大 `COUNT` 的 `MIGRATE SCAN` 尤其重要，生产环境建议
  在低峰期执行大批量操作，或改用 `MIGRATE SCAN` 的小批次滚动方式而不是一次性
  `MIGRATE DB`。

## 5. 与其他模块的关系

`numa_command` 处在模块依赖链的最顶端（见
[`05-building-block-view.md`](../05-building-block-view.md) 的依赖图）：它是
`numa_key_migrate`（`MIGRATE` 域）、`numa_configurable_strategy`/整体分配器状态
（`CONFIG` 域）、以及 NUMAflow 桥接
`numa_flow.c`（`FLOW` 域）共同的用户可见入口。它不会被其他任何 NUMA 模块调用
——只有 `server.c` 的命令表会调用到 `numaCommand()`。

## 6. 未解决问题与已知限制

- **没有为 `NUMA` 定义专用 ACL 分类**：权限完全依赖 Redis 通用的 `ADMIN`
  command flag（即 `+@admin`），无法用 ACL 做比"整个 NUMA 命令能不能用"更细粒
  度的控制（例如"只允许 `CONFIG GET`，禁止 `MIGRATE DB`"）。如果要在多租户场景
  下收紧权限，需要先在 Redis ACL 层面扩展。
- **大批量操作会阻塞主线程**：`MIGRATE DB` 都在
  主线程同步跑完才返回，没有走异步/分片执行；`MIGRATE SCAN` 现在只是触发
  NUMAflow `default` 工作流跑一次，单次执行预算由该工作流图自身的
  `budget_limit` 原子操作控制，不再由 `NUMA MIGRATE SCAN` 的 `COUNT` 参数决定。
