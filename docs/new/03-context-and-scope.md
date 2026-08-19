# 3. 系统上下文与范围（Context and Scope）

> arc42 §3。本章回答一个问题：**这个系统的边界在哪里，边界外面是谁，通过什么接口
> 交换什么数据。** 不涉及内部实现——内部怎么做的，见 [05-building-block-view.md](05-building-block-view.md)
> 及 `modules/` 下各模块详情页。

## 3.1 业务上下文（谁在用这个系统，怎么用）

从使用者的角度看，本项目对外只暴露一件事：**一个完全兼容标准 Redis 协议
（RESP）的服务端，外加一组新增的 `NUMA` 命令族**。任何标准 Redis 客户端库（无
需修改）都能连接、读写数据；只有当客户端想主动查询/控制 NUMA 行为时，才需要发
出 `NUMA ...` 命令。

| 角色 | 通过什么接口 | 交换什么数据 |
| --- | --- | --- |
| 普通业务客户端 | 标准 RESP 协议（`SET`/`GET`/`HSET`/... 全部原生命令不变） | 正常的 key-value 读写；对客户端完全透明——不知道、也不需要知道数据实际被分配在哪个 NUMA 节点 |
| 运维/监控客户端 | `NUMA CONFIG GET/SET/STATS`、`NUMA MIGRATE STATS/INFO`、`NUMA FLOW LIST/STATUS` | 查询当前分配策略、各节点内存占用、迁移统计；调整策略参数（权重、阈值等） |
| 高级用户 / 二次开发者 | `NUMA MIGRATE KEY/DB/SCAN`、`NUMA FLOW LOAD/RUN/DEFAULT/ADAPT/...` | 手动触发迁移；用 `NUMA FLOW DEFAULT` 在 `caat`/`composite_lru`/`tinylfu`/`noop` 间切换；加载 NUMAflow 编排出的自定义 DAG 工作流 |
| 部署/运维脚本 | `redis.conf` 中的 `numa-enabled`、`numa-demote-*`、`numa-flow-default-strategy`、`numa-flow-interval-sec` 等配置项 | 启动前静态配置分配策略、降级阈值、NUMAflow 默认迁移策略及其运行间隔 |

命令语法的完整参考见 [`modules/numa_command.md`](modules/numa_command.md)；`redis.conf`
配置项的完整清单见仓库根目录 `redis.conf`（第 1184–1211、2338–2354 行）。

## 3.2 技术上下文（系统边界内外的技术组件）

```text
                        ┌───────────────────────────┐
   RESP 客户端  ───────►│                           │
  （标准 Redis 协议 +    │      redis-server 进程     │
     NUMA 命令族）       │  （本项目：Redis 7.2.6 +   │
                        │   10 个 NUMA 模块）         │
                        │                           │
                        │  ┌─────────────────────┐  │
                        │  │  NUMAflow 引擎        │  │◄── 可选：GUI/TUI 通过
                        │  │ （进程内嵌入，通过     │  │    进程外工具编排 DAG，
                        │  │  numa_flow.c 桥接）    │  │    以 JSON 文件交换
                        │  └─────────────────────┘  │
                        └──────────┬────────────────┘
                                   │ libnuma 系统调用
                                   │ (numa_alloc_onnode / numa_available / ...)
                                   ▼
                        ┌───────────────────────────┐
                        │   操作系统 NUMA 子系统      │
                        │  （真实硬件拓扑，或          │
                        │   QEMU/CXLMemSim 仿真出的   │
                        │   拓扑）                   │
                        └───────────────────────────┘
```

| 边界两侧 | 接口 | 传递的数据/职责 |
| --- | --- | --- |
| 客户端 <-> `redis-server` | RESP over TCP（标准 Redis 协议） | 见 3.1 |
| `redis-server` <-> 操作系统 NUMA 子系统 | `libnuma`（`numa_alloc_onnode`、`numa_available`、`numa_node_of_cpu` 等） | 内存分配/迁移的物理落点；`redis-server` 本身不直接操作硬件寄存器，一切通过内核暴露的 NUMA API |
| `redis-server` 进程内 <-> NUMAflow 引擎 | `src/numa_flow.c` 实现的两个桥接回调：`enumerate`（把 keyspace 产出为 `nf_item_t`）、`apply`（把某 key 迁移到目标节点） | NUMAflow 是**进程内嵌入**的库，不是独立进程；边界是函数调用，不是网络/IPC |
| NUMAflow 引擎 <-> GUI/TUI 编排工具 | JSON 工作流文件（DAG 定义），`NUMA FLOW LOAD <name> <path.json>` | 用户在 GUI（`numaflow/gui/`）里拖拽拼出的策略，导出成 JSON，再交给 `redis-server` 加载执行——两者之间没有运行时网络连接，只有文件交换 |
| `redis-server` <-> QEMU/CXLMemSim（测试环境） | 进程外：QEMU 启动一个客户机，`redis-server` 在客户机**内部**运行；本机开发环境不直接依赖 QEMU | 仅用于验证——生产部署不涉及 QEMU；边界详见 [07-deployment-view.md](07-deployment-view.md) |
| `redis-server` <-> `external/CXLMemSim` | 通过 CXLMemSim 改版的 QEMU + `cxlmemsim_server`（TCP 时序转发协议），或直接调用 `libcxlmemsim.a`（`tests/cxl/cxlmemsim_workload_bench.cpp`） | 该组件**不属于本仓库**（`SlugLab/CXLMemSim`，未 vendor 进历史），只用于设备级时序仿真校验，不是运行时依赖 |

## 3.3 范围声明：这个系统不做什么

明确排除在系统边界之外的东西，避免歧义：

- **不实现真正的 CXL 硬件驱动**——CXL 内存对本项目而言就是「又一个 NUMA 节点」，
  硬件层面的枚举/绑定完全交给操作系统的 `cxl_pci`/`cxl_acpi` 驱动或 CXLMemSim
  的仿真设备；本项目只消费 `libnuma` 暴露出来的节点抽象。
- **不修改 Redis 客户端协议**——没有任何新的 wire-protocol 概念，`NUMA` 只是一个
  普通的 Redis 命令，标准客户端库天然兼容。
- **NUMAflow 不是一个独立可远程调用的服务**——它是编译进 `redis-server` 二进制
  的库代码（`HAVE_NUMA` 下）；GUI/TUI 只是编排 DAG 文件的工具，不在运行时与
  `redis-server` 保持连接。
- **QEMU/CXLMemSim 属于测试基础设施，不属于生产部署形态**——见
  [07-deployment-view.md](07-deployment-view.md) 中生产/测试环境的区分。
