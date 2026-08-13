# Redis-NUMA: Redis 6.2.21 → Redis 8 迁移指南

> 本文件记录将本项目的 NUMA 异构内存优化层从 Redis 6.2.21 迁移到 Redis 8 的
> 完整路径。由于本机为 16 GB 的 Windows 笔记本（无 libnuma / 无 QEMU / 无 Linux
> 构建环境），Redis 核心需要在 Linux 机器上完成最终编译；本文档逐项定位受影响的
> 集成点，并提供 `src/redis8_compat.h` 兼容头以及可直接套用的代码改动清单。
> 全新的策略子系统（NUMAflow，见 `docs/numaflow/`）是纯 C11 独立实现，已在本机
> 通过 MinGW 全部编译与测试。

## 1. 迁移范围总览

本项目的 NUMA 层由以下模块组成，全部位于 `src/` 且由 `#ifdef HAVE_NUMA` 保护：

| 模块 | 作用 | 受 Redis 8 影响点 |
| --- | --- | --- |
| `numa_pool` | 自定义 NUMA 内存池 | 低（仅依赖 zmalloc） |
| `numa_migrate` | 跨节点块迁移 | 低（libnuma + memcpy） |
| `numa_key_migrate` | 按 key 迁移（5 种类型适配器） | 中（`dict` / `robj` / `listpack`） |
| `numa_strategy_slots` | 16-slot 策略框架 | 中（AE 事件、`aeEventLoop`） |
| `numa_composite_lru` | 默认迁移策略（slot 1） | 高（LRU clock、dict 迭代、`serverLog`） |
| `numa_tinylfu` | 频率驱动策略（slot 2） | 高（同上） |
| `numa_configurable_strategy` | 9 种分配策略 | **高（`atomicvar.h` 全面重构）** |
| `numa_command` | `NUMA` 命令 | 中（`addReply*` API） |
| `numa_bw_monitor` | 带宽监控 | 低 |
| `evict_numa` | NUMA 感知淘汰 | 高（`evictionPoolEntry` 结构变更） |

## 2. 关键破坏性变更（6.2 → 8）

### 2.1 `atomicvar.h` 全面重构（影响最大）

Redis 6.2 的 `src/atomicvar.h` 定义了 `redisAtomic`、`atomicGet`、`atomicSet`、
`atomicIncr` 等包装宏；Redis 7.0 起改为**直接使用 C11 `_Atomic`**，并删除了上述
包装。`numa_configurable_strategy.c` 使用了：

```c
redisAtomic int *allocation_counters;
atomicIncr(g_runtime_state.allocation_counters[node], 1);
atomicGet(g_runtime_state.pressure_weights[i], w[i]);
```

迁移方式（二选一）：

1. 改用 C11 原子（推荐，纯 C11 更符合本项目硬性标准）：
   ```c
   _Atomic int *allocation_counters;
   atomic_fetch_add_explicit(&allocation_counters[node], 1, memory_order_relaxed);
   w[i] = atomic_load_explicit(&pressure_weights[i], memory_order_relaxed);
   ```
2. 使用 `src/redis8_compat.h` 中的兼容宏（见第 4 节）。

### 2.2 LRU 时钟

Redis 6.2：`server.lruclock` 字段 + `LRU_CLOCK()` 宏。
Redis 7.0+：字段删除，统一调用 `getLRUClock()`。

`numa_composite_lru.c` 中 `uint16_t last_access` 存的是 LRU_CLOCK 低 16 位，
迁移时把对 `server.lruclock` / `LRU_CLOCK()` 的读取替换为 `getLRUClock()`。

### 2.3 `evictionPoolEntry` 结构变更

Redis 7.0 重写了 `evict.c` 与 `evictionPoolEntry`（新增 `dbid`、`cached` 字段，
并改变了 `idle` 语义）。本项目的 `evict.h` 在条目末尾追加了：

```c
int current_node;
size_t object_size;
int numa_migrated;
```

迁移时需对照 Redis 8 的 `evict.c` 中 `EVPOOL_CACHED_SDS_SIZE` / 结构布局，
把 NUMA 扩展字段追加到最新结构末尾，并同步 `evictionPoolPopulate` 的填充逻辑。

