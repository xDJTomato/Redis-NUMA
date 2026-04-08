# Redis CXL 项目功能分析与负载设计文档

> **项目版本**: Redis 6.2.21 + NUMA/CXL 优化 (v3.2-P2)  
> **文档日期**: 2026-04-08  
> **文档目的**: 重启项目的功能梳理 + YCSB 高强度迁移负载设计

---

## 第一部分：项目功能点与模块实现

### 1. 项目概述

本项目是基于 Redis 6.2.21 的二次开发版本，核心目标是**在多节点 NUMA 架构上优化 Redis 的内存访问性能**，并为未来 CXL (Compute Express Link) 内存扩展提供支持。

#### 核心问题
- 原生 Redis 在 NUMA 服务器上随机分配内存，导致跨节点访问延迟高 (2-3倍)
- 缺乏热点数据感知和迁移机制
- 无法利用 CXL 扩展内存

#### 解决方案
1. **NUMA 感知内存分配器** - 优先本地节点分配
2. **智能内存迁移** - 动态迁移热点数据到访问频繁节点
3. **策略插槽框架** - 支持多种负载均衡策略
4. **热度追踪机制** - 基于 LRU 的轻量级热度管理

---

### 2. 核心模块详解

#### 2.1 NUMA 内存池模块 (numa_pool.c/h)

**功能定位**: 节点粒度的内存池管理，减少系统调用，提升分配效率

**架构设计**:
```
三级内存分配架构:
┌─────────────────────────────────────────┐
│  L1: Slab Allocator (≤128B)             │
│  - 4KB slab + 位图 O(1) 分配            │
│  - 无锁快速路径 (原子 CAS 操作)          │
│  - 按大小分类: 16B/32B/48B/64B/96B/128B│
└─────────────────────────────────────────┘
                    │
┌─────────────────────────────────────────┐
│  L2: Pool Allocator (128B - 4KB)        │
│  - 16级 size class (细粒度到4096B)      │
│  - 动态 chunk (16KB/64KB/256KB)         │
│  - Free List 复用 (O(1) 释放)           │
│  - Compact 机制 (清理低利用率 chunk)     │
└─────────────────────────────────────────┘
                    │
┌─────────────────────────────────────────┐
│  L3: Direct NUMA Allocation (>4KB)      │
│  - 直接调用 numa_alloc_onnode()         │
│  - 大对象专属路径                        │
└─────────────────────────────────────────┘
```

**关键数据结构**:

```c
// 16级大小分类
const size_t numa_pool_size_classes[16] = {
    16, 32, 48, 64,          // 细粒度小对象
    96, 128, 192, 256,       // 中小对象
    384, 512, 768, 1024,     // 中型对象
    1536, 2048, 3072, 4096   // 大型对象
};

// Pool Chunk 结构
struct numa_pool_chunk {
    void *memory;           // NUMA分配的内存基址
    size_t size;            // chunk总大小
    size_t offset;          // 当前分配偏移
    size_t used_bytes;      // 已使用字节数 (利用率跟踪)
    struct numa_pool_chunk *next;
};

// Slab 结构 (P2优化: 双向链表 + 原子操作)
typedef struct numa_slab {
    void *memory;                    // 对齐后的内存地址
    struct numa_slab *next, *prev;   // 双向链表
    uint32_t bitmap[4];              // 128位位图
    _Atomic uint16_t free_count;     // 空闲对象数 (原子)
    _Atomic int list_type;           // partial/full/empty
} numa_slab_t;
```

**实现思路**:
1. **Slab 快速路径** (≤128B):
   - 使用位图管理对象分配，`bitmap_find_and_set()` 实现无锁分配
   - CAS 原子操作保证并发安全
   - Slab 头部存储回指针，支持 O(1) free 查找

2. **Pool 常规路径** (128B-4KB):
   - 二分查找式 size class 分类 (O(log n))
   - 优先从 Free List 头部复用 (O(1))
   - 失败时尝试第一个 chunk 的顺序分配
   - 仍失败则分配新 chunk

