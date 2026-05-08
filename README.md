# Redis-NUMA: NUMA-Aware Redis with CXL Memory Tiering

[![License](https://img.shields.io/badge/License-BSD%203--Clause-blue.svg)](https://opensource.org/licenses/BSD-3-Clause)
[![Redis Version](https://img.shields.io/badge/Redis-6.2.21-red.svg)](https://redis.io/)
[![NUMA](https://img.shields.io/badge/NUMA-Optimized-green.svg)](https://en.wikipedia.org/wiki/Non-uniform_memory_access)

基于 Redis 6.2.21 的 NUMA 感知内存管理系统，支持 CXL（Compute Express Link）内存扩展。在保持 Redis 原有 API 完全兼容的前提下，实现了 NUMA 感知分配、智能热度追踪和跨节点数据迁移。

> **原版 Redis 文档**: [REDIS_ORIGINAL_README.md](docs/original/REDIS_ORIGINAL_README.md)

---

## 目录

- [项目概述](#项目概述)
- [核心模块](#核心模块)
- [架构设计](#架构设计)
- [快速开始](#快速开始)
- [命令接口](#命令接口)
- [性能数据](#性能数据)
- [详细文档](#详细文档)
- [测试](#测试)

---

## 项目概述

### 解决的核心问题

在传统 NUMA / CXL 系统中，Redis 内存分配无法感知 CPU 和内存的拓扑关系：
- **远程内存访问延迟高**：Key 可能分配在远程 NUMA 节点，访问延迟增加 20-40%
- **内存碎片率高**：频繁的小对象分配导致内存碎片化严重
- **无法利用 CXL 扩展内存**：无法将冷数据迁移至 CXL 内存以节省 DRAM 成本

### 解决方案

| 能力 | 实现 |
|------|------|
| NUMA 感知分配 | 所有 `zmalloc` 调用自动选择最优 NUMA 节点 |
| 两层分配器 | Slab (≤4KB, 原子 CAS 无锁) + Direct (>4KB) |
| 热度追踪 | 16 字节 PREFIX 内联元数据，O(1) 读写 |
| 双通道迁移 | 快速通道（候选池）+ 兜底通道（渐进扫描） |
| 压力感知分配 | WEIGHTED_INTERLEAVE 策略，无锁分配路径 |
| CXL 分层 | 热 Key 拉回 DRAM，冷 Key 推到 CXL |

---

## 核心模块

### NUMA Slab 分配器 (`numa_pool.c/h`)

纯 Slab 架构，24 级 jemalloc 风格 size class：

| 特性 | 参数 |
|------|------|
| Size class | 24 级 (16B, 32B, 48B, 64B, 80B, 96B, 112B, 128B, 192B, 256B, 320B, 384B, 512B, 640B, 768B, 1024B, 1280B, 1536B, 2048B, 2560B, 3072B, 3584B, 4096B) |
| Slab 大小 | 64KB |
| 位图管理 | 3072 bit (96 字节) |
| 分配方式 | 原子 CAS 无锁 |
| 碎片率 | 1.04 - 1.17 |

```
分配路径:
  size ≤ 4KB → numa_slab_alloc() → 原子 CAS 位图分配
  size > 4KB → numa_alloc_onnode() → 系统调用直接分配
```

### 可配置策略框架 (`numa_configurable_strategy.c/h`)

10 种分配策略，默认 WEIGHTED_INTERLEAVE：

| 策略 | 锁 | 说明 |
|------|-----|------|
| `local_first` | 无锁 | 固定返回 node 0 |
| `interleaved` | 无锁 | rand_r 随机 |
| `round_robin` | 无锁 | thread-local 计数器 |
| `weighted` | 短锁 | 持锁复制权重数组 |
| `pressure_aware` | 无锁 | 选择利用率最低的节点 |
| `cxl_optimized` | 无锁 | 小对象本地、大对象远端 |
| **`weighted_interleave`** | **无锁** | **默认策略**，atomicGet 读压力权重 |
| `adaptive` | - | 待实现 |
| `latency_aware` | - | 待实现 |

权重更新机制：
```
serverCron (每秒)                    分配路径 (每次 zmalloc)
       │                                    │
       ▼                                    ▼
numaGetNodePressure()               atomicGet(pressure_weights[i])
       │                                    │
       ▼                                    ▼
weight = (1 - pressure) * 100       加权随机选择节点
       │
       ▼
atomicSet(pressure_weights[i], w)
```

### Composite LRU 策略 (`numa_composite_lru.c/h`)

默认迁移策略（Slot 1），双通道架构：

- **快速通道**：候选池环形缓冲区，热度首次越过阈值时写入，每秒处理
- **兜底通道**：渐进扫描 key_heat_map，覆盖所有 Key，支持冷 Key 推出

```
Key 访问 → lookupKey → composite_lru_record_access
  │
  ├── 阶梯式惰性衰减 (< 10s: 0, < 60s: -1, < 5m: -2, < 30m: -3, ≥ 30m: 清零)
  ├── 热度 +1 (上限 7)
  ├── 写回 PREFIX (hotness, access_count, last_access)
  └── 热度越过阈值 且 Key 在远程 → 写入候选池
```

可配置参数（JSON 热加载）：

| 参数 | 默认值 | 说明 |
|------|--------|------|
| `migrate_hotness_threshold` | 3 | 触发迁移的热度阈值 |
| `hot_candidates_size` | 1024 | 候选池容量 |
| `scan_batch_size` | 500 | 每次扫描 Key 数 |
| `overload_threshold` | 0.8 | 节点内存过载阈值 |
| `bandwidth_threshold` | 0.9 | 带宽饱和阈值 |
| `pressure_threshold` | 0.7 | 迁移压力阈值 |

### Key 级别迁移 (`numa_key_migrate.c/h`)

以 `robj` 为迁移单元，5 种数据类型适配器：

| 类型 | 适配器 | 实现状态 |
|------|--------|---------|
| STRING | `migrate_string_type()` | 完整实现 |
| HASH | `migrate_hash_type()` | Stub |
| LIST | `migrate_list_type()` | Stub |
| SET | `migrate_set_type()` | Stub |
| ZSET | `migrate_zset_type()` | Stub |

迁移流程：目标节点 `numa_zmalloc_onnode` 分配 → `memcpy` 复制 → 原子指针切换 → 释放旧内存

### 策略插槽框架 (`numa_strategy_slots.c/h`)

16 个插槽，工厂模式 + 虚函数表，按优先级调度（HIGH → NORMAL → LOW）：

```
Slot 0:  Noop (LOW)           — 兜底策略
Slot 1:  Composite LRU (HIGH) — 默认迁移策略
Slot 2-15: 空闲               — 可插入自定义策略
```

### PREFIX 元数据 (`zmalloc.c/h`)

每次 `zmalloc` 分配在对象头部内联 16 字节元数据：

```c
typedef struct {
    size_t size;           // 8B - 实际分配大小
    char from_slab;        // 1B - 来源 (0=Direct, 1=Slab)
    char node_id;          // 1B - NUMA 节点 ID
    uint8_t hotness;       // 1B - 热度 (0-7)
    uint8_t access_count;  // 1B - 访问计数
    uint16_t last_access;  // 2B - LRU 时钟低 16 位
    char reserved[2];      // 2B - 对齐
} numa_alloc_prefix_t;     // 16B
```

---

## 架构设计

```
┌─────────────────────────────────────────────────────────────┐
│                        Redis Core                           │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌────────────┐  │
│  │ server.c │  │  db.c    │  │ config.c │  │ zmalloc.c  │  │
│  └────┬─────┘  └────┬─────┘  └────┬─────┘  └─────┬──────┘  │
└───────┼──────────────┼─────────────┼───────────────┼─────────┘
        │              │             │               │
┌───────┼──────────────┼─────────────┼───────────────┼─────────┐
│       ▼              ▼             ▼               ▼         │
│  ┌──────────────────────────────────────────────────────┐   │
│  │              NUMA 模块层                              │   │
│  │                                                      │   │
│  │  ┌────────────────────────────────────────────────┐  │   │
│  │  │          统一命令接口 (numa_command.c)          │  │   │
│  │  │     MIGRATE │ CONFIG │ STRATEGY │ HELP         │  │   │
│  │  └────────────────────────────────────────────────┘  │   │
│  │                                                      │   │
│  │  ┌─────────────────────┐  ┌───────────────────────┐  │   │
│  │  │  策略插槽框架        │  │  可配置策略框架        │  │   │
│  │  │  (16 slots)         │  │  (10 种分配策略)      │  │   │
│  │  │  Slot 0: Noop       │  │  默认: WEIGHTED_      │  │   │
│  │  │  Slot 1: C-LRU      │  │  INTERLEAVE (无锁)   │  │   │
│  │  └─────────────────────┘  └───────────────────────┘  │   │
│  │                                                      │   │
│  │  ┌─────────────────────┐  ┌───────────────────────┐  │   │
│  │  │ Composite LRU       │  │ NUMA Slab 分配器      │  │   │
│  │  │ ├─ 快速通道(候选池) │  │ ├─ Slab (≤4KB)       │  │   │
│  │  │ └─ 兜底通道(扫描)   │  │ └─ Direct (>4KB)     │  │   │
│  │  └─────────────────────┘  └───────────────────────┘  │   │
│  │                                                      │   │
│  │  ┌─────────────────────┐  ┌───────────────────────┐  │   │
│  │  │ Key 迁移            │  │ 带宽监控              │  │   │
│  │  │ (5 类型适配器)      │  │ (resctrl/numastat)   │  │   │
│  │  └─────────────────────┘  └───────────────────────┘  │   │
│  └──────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────┘
        │                                      │
        ▼                                      ▼
┌───────────────┐                      ┌───────────────┐
│  NUMA 节点 0  │                      │  NUMA 节点 1  │
│  (DRAM)       │◄──── 跨节点迁移 ────►│  (CXL)        │
└───────────────┘                      └───────────────┘
```

### 数据流

```
写路径:  SET → zmalloc → numa_config_get_best_node() → Slab/Direct → PREFIX → DB
读路径:  GET → lookupKey → composite_lru_record_access() → 更新 PREFIX → 候选池
迁移:    serverCron → Composite LRU → 候选 Key → 类型适配器 → 指针切换 → 释放
```

---

## 快速开始

### 系统要求

- Linux (支持 NUMA)
- GCC 4.8+ 或 Clang
- `libnuma-dev` (Debian/Ubuntu) 或 `numactl-devel` (CentOS/RHEL)

### 编译

```bash
cd src
make clean && make -j$(nproc)
```

编译强制 `MALLOC=libc` 并链接 `-lnuma`。jemalloc 与 NUMA 分配器不兼容。

### 运行

```bash
# 启动
./src/redis-server redis.conf

# 验证 NUMA 支持
redis-cli NUMA CONFIG GET

# 加载迁移策略配置
redis-cli NUMA CONFIG LOAD /path/to/composite_lru.json
```

### redis.conf 配置

```conf
# 启用 NUMA 支持
numa-enabled yes

# 指定 Composite LRU 配置文件
numa-migrate-config /path/to/composite_lru.json
```

### composite_lru.json 示例

```json
{
    "migrate_hotness_threshold": 3,
    "hot_candidates_size": 1024,
    "scan_batch_size": 500,
    "decay_threshold_sec": 10,
    "auto_migrate_enabled": 1,
    "debug_logging_enabled": 0,
    "overload_threshold": 0.8,
    "bandwidth_threshold": 0.9,
    "pressure_threshold": 0.7,
    "stability_count": 3,
    "max_bandwidth_node0_mbps": 51000,
    "max_bandwidth_node1_mbps": 12000
}
```

---

## 命令接口

所有 NUMA 操作通过统一的 `NUMA` 命令：

### NUMA MIGRATE

```bash
# 迁移单个 Key 到节点 1
NUMA MIGRATE KEY user:100 1

# 迁移整个数据库
NUMA MIGRATE DB 1

# 手动触发渐进扫描
NUMA MIGRATE SCAN COUNT 500

# 查看迁移统计
NUMA MIGRATE STATS

# 重置统计
NUMA MIGRATE RESET

# 查看 Key 的 NUMA 元数据
NUMA MIGRATE INFO user:100
```

### NUMA CONFIG

```bash
# 查看当前配置
NUMA CONFIG GET

# 设置分配策略
NUMA CONFIG SET strategy weighted_interleave

# 设置节点权重（WEIGHTED 策略）
NUMA CONFIG SET weight 0 80

# 启用 CXL 优化
NUMA CONFIG SET cxl_optimization on

# 加载 JSON 配置
NUMA CONFIG LOAD /path/to/composite_lru.json

# 查看分配统计
NUMA CONFIG STATS

# 手动触发重新平衡
NUMA CONFIG REBALANCE
```

### NUMA STRATEGY

```bash
# 列出所有策略插槽
NUMA STRATEGY LIST

# 将策略插入指定插槽
NUMA STRATEGY SLOT 2 my-strategy
```

---

## 性能数据

### QEMU 双节点环境

| 指标 | 数值 | 说明 |
|------|------|------|
| Phase 2 吞吐 | ~53K ops/s | WEIGHTED_INTERLEAVE 策略 |
| Phase 3 吞吐 | ~45K ops/s | 持续迁移负载 |
| 迁移速率 | ~1,524/sec | 恒定速率 |
| 内存碎片率 | 1.04-1.17 | Slab + PREFIX |

### 分配路径性能

| 路径 | 锁 | 复杂度 |
|------|-----|--------|
| Slab (≤4KB) | 无锁（原子 CAS） | O(1) |
| Direct (>4KB) | 系统调用 | - |
| 节点选择 | 无锁（atomicGet） | O(n) n=节点数 |
| 热度追踪 | 单线程（无需同步） | O(1) |

---

## 详细文档

`docs/new/` 目录包含完整的模块设计文档：

| 文档 | 内容 |
|------|------|
| [00-design-proposal](docs/new/00-design-proposal.md) | 项目方案设计 |
| [01-overview](docs/new/01-overview.md) | 架构概览与核心模块 |
| [02-numa-pool](docs/new/02-numa-pool.md) | NUMA Slab 分配器 |
| [03-zmalloc-numa](docs/new/03-zmalloc-numa.md) | zmalloc 适配与 PREFIX 元数据 |
| [04-numa-migrate](docs/new/04-numa-migrate.md) | 块级内存迁移 |
| [05-numa-strategy-slots](docs/new/05-numa-strategy-slots.md) | 策略插槽框架 |
| [06-numa-composite-lru](docs/new/06-numa-composite-lru.md) | Composite LRU 双通道迁移 |
| [07-numa-key-migrate](docs/new/07-numa-key-migrate.md) | Key 级别迁移 |
| [08-numa-configurable](docs/new/08-numa-configurable.md) | 可配置策略框架 |
| [09-numa-command](docs/new/09-numa-command.md) | 统一命令接口 |
| [10-call-chain](docs/new/10-call-chain.md) | 调用链与模块交互 |
| [11-alloc-path-instrumentation](docs/new/11-alloc-path-instrumentation.md) | 分配路径埋点 |
| [12-perf-root-cause-analysis](docs/new/12-perf-root-cause-analysis.md) | 性能根因分析 |
| [13-lockfree-alloc-design](docs/new/13-lockfree-alloc-design.md) | 无锁分配设计 |

### 源码结构

```
src/
├── numa_pool.c/h                  # Slab 分配器（24 级 size class, 64KB slab, 原子 CAS）
├── numa_migrate.c/h               # 块级内存迁移
├── numa_key_migrate.c/h           # Key 级别迁移（5 类型适配器）
├── numa_strategy_slots.c/h        # 策略插槽框架（16 槽, 优先级调度）
├── numa_composite_lru.c/h         # Composite LRU 策略（双通道, JSON 配置）
├── numa_configurable_strategy.c/h # 可配置分配策略（10 种, WEIGHTED_INTERLEAVE 默认）
├── numa_command.c                 # 统一命令接口（MIGRATE/CONFIG/STRATEGY）
├── numa_bw_monitor.c/h            # 带宽监控（resctrl/numastat）
├── evict_numa.c/h                 # NUMA 感知驱逐
├── zmalloc.c/h                    # 内存分配入口（PREFIX 元数据）
└── server.c                       # NUMA 初始化与 serverCron 集成
```

---

## 测试

### Redis 标准测试

```bash
cd src && make test
```

### NUMA 基准测试（YCSB）

```bash
# 三阶段测试：Fill → Hotspot → Sustain
cd tests/ycsb && ./run_bw_benchmark.sh

# YCSB 基线/压力模式
cd tests/ycsb && ./run_ycsb.sh
```

### NUMA 环境检查

```bash
./check_numa_config.sh
./diagnose_numa.sh
```

---

## 许可证

BSD 3-Clause License. 基于 Redis 6.2.21 开发。

---

## 项目状态

### 已完成

- [x] NUMA Slab 分配器（24 级 size class, 原子 CAS 无锁）
- [x] WEIGHTED_INTERLEAVE 默认分配策略（压力感知, 无锁）
- [x] Composite LRU 双通道迁移策略
- [x] 16 字节 PREFIX 内联元数据
- [x] 策略插槽框架（16 槽, 优先级调度）
- [x] STRING 类型迁移适配器
- [x] 统一 NUMA 命令接口
- [x] JSON 配置热加载
- [x] 带宽监控
- [x] 分配路径无锁化
- [x] NUMA 感知驱逐

### 待实现

- [ ] HASH/LIST/SET/ZSET 类型迁移适配器
- [ ] Adaptive 自适应策略
- [ ] Latency-Aware 延迟感知策略
- [ ] ML-based 迁移预测
