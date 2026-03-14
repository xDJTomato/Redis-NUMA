# 07-numa-composite-lru.md - NUMA复合LRU策略深度解析

## 模块概述

**核心文件**: `src/numa_composite_lru.h`, `src/numa_composite_lru.c`
**版本**: v2.7
**状态**: 已实现 — 策略插槽框架1号默认策略，支持NUMACONFIG运行时配置

### 核心理念

NUMA复合LRU策略基于Redis原生LRU机制，通过"**访问感知热度追踪 + 资源感知迁移决策**"的方式，实现智能化的NUMA数据迁移：

```
访问感知 ──→ 热度累积 ──→ 候选识别 ──→ 资源评估 ──→ 智能迁移
    ↓           ↓          ↓          ↓          ↓
记录访问    统一递增    双通道筛选   带宽检查    数据重分布
```

### 解决的核心问题

**1. 热点数据分布不均**：
- 问题：热门Key集中在少数NUMA节点
- 影响：远程访问延迟增加2-3倍
- 解决：将热Key迁移到访问频率高的节点

**2. 缺乏自动化迁移**：
- 问题：需要人工干预才能优化数据分布
- 影响：运维成本高，响应慢
- 解决：基于访问模式自动触发迁移

**3. 迁移时机难以把握**：
- 问题：过早或过晚迁移都会影响性能
- 影响：迁移开销与收益不匹配
- 解决：热度阈值 + 资源状态综合判断

### 策略特色

**双通道迁移决策**：
1. **快速通道（候选池）**：访问时热度突变立即入队，serverCron优先处理
2. **兜底通道（渐进扫描）**：定期扫描全库，确保不遗漏冷门热点

**PREFIX热度追踪**：
- 热度信息直接嵌入内存PREFIX，零额外开销
- 支持原子操作，线程安全
- 与Redis原生LRU完美融合

**惰性阶梯衰减**：
- 短暂停顿豁免（<10秒不衰减）
- 分级衰减（10秒→1级，1分钟→2级，5分钟→3级）
- 长期冷却清零（≥30分钟）

---

## 核心架构深度解析

### 策略框架集成

```
┌─────────────────────────────────────────────────────────────┐
│              Redis ServerCron 调度循环                       │
│              (server.c: serverCron)                         │
└──────────────────────────┬──────────────────────────────────┘
                           │ 每秒调用
                           ▼
┌─────────────────────────────────────────────────────────────┐
│         策略插槽框架调度器                                   │
│         numa_strategy_slots.c: numa_strategy_execute_all()   │
└──────────────────────────┬──────────────────────────────────┘
                           │
                           ▼
┌─────────────────────────────────────────────────────────────┐
│              复合LRU策略执行                                 │
│              composite_lru_execute()                        │
└──────────────────────────┬──────────────────────────────────┘
                           │
        ┌──────────────────┼──────────────────┐
        │                  │                  │
        ▼                  ▼                  ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ 快速通道处理  │  │ 兜底通道扫描  │  │ 热度衰减处理  │
│ (候选池)     │  │ (渐进式)     │  │ (周期性)     │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │
       ▼                 ▼                 ▼
┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ 候选池消费    │  │ key_heat_map │  │ 热度分级衰减  │
│ 热度重读      │  │ 扫描迭代     │  │ 时间窗口判断  │
│ 资源检查      │  │ 热度评估     │  │ 原子更新     │
│ 迁移触发      │  │ 迁移决策     │  │              │
└──────────────┘  └──────────────┘  └──────────────┘
```

### 热度追踪双路径

复合LRU策略支持两种热度追踪路径：

**1. PREFIX路径（主路径）**：
```c
// 热度直接存储在内存PREFIX中
uint8_t hotness = numa_get_hotness(val);  // O(1)读取
numa_set_hotness(val, new_hotness);       // 原子更新
```
优势：零额外内存开销，与分配器紧密集成

**2. 字典回退路径（兼容路径）**：
```c
// 为不支持PREFIX的老接口提供兼容
dict *key_heat_map;  // key → heat_info 映射
heat_info_t *info = dictFetchValue(key_heat_map, key);
info->hotness = new_value;
```
优势：保持向后兼容性

### 候选池环形缓冲区