3. **Compact 机制**:
   - 遍历所有 chunk，计算利用率
   - 释放利用率 <30% 的 chunk
   - 清理空闲块数量 >10 的 free list

**性能成果**:
- 碎片率: 3.61 → **1.02** (降低 72%)
- 内存效率: 27% → **98%** (提升 263%)
- SET 性能: **96K req/s**

---

#### 2.2 NUMA 感知分配器 (zmalloc.c)

**功能定位**: 拦截 Redis 所有内存分配，路由到 NUMA 优化路径

**核心机制**:

```c
// 16字节 PREFIX 元数据结构
typedef struct {
    size_t size;           // 8B: 实际大小
    char from_pool;        // 1B: 0=直接, 1=Pool, 2=Slab
    char node_id;          // 1B: 分配节点ID
    uint8_t hotness;       // 1B: 热度级别 (0-7)
    uint8_t access_count;  // 1B: 访问计数
    uint16_t last_access;  // 2B: LRU时钟
    char reserved[2];      // 2B: 保留
} numa_alloc_prefix_t;
```

**分配流程**:
```
zmalloc(size)
  │
  ├─ NUMA可用? ──否──> jemalloc/libc
  │
  └─是
     │
     ├─ 单节点? ──是──> 节点0直接分配
     │
     └─多节点
        │
        ├─ 交错策略 (默认): rand_r() % num_nodes
        ├─ ≤128B? ──是──> numa_slab_alloc()
        └─ 否则 ────────> numa_pool_alloc()
           │
           └─ 失败? ──是──> numa_alloc_onnode() (直接)
```

**释放流程**:
```
zfree(ptr)
  │
  └─ 读取 PREFIX
     │
     ├─ from_pool == 2 (Slab)
     │   └─ numa_slab_free(ptr, size, node_id)
     │
     └─ from_pool == 1 (Pool)
         └─ numa_pool_free(ptr, size, 1)
```

**热度追踪 API**:
- `numa_get_hotness(ptr)` - 读取热度级别
- `numa_set_hotness(ptr, level)` - 设置热度
- `numa_increment_access_count(ptr)` - 递增访问计数
- `numa_get_node_id(ptr)` - 获取分配节点

---

#### 2.3 复合 LRU 迁移策略 (numa_composite_lru.c)

**功能定位**: 智能识别热点 Key 并触发跨 NUMA 节点迁移

**双通道架构**:
```
┌─────────────────────────────────────────────┐
│  快速通道 (Fast Path)                        │
│  - 访问时写入候选池 (环形缓冲 256 条目)       │
│  - 条件: 热度越过阈值 AND 内存在远程节点       │
│  - serverCron 优先处理                       │
└─────────────────────────────────────────────┘
              │
┌─────────────────────────────────────────────┐
│  兜底通道 (Fallback Path)                    │
│  - serverCron 渐进扫描 key_heat_map          │
│  - 每次 scan_batch_size 个 key (默认200)     │
│  - 扫描到末尾后重置，循环扫描                 │
└─────────────────────────────────────────────┘
```

**热度管理**:
```c
// 阶梯式惰性衰减
uint8_t compute_lazy_decay_steps(uint16_t elapsed_secs) {
    if (elapsed_secs < 10s)   return 0;  // 不衰减
    if (elapsed_secs < 60s)   return 1;  // 衰减1级
    if (elapsed_secs < 5min)  return 2;  // 衰减2级
    if (elapsed_secs < 30min) return 3;  // 衰减3级
    return 7;  // 全清除
}

// 访问记录
void composite_lru_record_access(strategy, key, val) {
    1. 计算时间差，执行惰性衰减
    2. hotness++ (上限7)
    3. 更新 access_count 和 last_access
    4. 若 hotness_before < threshold <= hotness_after
       AND mem_node != cpu_node:
       写入候选池
}
```

**迁移决策**:
```c
int composite_lru_execute(strategy) {
    // 快速通道
    for each candidate in pool:
        cur_hotness = numa_get_hotness(val);  // 重读
        mem_node = numa_get_node_id(val);
        if (cur_hotness >= threshold && mem_node != target_node):
            numa_migrate_single_key(key, target_node);
    
    // 兜底通道
    composite_lru_scan_once(batch_size);
}
```