### 2.4 配置系统（`config.c`）

Redis 7.0 将 `config.c` 改为**声明式配置注册**（`createIntConfig` /
`createBoolConfig` / `createSizeTConfig` 等）。本项目在 `redis.conf`（1051–1071 行
的 `numa-demote-*`、2092–2104 行的 `numa-enabled`/`numa-migrate-config`）注册的
配置项，需要改为在 `config.c` 的 `configs[]` 表中用新 API 声明，并保留 `NUMA`
命令下的运行时开关。

### 2.5 `dict` 迭代器

`dictGetIterator` 在 Redis 7.0 被标记为不推荐（可用 `dictGetSafeIterator`），但
函数仍保留；`dictFind` / `dictGetVal` / `dictGetKey` / `dictNext` 签名不变。
建议统一改为 `dictGetSafeIterator`。

### 2.6 `serverLog` / `_serverLog`

`numa_bw_monitor.c` 使用了 `extern void _serverLog(...)`（Redis 内部惯例）。
Redis 8 中 `serverLog` 仍以 `void serverLog(int level, const char *fmt, ...)` 存在；
建议直接在 `server.h` 可见的模块里使用 `serverLog`，对独立模块保留一个
`#define serverLog(...) _serverLog(__VA_ARGS__)` 的兼容声明。

### 2.7 其它

- `listpack`（Redis 7.0 全面替换 `ziplist`）：`numa_key_migrate.c` 的 HASH/ZSET/SET
  适配器若直接操作 `ziplist` 内部结构，需改用 `listpack` 迭代 API。
- `addReply*` 系列（`numa_command.c`）：Redis 8 的回复 API 与 6.2 基本兼容，
  但部分函数（如 `addReplyBulkCBuffer`）有签名微调，需逐处核对。
- `aeEventLoop` / `aeCreateTimeEvent`：`numa_strategy_slots.c` 的 AE 调度接口在
  Redis 8 中保持兼容。

## 3. 推荐迁移顺序（自底向上）

```text
1. numa_pool / numa_migrate        （无依赖，先迁）
2. numa_key_migrate                （dict/listpack 适配器）
3. numa_configurable_strategy      （atomicvar.h 重构）
4. numa_composite_lru / numa_tinylfu（LRU clock + dict 迭代）
5. numa_strategy_slots / numa_command
6. evict_numa                      （evictionPoolEntry 同步）
7. server.c / server.h / config.c / evict.h 集成点
8. redis.conf 配置注册
```

## 4. `src/redis8_compat.h`（兼容头）

见 `src/redis8_compat.h`。它在 `REDIS_VERSION_NUM >= 0x00070000` 时提供以下兼容：

- 原子操作：`redisAtomic`、`atomicGet`、`atomicSet`、`atomicIncr` 映射到 C11 原子。
- LRU 时钟：`NUMA_LRU_CLOCK()` 统一为 `getLRUClock()`。
- `dictGetIterator` 安全别名。
- `serverLog` 的 `_serverLog` 别名。

## 5. 迁移后的新增能力（本仓库本次交付）

迁移到 Redis 8 的同时，本仓库新增了独立于 Redis 内核的 **NUMAflow** 子系统，
把上述策略拆分为 36 个可流程化执行的原子操作，并提供 N8N 风格 GUI / TUI、
公平评测框架与轻量追踪反馈。详见 `docs/numaflow/README.md`。

## 6. 验证清单（需在 Linux + libnuma 环境执行）

```bash
# 1. 准备环境（Debian/Ubuntu）
sudo apt-get install -y build-essential tcl libnuma-dev pkg-config

# 2. 编译（强制 MALLOC=libc，见 src/Makefile 第 103-110 行）
cd src && make clean && make -j$(nproc)

# 3. 跑标准测试与 NUMA 功能测试
make test
../tests/ycsb/run_ycsb.sh

# 4. 迁移后的 NUMAflow 子系统（纯 C11，任何平台可编）
cd ../numaflow && make test && make report
```