```c
// src/numa_composite_lru.h:54-56
typedef struct {
    void *key;              /* Redis key对象指针 */
    void *val;              /* Redis value对象指针 */
    int target_node;        /* 目标迁移节点 */
    uint64_t enqueue_time;  /* 入队时间戳 */
} hot_candidate_t;

// src/numa_composite_lru.h:54-56
typedef struct {
    hot_candidate_t *hot_candidates;    /* 环形缓冲区 */
    uint32_t candidates_head;          /* 写入游标 */
    uint32_t candidates_count;         /* 有效元素数 */
} composite_lru_data_t;
```

**环形缓冲区工作原理**：
```
缓冲区大小 = 256

写入过程：
head = 0, count = 0 → [ ][ ][ ]...[ ] (空)
head = 1, count = 1 → [K1][ ][ ]...[ ] (写入K1)
head = 2, count = 2 → [K1][K2][ ]...[ ] (写入K2)
...
head = 255, count = 256 → [K1][K2]...[K256] (满)
head = 0, count = 256 → [K257][K2]...[K256] (覆盖K1)

读取过程：
start = (head - count) % size = (0 - 256) % 256 = 0
依次读取 K257, K2, K3, ..., K256
```

这种设计的优点：
- 固定内存占用（256个条目）
- FIFO淘汰机制自然老化
- 无锁写入，简单高效

---

## 核心算法详解

### 1. 热度更新算法

```c
// src/numa_composite_lru.c:110-145
void composite_lru_record_access(numa_strategy_t *strategy, void *key, void *val) {
    if (!strategy || !strategy->private_data || !val) return;
    
    composite_lru_data_t *data = strategy->private_data;
    uint8_t current_hotness = numa_get_hotness(val);
    
    // 先衰减后递增（惰性衰减）
    uint8_t decayed_hotness = lazy_decay_hotness(val, current_hotness);
    uint8_t new_hotness = MIN(decayed_hotness + 1, COMPOSITE_LRU_HOTNESS_MAX);
    
    // 原子更新热度
    numa_set_hotness(val, new_hotness);
    numa_increment_access_count(val);
    numa_set_last_access(val, get_current_lru_clock());
    
    // 检查是否需要加入候选池
    if (current_hotness < data->config.migrate_hotness_threshold && 
        new_hotness >= data->config.migrate_hotness_threshold) {
        
        // 计算目标节点（通常是访问CPU所在的节点）
        int target_node = get_optimal_target_node(key, val);
        
        // 添加到候选池
        add_to_candidate_pool(data, key, val, target_node);
    }
    
    __atomic_fetch_add(&data->heat_updates, 1, __ATOMIC_RELAXED);
}

static uint8_t lazy_decay_hotness(void *val, uint8_t current_hotness) {
    if (current_hotness == 0) return 0;
    
    uint16_t last_access = numa_get_last_access(val);
    uint16_t current_time = get_current_lru_clock();
    uint16_t time_diff = calculate_time_delta(current_time, last_access);
    
    // 惰性阶梯衰减
    if (time_diff < LAZY_DECAY_STEP1_SECS * LRU_CLOCK_RESOLUTION) {
        return current_hotness;  // <10秒：不衰减
    } else if (time_diff < LAZY_DECAY_STEP2_SECS * LRU_CLOCK_RESOLUTION) {
        return MAX(current_hotness - 1, 0);  // 10秒-1分钟：衰减1级
    } else if (time_diff < LAZY_DECAY_STEP3_SECS * LRU_CLOCK_RESOLUTION) {
        return MAX(current_hotness - 2, 0);  // 1-5分钟：衰减2级
    } else if (time_diff < LAZY_DECAY_STEP4_SECS * LRU_CLOCK_RESOLUTION) {
        return MAX(current_hotness - 3, 0);  // 5-30分钟：衰减3级
    } else {
        return 0;  // ≥30分钟：清零
    }
}
```

### 2. 候选池管理算法