**可配置参数** (JSON 热加载):
- `migrate_hotness_threshold`: 迁移热度阈值 (1-7, 默认5)
- `hot_candidates_size`: 候选池大小 (默认256)
- `scan_batch_size`: 扫描批次大小 (默认200)
- `decay_threshold_sec`: 衰减阈值 (默认10s)
- `auto_migrate_enabled`: 自动迁移开关

---

#### 2.4 Key 级别迁移模块 (numa_key_migrate.c)

**功能定位**: 支持 Redis 所有数据类型的 Key 级迁移

**支持的类型**:
| 类型 | 编码方式 | 迁移策略 |
|------|---------|---------|
| STRING | RAW/EMBSTR | SDS 重新分配 |
| HASH | ZIPLIST | 整体拷贝 |
| HASH | HT | 逐对重建 dict |
| LIST | QUICKLIST | 逐节点迁移 |
| SET | INTSET | 整体拷贝 |
| SET | HT | 逐元素重建 |
| ZSET | ZIPLIST | 整体拷贝 |
| ZSET | SKIPLIST | 重建跳表+dict |

**迁移流程**:
```
numa_migrate_single_key(db, key, target_node)
  │
  ├─ dictFind(db->dict, key)
  ├─ 识别 val->type
  ├─ 调用类型适配器
  │   ├─ migrate_string_type()
  │   ├─ migrate_hash_type()
  │   ├─ migrate_list_type()
  │   ├─ migrate_set_type()
  │   └─ migrate_zset_type()
  │
  └─ 更新元数据: meta->current_node = target_node
```

**元数据管理**:
```c
typedef struct {
    int current_node;           // 当前 NUMA 节点
    uint8_t hotness_level;      // 热度等级 (0-7)
    uint16_t last_access_time;  // LRU 时钟
    uint64_t access_count;      // 访问次数
} key_numa_metadata_t;
```

**生命周期管理**:
- `numa_record_key_access(key, val)` - 记录访问
- `numa_on_key_delete(key)` - 清理元数据 (防内存泄漏)
- `numa_perform_heat_decay()` - 定期热度衰减

---

#### 2.5 策略插槽框架 (numa_strategy_slots.c)

**功能定位**: 可扩展的策略注册和调度系统

**架构**:
```
策略管理器 (16个插槽)
├─ Slot 0: No-op 兜底策略 (默认启用)
├─ Slot 1: Composite LRU (默认启用)
├─ Slot 2-15: 自定义策略 (可扩展)
│
调度机制: serverCron 每秒调用 numa_strategy_run_all()
  └─ 按优先级执行: HIGH → NORMAL → LOW
```

**策略接口 (虚函数表)**:
```c
typedef struct {
    int (*init)(strategy);
    int (*execute)(strategy);
    void (*cleanup)(strategy);
    const char* (*get_name)(strategy);
    const char* (*get_description)(strategy);
    int (*set_config)(strategy, key, value);
    int (*get_config)(strategy, key, buf, buf_len);
} numa_strategy_vtable_t;
```

**工厂模式注册**:
```c
numa_strategy_factory_t factory = {
    .name = "my-strategy",
    .create = my_strategy_create,
    .destroy = my_strategy_destroy,
    ...
};
numa_strategy_register_factory(&factory);
numa_strategy_slot_insert(slot_id, "my-strategy");
```

---

#### 2.6 可配置分配策略 (numa_configurable_strategy.c)

**功能定位**: 运行时切换内存分配策略

**6种策略**:
| 策略 | 描述 | 适用场景 |
|------|------|---------|
| local_first | 固定节点0分配 | 单线程访问 |
| interleaved | 随机交错分配 | 负载均衡 (默认) |
| round_robin | 轮询分配 | 均匀分布 |
| weighted | 加权随机 | 异构节点 |
| pressure_aware | 选择负载最轻节点 | 动态均衡 |
| cxl_optimized | 小对象本地/大对象CXL | CXL环境 |

