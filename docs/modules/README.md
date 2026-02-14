# Redis NUMA 模块文档

本文档详细介绍 Redis NUMA 改造项目的二次开发模块。

## 模块概览

```
┌─────────────────────────────────────────────────────────────┐
│                    Redis Server                              │
├─────────────────────────────────────────────────────────────┤
│  Application Layer                                           │
│  - Redis Objects (robj, sds, dict, etc.)                    │
│  - Data Structures (string, list, hash, set, zset)          │
└──────────────────────────┬──────────────────────────────────┘
                           │ zmalloc/zfree
                           ▼
┌─────────────────────────────────────────────────────────────┐
│  Memory Allocation Layer (P2优化: Slab+Pool双路径)           │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────┐  │
│  │ zmalloc.c   │  │ numa_pool   │  │ numa_slab.c         │  │
│  │ (NUMA适配)  │  │ (内存池)     │  │ (Slab分配器)        │  │
│  │             │  │ 256KB chunk │  │ 16KB slab           │  │
│  │ PREFIX机制   │  │ >128B对象   │  │ ≤128B对象           │  │
│  └──────┬──────┘  └──────┬──────┘  └──────────┬──────────┘  │
└─────────┼────────────────┼────────────────────┼─────────────┘
          │                │                    │
          ▼                ▼                    ▼
┌─────────────────────────────────────────────────────────────┐
│  System Layer                                                │
│  - libnuma (numa_alloc_onnode, numa_free)                   │
│  - libc (malloc, free, memcpy)                              │
└─────────────────────────────────────────────────────────────┘
```

## 模块列表

| 模块 | 文件 | 功能描述 | 状态 |
|------|------|----------|------|
| **NUMA内存池** | `numa_pool.h/c` | 节点粒度的内存池管理，256KB chunk | 已完成 v3.2-P2 |
| **Slab分配器** | `numa_pool.h/c` | 小对象快速分配，16KB slab，O(1) bitmap | 已完成 v3.2-P2 |
| **NUMA迁移** | `numa_migrate.h/c` | 内存对象跨节点迁移 | 已完成 v2.2 |
| **zmalloc NUMA适配** | `zmalloc.c/h` | 内存分配器NUMA感知改造，PREFIX机制 | 已完成 v3.2-P2 |

## 核心设计原则

### 1. 向后兼容
- 所有现有Redis代码无需修改即可运行
- zmalloc接口保持完全不变
- NUMA不可用时自动回退到标准分配器

### 2. 模块化架构
- 职责分离：分配、池管理、迁移各自独立
- 接口清晰：通过头文件定义模块边界
- 易于扩展：支持未来libNUMA功能增强

### 3. 性能优先
- 内存池减少系统调用，提升6倍性能
- O(1)时间复杂度的内存分配
- 无锁设计（per-pool锁减少竞争）

## 文档导航

### 核心模块文档
- [01-numa-pool.md](./01-numa-pool.md) - NUMA内存池模块详解
- [02-numa-migrate.md](./02-numa-migrate.md) - NUMA迁移模块详解
- [03-zmalloc-numa.md](./03-zmalloc-numa.md) - zmalloc NUMA适配详解
- [04-call-chain.md](./04-call-chain.md) - 业务调用链分析

### Key级别迁移与策略框架
- [05-numa-key-migrate.md](./05-numa-key-migrate.md) - Key级别迁移核心模块
- [06-numa-strategy-slots.md](./06-numa-strategy-slots.md) - 策略插槽框架
- [07-numa-composite-lru.md](./07-numa-composite-lru.md) - 复合LRU策略实现
- [08-numa-configurable-strategy.md](./08-numa-configurable-strategy.md) - 可配置NUMA分配策略

### 性能分析与优化
- [09-memory-fragmentation-analysis.md](./09-memory-fragmentation-analysis.md) - 内存碎片问题深度分析

## 版本历史

| 版本 | 日期 | 变更内容 |
|------|------|----------|
| v3.2-P2 | 2026-02-14 | P2性能修复：Slab对齐、bitmap O(1)、node_id追踪 |
| v3.1-P1 | 2026-02-14 | P1优化：Free List + Compact机制 |
| v3.0 | 2026-02-14 | P0优化：动态chunk + 16级size class |
| v2.5 | 2026-02-14 | 添加可配置NUMA分配策略，支持6种分配模式 |
| v2.4 | 2026-02-13 | Key级别迁移模块实现 |
| v2.3 | 2026-02-13 | 策略插槽框架完成 |
| v2.2 | 2026-02-13 | 添加迁移模块，模块化重构完成 |
| v2.1 | 2026-02-13 | 内存池模块化，分离为独立模块 |
| v2.0 | 2026-02-07 | 性能优化，166K req/s达成 |
| v1.0 | 2025-02 | 初始NUMA适配实现 |
