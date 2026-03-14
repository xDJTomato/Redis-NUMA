# 08-numa-configurable-strategy - NUMA可配置分配策略

**文件**: `src/numa_configurable_strategy.h`, `src/numa_configurable_strategy.c`  
**关联**: `src/numa_config_command.c`（NUMACONFIG命令实现）  
**版本**: v1.2

运行时动态配置的NUMA内存分配策略模块，支持6种分配模式，可通过配置文件或 `NUMACONFIG` 命令在线切换。

---

## 架构

```
zmalloc → numa_config_malloc(size)
               │
               └── 按当前策略选择目标节点:
                   LOCAL_FIRST   → 总是节点0
                   INTERLEAVE    → 线程局部随机均匀分布
                   ROUND_ROBIN   → 线程局部轮询计数器
                   WEIGHTED      → 按配置权重概率选择
                   PRESSURE_AWARE→ 实时选择内存利用率最低节点
                   CXL_OPTIMIZED → size < min_size → 节点0; 否则 → 节点1
```

---

## 6种分配策略

| 策略 | 适用场景 | 核心行为 |
|------|---------|---------|
| `LOCAL_FIRST` | 单节点/最低延迟 | 固定节点0 |
| `INTERLEAVE` | 多节点负载均衡 | 线程局部随机 |
| `ROUND_ROBIN` | 均匀确定性分配 | 线程局部轮询 |
| `WEIGHTED` | 异构节点环境 | 按权重概率选择 |
| `PRESSURE_AWARE` | 动态负载变化 | 选利用率最低节点 |
| `CXL_OPTIMIZED` | CXL内存架构 | 小对象本地/大对象CXL |

---

## 核心数据结构

```c
typedef struct {
    numa_config_strategy_type_t strategy_type;
    int *node_weights;
    int  num_nodes;
    double balance_threshold;
    int enable_cxl_optimization;
    size_t min_allocation_size;    /* CXL_OPTIMIZED: 大对象阈值 */
} numa_strategy_config_t;

typedef struct {
    numa_strategy_config_t config;
    int current_strategy;
    uint64_t last_rebalance_time;
    int    *allocation_counters;
    size_t *bytes_allocated_per_node;
} numa_runtime_state_t;
```

---

## 接口

```c
/* 初始化 */
int numa_config_strategy_init(void);
int numa_config_load_from_file(const char *config_file);

/* 运行时控制 */
int numa_config_set_strategy(numa_config_strategy_type_t strategy);
int numa_config_set_node_weights(int *weights, int num_nodes);
int numa_config_set_cxl_optimization(int enable);
const numa_strategy_config_t *numa_config_get_current(void);

/* 内存分配 */
void *numa_config_malloc(size_t size);
void *numa_config_calloc(size_t nmemb, size_t size);
void *numa_config_malloc_onnode(size_t size, int node);

/* 统计查询 */
void   numa_config_get_statistics(uint64_t *allocs_per_node,
                                  size_t *bytes_per_node, int num_nodes);
double numa_config_get_node_utilization(int node_id);
int    numa_config_needs_rebalance(void);

/* Redis命令处理 */
void numaconfigCommand(client *c);
```

---

## NUMACONFIG 命令

```bash
NUMACONFIG GET                    # 查看当前配置
NUMACONFIG SET strategy weighted  # 设置分配策略
NUMACONFIG SET weight 0 100       # 设置节点权重
NUMACONFIG SET cxl_optimization on
NUMACONFIG REBALANCE              # 手动触发重新平衡
NUMACONFIG STATS                  # 查看统计
```

---

## 配置文件格式（numa.conf）

```ini
[numa_strategy]
strategy = interleaved
balance_threshold = 0.3

[node_weights]
node_0 = 100
node_1 = 80

[cxl_optimization]
enable = yes
min_allocation_size = 1024
```