**运行时配置**:
```bash
redis-cli NUMA CONFIG GET
redis-cli NUMA CONFIG SET strategy round_robin
redis-cli NUMA CONFIG SET weight 0 80
redis-cli NUMA CONFIG SET weight 1 120
redis-cli NUMA CONFIG SET cxl_optimization on
redis-cli NUMA CONFIG STATS
redis-cli NUMA CONFIG REBALANCE
```

---

#### 2.7 统一命令接口 (numa_command.c)

**功能定位**: 所有 NUMA 操作的 Redis 命令入口

**命令树**:
```
NUMA
├─ MIGRATE
│   ├─ KEY <key> <node>       - 迁移单个Key
│   ├─ DB <node>              - 迁移整个数据库
│   ├─ SCAN [COUNT n]         - 触发渐进扫描
│   ├─ STATS                  - 查看迁移统计
│   ├─ RESET                  - 重置统计
│   └─ INFO <key>             - 查看Key元数据
│
├─ CONFIG
│   ├─ GET                    - 查看当前配置
│   ├─ SET <param> <val>      - 修改配置
│   ├─ LOAD [/path]           - 热加载JSON配置
│   ├─ REBALANCE              - 手动触发重平衡
│   └─ STATS                  - 查看分配统计
│
├─ STRATEGY
│   ├─ SLOT <id> <name>       - 插入策略到插槽
│   └─ LIST                   - 列出所有策略
│
└─ HELP                       - 显示帮助
```

---

### 3. 模块依赖关系

```
┌─────────────────────────────────────────────┐
│              Redis Core                      │
│         (server.c, db.c, networking.c)       │
└─────────────────┬───────────────────────────┘
                  │
    ┌─────────────┼─────────────┐
    │             │             │
    ▼             ▼             ▼
┌───────┐  ┌──────────┐  ┌──────────┐
│内存池  │  │策略插槽   │  │Key迁移    │
│Pool   │  │Strategy  │  │Migrate   │
└───┬───┘  └────┬─────┘  └────┬─────┘
    │           │              │
    │     ┌─────┴──────┐       │
    │     │ 复合LRU     │       │
    │     │Composite   │       │
    │     └─────┬──────┘       │
    │           │              │
    └───────────┼──────────────┘
                │
                ▼
┌─────────────────────────────────────────────┐
│          基础内存迁移 (numa_migrate)          │
└─────────────────────┬───────────────────────┘
                      │
                      ▼
┌─────────────────────────────────────────────┐
│           libnuma / OS 系统调用              │
└─────────────────────────────────────────────┘
```

---

### 4. 关键调用链

#### 4.1 内存分配调用链
```
zmalloc(size)
  └─> numa_alloc_with_size(size)          [zmalloc.c]
      ├─> 选择目标节点 (交错/轮询/加权)
      ├─> should_use_slab(size)?
      │   └─> numa_slab_alloc()           [numa_pool.c]
      │       └─> bitmap_find_and_set()   // 无锁分配
      │
      └─> numa_pool_alloc()               [numa_pool.c]
          ├─> Free List 查找 (O(1))
          ├─> Chunk 顺序分配
          └─> alloc_new_chunk()           // 慢速路径
              └─> numa_alloc_onnode()     // libnuma
```

#### 4.2 Key 访问与迁移调用链
```
lookupKey(db, key)  [db.c - 需添加Hook]
  └─> numa_record_key_access(key, val)   [numa_key_migrate.c]
      ├─> 惰性衰减热度
      ├─> hotness++
      └─> 越过阈值? ──是──> 写入候选池

serverCron()  [server.c - 每秒]
  └─> numa_strategy_run_all()            [numa_strategy_slots.c]
      └─> composite_lru_execute()        [numa_composite_lru.c]
          ├─> 处理候选池 (快速通道)
          │   └─> numa_migrate_single_key()  [numa_key_migrate.c]
          │       └─> migrate_string_type() 等
          │
          └─> composite_lru_scan_once()    (兜底通道)
              └─> 扫描 key_heat_map
```

---

### 5. 性能指标汇总