```c
// src/numa_composite_lru.c:147-175
static void add_to_candidate_pool(composite_lru_data_t *data, 
                                 void *key, void *val, int target_node) {
    uint32_t pool_size = data->config.hot_candidates_size;
    
    // 计算写入位置（环形缓冲区）
    uint32_t write_pos = data->candidates_head % pool_size;
    
    // 填充候选条目
    hot_candidate_t *candidate = &data->hot_candidates[write_pos];
    candidate->key = key;
    candidate->val = val;
    candidate->target_node = target_node;
    candidate->enqueue_time = get_current_time_us();
    
    // 更新游标
    data->candidates_head = (data->candidates_head + 1) % pool_size;
    if (data->candidates_count < pool_size) {
        data->candidates_count++;
    }
    
    __atomic_fetch_add(&data->candidates_written, 1, __ATOMIC_RELAXED);
}
```

### 3. 快速通道处理

```c
// src/numa_composite_lru.c:509-568
int composite_lru_execute(numa_strategy_t *strategy) {
    if (!strategy || !strategy->private_data) return NUMA_STRATEGY_ERR;
    composite_lru_data_t *data = strategy->private_data;
    
    /* 自动迁移开关 */
    if (!data->config.auto_migrate_enabled) return NUMA_STRATEGY_OK;
    
    /* ---- 快速通道：处理热点候选池 ---- */
    uint32_t pool_size = data->config.hot_candidates_size;
    uint32_t count = data->candidates_count;
    uint8_t thr = data->config.migrate_hotness_threshold;
    
    /* 计算起始槽位（环形缓冲区中最旧的条目）*/
    uint32_t start_slot = (count < pool_size) ? 0 : (data->candidates_head % pool_size);
    uint32_t processed = 0;
    
    for (uint32_t i = 0; i < count; i++) {
        uint32_t idx = (start_slot + i) % pool_size;
        hot_candidate_t *cand = &data->hot_candidates[idx];
        if (!cand->key) continue;
        
        /* 重新读取PREFIX当前热度（不依赖快照）*/
        uint8_t cur_hotness = numa_get_hotness(cand->val);
        int mem_node = numa_get_node_id(cand->val);
        
        /* 检查迁移条件 */
        if (cur_hotness >= thr && mem_node != cand->target_node) {
            /* 资源状态检查 */
            int status = check_resource_status(data, cand->target_node);
            if (status == RESOURCE_AVAILABLE) {
                /* 触发迁移 */
                trigger_migration(cand->key, cand->val, cand->target_node);
                data->migrations_triggered++;
                processed++;
            }
        }
        
        /* 清空已处理槽位 */
        cand->key = NULL;
        cand->val = NULL;
    }
    
    /* 重置候选池 */
    if (processed > 0 || count > 0) {
        data->candidates_count = 0;
        data->candidates_head = 0;
    }
    
    /* ---- 兜底通道：渐进扫描 ---- */
    composite_lru_scan_once(strategy, data->config.scan_batch_size, NULL, NULL);
    
    return NUMA_STRATEGY_OK;
}
```

### 4. 渐进式扫描算法

```c
// src/numa_composite_lru.c:570-620
int composite_lru_scan_once(numa_strategy_t *strategy, uint32_t batch_size,
                           uint64_t *scanned_out, uint64_t *migrated_out) {
    composite_lru_data_t *data = strategy->private_data;
    uint32_t scanned = 0;
    uint32_t migrated = 0;
    
    /* 获取全局数据库引用 */
    extern redisDb *server_db;
    dict *main_dict = server_db->dict;
    
    /* 初始化或继续迭代器 */
    if (!data->scan_iter) {
        data->scan_iter = dictGetSafeIterator(main_dict);
    }
    
    /* 扫描batch_size个key */
    for (uint32_t i = 0; i < batch_size; i++) {
        dictEntry *de = dictNext(data->scan_iter);
        if (!de) break;  /* 扫描完成 */
        
        robj *key = dictGetKey(de);
        robj *val = dictGetVal(de);
        
        /* 评估是否需要迁移 */
        if (should_migrate_key(data, key, val)) {
            int target_node = calculate_optimal_node(key, val);
            if (perform_migration_check(data, key, val, target_node)) {
                trigger_migration(key, val, target_node);
                migrated++;
            }
        }
        
        scanned++;
    }
    
    /* 更新统计 */
    __atomic_fetch_add(&data->scan_keys_checked, scanned, __ATOMIC_RELAXED);
    
    if (scanned_out) *scanned_out = scanned;
    if (migrated_out) *migrated_out = migrated;
    
    return scanned < batch_size ? SCAN_COMPLETE : SCAN_CONTINUE;
}
```

---