# 09-numa-command.md - NUMA统一命令入口模块

## 模块概述

**状态**: ✅ **已实现** - 三层路由架构完成，统一替代原 NUMAMIGRATE / NUMACONFIG 分散命令

**文件**: [src/numa_command.c](file:///home/xdjtomato/下载/Redis with CXL/redis-CXL in v6.2.21/src/numa_command.c)

**已实现功能**:
- ✅ 顶层命令 `NUMA`，统一入口，参数解析与业务逻辑彻底分离
- ✅ `NUMA MIGRATE` 域：Key 级别迁移指令路由（原 NUMAMIGRATE）
- ✅ `NUMA CONFIG` 域：内存分配策略 + composite-lru JSON 热加载（原 NUMACONFIG + NUMAMIGRATE CONFIG）
- ✅ `NUMA STRATEGY` 域：策略插槽管理（新增）
- ✅ `NUMA HELP`：内联帮助信息

**功能**: 将所有 NUMA 相关 Redis 命令集中到单一命令 `NUMA` 下，通过域（Domain）隔离职责边界，业务逻辑仍保留在各自模块（`numa_key_migrate.c`、`numa_configurable_strategy.c`、`numa_strategy_slots.c`、`numa_composite_lru.c`），本文件只负责参数解析和 `addReply*`。

**依赖关系**:
- Key 级别迁移：[05-numa-key-migrate.md](./05-numa-key-migrate.md)（`numa_key_migrate.h/c`）
- 策略插槽框架：[06-numa-strategy-slots.md](./06-numa-strategy-slots.md)（`numa_strategy_slots.h/c`）
- 复合LRU策略：[07-numa-composite-lru.md](./07-numa-composite-lru.md)（`numa_composite_lru.h/c`）
- 可配置分配策略：[08-numa-configurable-strategy.md](./08-numa-configurable-strategy.md)（`numa_configurable_strategy.h/c`）

---

## 架构

### 三层路由结构

```
Redis 客户端
    │
    ▼
numaCommand(client *c)           ← 顶层入口，域路由
    ├── "MIGRATE"  → numa_cmd_migrate(c)   ← MIGRATE 域
    ├── "CONFIG"   → numa_cmd_config(c)    ← CONFIG 域
    ├── "STRATEGY" → numa_cmd_strategy(c)  ← STRATEGY 域
    └── "HELP"     → numa_cmd_help(c)      ← 内联帮助
```

### 各域子命令路由

```
numa_cmd_migrate
    ├── KEY   → numa_migrate_single_key()
    ├── DB    → numa_migrate_entire_database()
    ├── SCAN  → composite_lru_scan_once()       ← 触发一轮渐进式扫描
    ├── STATS → numa_get_migration_statistics()
    ├── RESET → numa_reset_migration_statistics()
    └── INFO  → numa_get_key_metadata() + lookupKeyRead()

numa_cmd_config
    ├── GET       → numa_config_get_current()
    ├── SET       → numa_config_set_strategy() / set_node_weights() / ...
    ├── LOAD      → composite_lru_load_config() + composite_lru_apply_config()
    ├── REBALANCE → numa_config_trigger_rebalance()
    └── STATS     → numa_config_get_statistics()

numa_cmd_strategy
    ├── SLOT → numa_strategy_slot_insert()
    └── LIST → numa_strategy_slot_list()
```

---

## 业务逻辑

### MIGRATE 域触发链

```
NUMA MIGRATE KEY <key> <node>
    │
    ├── 检查模块初始化: numa_key_migrate_is_initialized()
    ├── 解析目标节点范围: numa_max_node()
    ├── 执行迁移: numa_migrate_single_key(db, key, node)
    └── 返回结果码: NUMA_KEY_MIGRATE_OK / ENOENT / ENOMEM / ETYPE

NUMA MIGRATE SCAN [COUNT n]
    │
    ├── 获取策略插槽1: numa_strategy_slot_get(1)
    ├── 读取默认批量: composite_lru_data_t.config.scan_batch_size
    └── 执行一轮扫描: composite_lru_scan_once(strat, batch, &scanned, &migrated)
```

### CONFIG LOAD 热重载链

```
NUMA CONFIG LOAD [/path/to/config.json]
    │
    ├── 获取路径: argv[3] 或 server.numa_migrate_config_file
    ├── 获取策略插槽1: numa_strategy_slot_get(1)
    ├── 加载 JSON: composite_lru_load_config(path, &cfg)
    ├── 应用配置: composite_lru_apply_config(strat, &cfg)
    └── 记录日志: serverLog(LL_NOTICE, "[NUMA] composite-lru config hot-reloaded ...")
```

---

## 命令参考

### NUMA MIGRATE

| 子命令 | 语法 | 说明 |
|--------|------|------|
| `KEY` | `NUMA MIGRATE KEY <key> <node>` | 迁移指定 key 到目标 NUMA 节点 |
| `DB` | `NUMA MIGRATE DB <node>` | 迁移整个数据库到目标节点 |
| `SCAN` | `NUMA MIGRATE SCAN [COUNT n]` | 触发一轮渐进式 Key 扫描与迁移 |
| `STATS` | `NUMA MIGRATE STATS` | 查看迁移统计信息 |
| `RESET` | `NUMA MIGRATE RESET` | 重置迁移统计计数 |
| `INFO` | `NUMA MIGRATE INFO <key>` | 查看指定 key 的 NUMA 元数据 |

**NUMA MIGRATE STATS 返回字段**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `total_migrations` | integer | 累计迁移次数 |
| `successful_migrations` | integer | 成功迁移次数 |
| `failed_migrations` | integer | 失败迁移次数 |
| `total_bytes_migrated` | integer | 累计迁移字节数 |
| `total_migration_time_us` | integer | 累计迁移耗时（微秒） |
| `peak_concurrent_migrations` | integer | 历史最高并发迁移数 |

**NUMA MIGRATE INFO <key> 返回字段**:

| 字段 | 类型 | 说明 |
|------|------|------|
| `type` | string | Key 的 Redis 数据类型 |
| `current_node` | integer | Key 当前所在 NUMA 节点（无元数据时为 -1） |
| `hotness_level` | integer | 热度等级（0-7） |
| `access_count` | integer | 累计访问次数 |
| `numa_nodes_available` | integer | 系统可用 NUMA 节点总数 |
| `current_cpu_node` | integer | 当前 CPU 所在 NUMA 节点 |

---

### NUMA CONFIG

| 子命令 | 语法 | 说明 |
|--------|------|------|
| `GET` | `NUMA CONFIG GET` | 查看当前分配器配置 |
| `SET strategy` | `NUMA CONFIG SET strategy <name>` | 切换分配策略（见策略列表）|
| `SET weight` | `NUMA CONFIG SET weight <node> <w>` | 设置节点权重（0-1000）|
| `SET cxl_optimization` | `NUMA CONFIG SET cxl_optimization <on\|off>` | 启用/禁用 CXL 优化 |
| `SET balance_threshold` | `NUMA CONFIG SET balance_threshold <pct>` | 设置负载均衡阈值（0-100）|
| `LOAD` | `NUMA CONFIG LOAD [/path]` | 热加载 composite-lru JSON 配置文件 |
| `REBALANCE` | `NUMA CONFIG REBALANCE` | 手动触发负载重新均衡 |
| `STATS` | `NUMA CONFIG STATS` | 查看各节点内存分配统计 |

---

### NUMA STRATEGY

| 子命令 | 语法 | 说明 |
|--------|------|------|
| `SLOT` | `NUMA STRATEGY SLOT <slot_id> <strategy_name>` | 向指定插槽插入策略 |
| `LIST` | `NUMA STRATEGY LIST` | 列出所有插槽当前状态 |

---

### NUMA HELP

```bash
NUMA HELP    # 返回所有子命令简要说明
```

---

## 使用示例

```bash
# === MIGRATE 域 ===

# 迁移单个 key 到节点 1
NUMA MIGRATE KEY mykey 1

# 迁移整个数据库到节点 0
NUMA MIGRATE DB 0

# 触发一轮渐进式扫描（使用默认批量大小）
NUMA MIGRATE SCAN

# 触发一轮渐进式扫描（指定批量 500 个 key）
NUMA MIGRATE SCAN COUNT 500

# 查看迁移统计
NUMA MIGRATE STATS

# 查看某个 key 的 NUMA 元数据
NUMA MIGRATE INFO user:1001

# 重置统计计数
NUMA MIGRATE RESET

# === CONFIG 域 ===

# 查看当前分配器配置
NUMA CONFIG GET

# 切换至 CXL 优化模式
NUMA CONFIG SET strategy cxl_optimized

# 设置节点0权重为 120
NUMA CONFIG SET weight 0 120

# 热加载 composite-lru 配置（路径来自 redis.conf 的 numa_migrate_config_file）
NUMA CONFIG LOAD

# 指定路径热加载
NUMA CONFIG LOAD /etc/redis/composite_lru.json

# 手动触发重新平衡
NUMA CONFIG REBALANCE

# 查看节点分配统计
NUMA CONFIG STATS

# === STRATEGY 域 ===

# 向插槽2插入自定义策略
NUMA STRATEGY SLOT 2 custom_lru

# 列出所有策略插槽
NUMA STRATEGY LIST
```

---

## 接口

### 注册到 Redis 命令表

```c
/* server.c */
{"numa", numaCommand, -2, "admin write", 0, NULL, 0, 0, 0, 0, 0, 0},
```

- `arity -2`：至少需要 2 个参数（`NUMA` + 域名）
- `flags "admin write"`：需管理员权限，属于写操作

### 入口函数

```c
/* server.h */
void numaCommand(client *c);
```

```c
/* numa_command.c */
void numaCommand(client *c) {
    /* 域路由 */
    if (!strcasecmp(domain, "MIGRATE"))       numa_cmd_migrate(c);
    else if (!strcasecmp(domain, "CONFIG"))   numa_cmd_config(c);
    else if (!strcasecmp(domain, "STRATEGY")) numa_cmd_strategy(c);
    else if (!strcasecmp(domain, "HELP"))     numa_cmd_help(c);
    else addReplyErrorFormat(c, "Unknown NUMA domain '%s'. Try NUMA HELP.", domain);
}
```

### 模块初始化查询接口

```c
/* numa_key_migrate.h（由 numa_command.c 调用） */
int numa_key_migrate_is_initialized(void);
```

在执行 `NUMA MIGRATE *` 子命令前，本模块通过该接口确认 Key 迁移子系统已完成初始化，避免访问未初始化的全局上下文。

---

## 错误处理

| 错误场景 | 返回信息 |
|---------|---------|
| 缺少域参数 | `ERR Usage: NUMA <MIGRATE\|CONFIG\|STRATEGY\|HELP> [args...]` |
| 未知域 | `ERR Unknown NUMA domain '<name>'. Try NUMA HELP.` |
| 迁移模块未初始化 | `ERR NUMA Key Migrate module not initialized` |
| Key 不存在 | `ERR Key not found` |
| 目标节点越界 | `ERR Target node <n> out of range (0-<max>)` |
| 无活跃策略 | `ERR No active strategy on slot 1` |
| 配置文件加载失败 | `ERR Failed to load config from: <path>` |
| 未知子命令 | `ERR Unknown NUMA <DOMAIN> subcommand '<name>'` |

---

## 设计说明

### 职责边界

本模块严格遵循"参数解析与业务逻辑分离"原则：

- **本文件负责**：`argv` 解析、参数校验、`addReply*` 响应构造、日志记录
- **业务模块负责**：迁移执行、策略决策、配置管理、统计维护

### 与旧命令的关系

| 旧命令 | 新命令 | 变更说明 |
|--------|--------|---------|
| `NUMAMIGRATE KEY <k> <n>` | `NUMA MIGRATE KEY <k> <n>` | 直接替换 |
| `NUMAMIGRATE DB <n>` | `NUMA MIGRATE DB <n>` | 直接替换 |
| `NUMAMIGRATE STATS` | `NUMA MIGRATE STATS` | 直接替换 |
| `NUMAMIGRATE SCAN` | `NUMA MIGRATE SCAN` | 直接替换 |
| `NUMACONFIG GET` | `NUMA CONFIG GET` | 直接替换 |
| `NUMACONFIG SET ...` | `NUMA CONFIG SET ...` | 直接替换 |
| `NUMACONFIG REBALANCE` | `NUMA CONFIG REBALANCE` | 直接替换 |
| `NUMACONFIG STATS` | `NUMA CONFIG STATS` | 直接替换 |
| `NUMAMIGRATE CONFIG LOAD` | `NUMA CONFIG LOAD` | 域整合 |
| —（新增）— | `NUMA STRATEGY SLOT/LIST` | 新增策略插槽管理命令 |