| 指标 | 数值 | 说明 |
|------|------|------|
| SET 吞吐量 | 169K-188K req/s | 单线程 |
| GET 吞吐量 | 188K req/s | 单线程 |
| p50 延迟 | 0.031ms | 原生 Redis 水平 |
| p99 延迟 | 0.071-0.079ms | |
| 内存碎片率 | 1.02 | 降低 72% |
| 内存效率 | 98% | 提升 263% |
| Slab 分配 | O(1) 无锁 | ≤128B |
| Pool 分类 | O(log n) | 16级 |

---

## 第二部分：YCSB 高强度 NUMA 迁移负载设计

### 1. 设计目标

触发 **NUMA 节点间的内存迁移**，需要满足以下条件：
1. **数据分布在多个 NUMA 节点** - 初始分配时交错分布
2. **产生热点访问模式** - 某些 key 被频繁访问
3. **热点 key 的访问线程位于远程 NUMA 节点** - 触发迁移决策
4. **持续足够时间** - 让热度达到迁移阈值

---

### 2. 负载设计方案

#### 2.1 负载架构

```
Phase 1: 数据加载 (交错分配)
  └─ 使用 interleaved 策略，数据均匀分布到 Node 0 和 Node 1

Phase 2: 热点制造 (触发迁移)
  ├─ 线程组 A (绑定 Node 0 CPU): 访问热点 key 集合 H_A
  └─ 线程组 B (绑定 Node 1 CPU): 访问热点 key 集合 H_B

Phase 3: 迁移观察
  └─ 监控 NUMA MIGRATE STATS，观察迁移发生
```

#### 2.2 工作负载配置文件

创建 `workload_numa_migration` 文件:

```properties
# ============================================================
# YCSB NUMA 迁移触发工作负载
# 目标: 触发跨 NUMA 节点的热点数据迁移
# ============================================================

# 基础配置
recordcount=500000
operationcount=2000000
workload=site.ycsb.workloads.CoreWorkload

# 操作比例: 写密集，触发内存分配
readproportion=0.4
updateproportion=0.5
scanproportion=0
insertproportion=0.1

# ============================================================
# 热点分布 - 关键配置
# ============================================================
# Zipfian 分布比 Hotspot 更真实，α 参数控制倾斜程度
requestdistribution=zipfian
zipfianconstant=0.99

# 如果需要极端热点，使用 hotspot 模式
# requestdistribution=hotspot
# hotspotdatafraction=0.05    # 5% 的 key
# hotspotopnfraction=0.95     # 95% 的操作

# ============================================================
# Value 大小 - 覆盖 Pool 分配路径
# ============================================================
fieldcount=1
fieldlength=2048              # 2KB value，走 Pool 路径
fieldlengthdistribution=constant

# ============================================================
# 插入顺序
# ============================================================
insertorder=hashed            # 哈希分布，确保 NUMA 交错

# ============================================================
# Redis 连接
# ============================================================
redis.host=127.0.0.1
redis.port=6379
redis.timeout=10000
redis.database=0
```

#### 2.3 执行脚本设计

创建 `run_numa_migration_test.sh`:

```bash
#!/bin/bash
# NUMA 迁移触发测试脚本
# 需要在双路 NUMA 服务器上运行

set -euo pipefail

REDIS_SERVER="./src/redis-server"
REDIS_CLI="./src/redis-cli"
YCSB_BIN="./tests/ycsb/ycsb-0.17.0/bin/ycsb"
WORKLOAD="./tests/ycsb/workloads/workload_numa_migration"

REDIS_PORT=6379
NUM_NODES=2

# ── 颜色输出 ──
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

log_info()  { echo -e "${BLUE}[INFO]${NC}  $*"; }
log_ok()    { echo -e "${GREEN}[OK]${NC}    $*"; }
log_step()  { echo -e "\n${BOLD}${YELLOW}══ $* ══${NC}"; }

# ── 步骤 1: 启动 Redis (交错策略) ──
start_redis() {
    log_step "步骤 1: 启动 Redis (交错分配策略)"
    
    pkill -9 -f "redis-server.*$REDIS_PORT" 2>/dev/null || true
    sleep 2
    
    $REDIS_SERVER \
        --daemonize yes \
        --port $REDIS_PORT \
        --loglevel verbose \
        --save "" \
        --appendonly no \
        --maxmemory 16gb \
        --maxmemory-policy allkeys-lru
    
    # 等待就绪
    for i in {1..10}; do
        if $REDIS_CLI -p $REDIS_PORT ping 2>/dev/null | grep -q "PONG"; then
            log_ok "Redis 启动成功"
            return 0
        fi
        sleep 1
    done
    
    log_err "Redis 启动超时"
    exit 1
}

# ── 步骤 2: 配置 NUMA 策略 ──
config_numa() {
    log_step "步骤 2: 配置 NUMA 交错策略"
    
    # 设置为交错分配，确保数据初始分布在两个节点
    $REDIS_CLI -p $REDIS_PORT NUMA CONFIG SET strategy interleaved
    sleep 1
    
    # 查看当前配置
    $REDIS_CLI -p $REDIS_PORT NUMA CONFIG GET
    
    # 启用自动迁移
    $REDIS_CLI -p $REDIS_PORT NUMA CONFIG SET cxl_optimization on
}

# ── 步骤 3: 加载数据 ──
load_data() {
    log_step "步骤 3: 加载数据 (50万条，交错分布)"
    
    $REDIS_CLI -p $REDIS_PORT FLUSHALL
    
    $YCSB_BIN load redis \
        -P "$WORKLOAD" \
        -p recordcount=500000 \
        -p threadcount=16 \
        -p redis.host=127.0.0.1 \
        -p redis.port=$REDIS_PORT \
        -s
    
    log_ok "数据加载完成"
    
    # 查看内存分布
    log_info "查看 NUMA 分配统计:"
    $REDIS_CLI -p $REDIS_PORT NUMA CONFIG STATS
}

# ── 步骤 4: 热点访问 (触发迁移) ──
run_hotspot_access() {
    log_step "步骤 4: 热点访问测试 (触发迁移)"
    
    # 重置统计
    $REDIS_CLI -p $REDIS_PORT NUMA MIGRATE RESET
    
    # 配置更低的迁移阈值，更容易触发
    $REDIS_CLI -p $REDIS_PORT NUMA CONFIG LOAD composite_lru.json || true
    
    # 如果有 numactl，绑定不同线程到不同 NUMA 节点
    if command -v numactl &>/dev/null; then
        log_info "使用 numactl 绑定线程到不同 NUMA 节点"
        
        # 线程组 A: 绑定 Node 0，访问热点 key
        numactl --cpunodebind=0 --membind=0 \
            $YCSB_BIN run redis \
                -P "$WORKLOAD" \
                -p recordcount=500000 \
                -p operationcount=1000000 \
                -p threadcount=8 \
                -p redis.host=127.0.0.1 \
                -p redis.port=$REDIS_PORT \
                -p requestdistribution=zipfian \
                -p zipfianconstant=0.99 \
                -s 2>&1 | tee /tmp/ycsb_node0.txt &
        PID_A=$!
        
        # 线程组 B: 绑定 Node 1，访问热点 key
        numactl --cpunodebind=1 --membind=1 \
            $YCSB_BIN run redis \
                -P "$WORKLOAD" \
                -p recordcount=500000 \
                -p operationcount=1000000 \
                -p threadcount=8 \
                -p redis.host=127.0.0.1 \
                -p redis.port=$REDIS_PORT \
                -p requestdistribution=zipfian \
                -p zipfianconstant=0.99 \
                -s 2>&1 | tee /tmp/ycsb_node1.txt &
        PID_B=$!
        
        wait $PID_A $PID_B
    else
        log_warn "numactl 未安装，使用普通模式"
        
        $YCSB_BIN run redis \
            -P "$WORKLOAD" \
            -p recordcount=500000 \
            -p operationcount=2000000 \
            -p threadcount=16 \
            -p redis.host=127.0.0.1 \
            -p redis.port=$REDIS_PORT \
            -p requestdistribution=zipfian \
            -p zipfianconstant=0.99 \
            -s
    fi
    
    log_ok "热点访问完成"
}

# ── 步骤 5: 查看迁移结果 ──
check_migration() {
    log_step "步骤 5: 查看迁移结果"
    
    echo -e "${BOLD}迁移统计:${NC}"
    $REDIS_CLI -p $REDIS_PORT NUMA MIGRATE STATS
    
    echo -e "${BOLD}策略统计:${NC}"
    $REDIS_CLI -p $REDIS_PORT NUMA CONFIG STATS
    
    echo -e "${BOLD}热点 Key 示例:${NC}"
    for key in user:100 user:200 user:300; do
        $REDIS_CLI -p $REDIS_PORT NUMA MIGRATE INFO $key 2>/dev/null || true
    done
}

# ── 主流程 ──
main() {
    log_step "NUMA 迁移测试开始"
    
    start_redis
    config_numa
    load_data
    run_hotspot_access
    check_migration
    
    log_ok "测试完成"
}

main "$@"
```

---

### 3. 迁移触发原理

#### 3.1 为什么这个负载能触发迁移?

```
时间线:
┌─────────────────────────────────────────────────────────┐
│ T0: 数据加载 (interleaved 策略)                          │
│     Key:100 → Node 0                                   │
│     Key:200 → Node 1                                   │
│     Key:300 → Node 0                                   │
│     ... (50万条交错分布)                                 │
│                                                         │
│ T1: 热点访问开始                                         │
│     Zipfian 0.99 → 1% 的 key 接收 99% 的请求            │
│     假设 Key:100,200,300 成为热点                        │
│                                                         │
│ T2: 线程组 A (Node 0 CPU) 频繁访问 Key:100               │
│     每次访问: numa_record_key_access()                   │
│     热度: 1 → 2 → 3 → ... → 5 (达到阈值)                 │
│     但 Key:100 在 Node 0 → 无需迁移                      │
│                                                         │
│ T3: 线程组 B (Node 1 CPU) 频繁访问 Key:100               │
│     远程访问! current_cpu_node=1, key在Node 0            │
│     热度继续增加: 5 → 6 → 7                              │
│     越过阈值 → 写入候选池                                │
│                                                         │
│ T4: serverCron 执行 composite_lru_execute()              │
│     扫描候选池，重读热度                                  │
│     条件满足: hotness=7 ≥ threshold=5                     │
│              AND mem_node=0 ≠ cpu_node=1                 │
│                                                         │
│ T5: 触发迁移!                                            │
│     numa_migrate_single_key(Key:100, target=Node 1)      │
│     数据从 Node 0 迁移到 Node 1                           │
└─────────────────────────────────────────────────────────┘
```

#### 3.2 关键参数调优

```bash
# 降低迁移阈值，更容易触发
redis-cli NUMA STRATEGY SLOT 1 composite-lru
redis-cli NUMA MIGRATE SCAN COUNT 500

# 或者直接修改 JSON 配置
cat > composite_lru.json <<EOF
{
    "migrate_hotness_threshold": 3,
    "hot_candidates_size": 512,
    "scan_batch_size": 500,
    "decay_threshold_sec": 5,
    "auto_migrate_enabled": 1
}
EOF
redis-cli NUMA CONFIG LOAD composite_lru.json
```

---

### 4. QEMU CXL 环境配置建议

#### 4.1 QEMU 启动参数

```bash
qemu-system-x86_64 \
    -machine q35,cxl=on \
    -m 16G,slots=4,maxmem=32G \
    -smp 16,sockets=2,cores=8 \
    -numa node,nodeid=0,cpus=0-7,memdev=ram0 \
    -numa node,nodeid=1,cpus=8-15,memdev=ram1 \
    -object memory-backend-ram,id=ram0,size=8G \
    -object memory-backend-ram,id=ram1,size=8G \
    -numa hmat-lb,initiator=0,target=1,hierarchy=memory,data-type=access-latency,latency=100 \
    -numa hmat-lb,initiator=1,target=0,hierarchy=memory,data-type=access-latency,latency=100 \
    -device cxl-type3,memdev=cxl-mem0,id=cxl0,host=0x0000 \
    -object memory-backend-ram,id=cxl-mem0,size=4G
```

#### 4.2 验证 NUMA 拓扑

```bash
# 查看 NUMA 拓扑
numactl --hardware

# 预期输出:
# available: 2 nodes (0-1)
# node 0 cpus: 0 1 2 3 4 5 6 7
# node 0 size: 8192 MB
# node 1 cpus: 8 9 10 11 12 13 14 15
# node 1 size: 8192 MB
```

---

### 5. 预期结果

#### 5.1 迁移统计输出

```bash
$ redis-cli NUMA MIGRATE STATS
1) "total_migrations"
2) (integer) 1250        # 预期: 数百到数千次迁移
3) "successful_migrations"
4) (integer) 1248
5) "failed_migrations"
6) (integer) 2
7) "total_bytes_migrated"
8) (integer) 2560000000  # 约 2.5GB 数据迁移
9) "total_migration_time_us"
10) (integer) 45000000   # 约 45 秒
```

#### 5.2 性能观察点

1. **迁移期间**: 延迟会有短暂上升 (每次迁移 1-5ms)
2. **迁移完成后**: 热点 Key 访问延迟下降 30-50%
3. **Composite LRU 统计**:
   - `heat_updates`: 数百万次
   - `migrations_triggered`: 数百到数千次
   - `decay_operations`: 数十万次

---

### 6. 测试检查清单

- [ ] NUMA 环境正常 (`numactl --hardware` 显示 2+ 节点)
- [ ] Redis 编译成功 (`make -j$(nproc)`)
- [ ] YCSB 安装完成 (`tests/ycsb/scripts/install_ycsb.sh`)
- [ ] Redis 启动成功并启用 NUMA
- [ ] 数据加载完成 (50万条)
- [ ] 热点访问期间 CPU 两个节点都有负载
- [ ] 迁移统计显示非零值
- [ ] 日志中出现 `[Composite LRU] Fast-path migrate` 或 `Scan migrate`

---

## 第三部分：项目重启建议

### 1. 当前状态评估

**已完成**:
- ✅ 内存池三级分配架构 (Slab + Pool + Direct)
- ✅ 基础内存迁移功能
- ✅ 策略插槽框架 (No-op + Composite LRU)
- ✅ Key 级迁移 (STRING/HASH/LIST/SET/ZSET)
- ✅ 统一命令接口 (NUMA MIGRATE/CONFIG/STRATEGY)
- ✅ YCSB 测试框架

**待完善**:
- ⚠️ 复合 LRU 实际迁移调用未完全接入 (代码中标记 TODO)
- ⚠️ db.c 中 Key 访问 Hook 未完全集成
- ⚠️ CXL 内存支持仅框架，未实现
- ⚠️ 复杂数据类型的 NUMA 感知分配未完全实现

### 2. 下一步工作

1. **完善迁移调用链**: 在 `composite_lru_execute()` 中接入实际迁移
2. **集成 Key 访问 Hook**: 在 `lookupKey()` 和 `setKey()` 中添加热度记录
3. **编写集成测试**: 验证迁移在真实 NUMA 环境下的效果
4. **性能基准测试**: 使用 YCSB 对比优化前后的性能差异
5. **CXL 支持**: 扩展 `cxl_optimized` 策略的实际实现

### 3. 关键文件清单

| 文件 | 行数 | 状态 |
|------|------|------|
| `src/numa_pool.c` | 1023 | ✅ 完整 |
| `src/zmalloc.c` | 1218 | ✅ 完整 |
| `src/numa_composite_lru.c` | 780 | ⚠️ 需接入 |
| `src/numa_key_migrate.c` | 920 | ✅ 完整 |
| `src/numa_strategy_slots.c` | 591 | ✅ 完整 |
| `src/numa_configurable_strategy.c` | 610 | ✅ 完整 |
| `src/numa_command.c` | 541 | ✅ 完整 |
| `src/numa_migrate.c` | 98 | ✅ 完整 |

---

**文档结束** - 本文档提供了完整的功能分析和迁移测试方案，可直接用于项目重启和后续开发。
