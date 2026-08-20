# numa_bw_monitor：NUMA 节点带宽监控模块

> 本文档从源码（`src/numa_bw_monitor.c` / `.h`）直接编写——这是该模块此前从未
> 有过专门设计文档的一次补齐，而不是对旧文档的重排。若本文档与代码出现分歧，
> 以代码为准。

## 1. 职责（Responsibility）

`numa_bw_monitor` 只做一件事：**实时估算每个 NUMA 节点当前的内存带宽利用率**
（0.0～1.0 的一个比例），并把这个数字提供给其他模块做决策——它自己不做任何
迁移、分配或淘汰决策。

它是 [`numa_configurable_strategy`](numa_configurable_strategy.md) 的
`PRESSURE_AWARE` 策略、[`evict_numa`](evict_numa.md) 的降级评分（带宽权重占
30%）、以及 `redis.conf` 里 `numa-bw-saturation-threshold` 配置的共同数据来源。
它由 `serverCron` 每秒调用一次 `numa_bw_monitor_sample()` 驱动采样，属于第
[06-runtime-view.md](../06-runtime-view.md) 中 serverCron 调用链的一环。

## 2. 接口（Interface）

```c
int  numa_bw_monitor_init(void);                       // 初始化，自动探测最佳后端
void numa_bw_monitor_sample(void);                      // 采样一次（serverCron 每秒调用）
double numa_bw_get_usage(int node_id);                  // 读取利用率 [0.0, 1.0]，-1 表示无效节点
double numa_bw_get_current_mbps(int node_id);            // 读取当前带宽（MB/s）
void numa_bw_set_max_bandwidth(int node_id, double max_mbps); // 设置某节点的带宽基线
const char* numa_bw_get_backend_name(void);              // 查询当前使用的后端名字符串
const numa_bw_monitor_t* numa_bw_get_monitor(void);      // 只读地拿到整个监控器结构体
void numa_bw_monitor_cleanup(void);                      // 释放/清空状态
```

`numa_bw_monitor_init()` 在 `HAVE_NUMA` 未定义时（即 `#else` 分支）全部退化为
无操作的空实现：`init()` 返回 -1，其余查询函数返回 -1.0 或 `NULL`，`sample()`
和 `cleanup()` 什么都不做——调用方不需要为「NUMA 不可用」写任何特殊判断，直接
调用即可安全地拿到「无效」的返回值。

## 3. 内部结构与关键路径（Internal Structure & Key Paths）

### 3.1 数据结构

```c
typedef struct {
    double max_bandwidth_mbps;   // 带宽基线（MB/s），来自 C-TAP 实测或配置
    double current_bw_mbps;      // 最近一次采样算出的实时带宽（MB/s）
    double bw_usage;             // 利用率 = current / max，裁剪到 [0.0, 1.0]
    uint64_t last_sample_us;     // 上一次采样的微秒时间戳
    uint64_t total_bytes_prev;   // 上一次采样时的累计字节/页数（用于算差值）
} numa_bw_node_t;

typedef struct {
    numa_bw_node_t nodes[NUMA_BW_MAX_NODES];  // 最多 16 个节点（NUMA_BW_MAX_NODES）
    int num_nodes;
    int backend;                 // 当前使用的后端
    uint32_t sample_interval_ms; // 采样间隔，默认 1000ms（NUMA_BW_SAMPLE_INTERVAL_MS）
    int initialized;
} numa_bw_monitor_t;
```

带宽基线默认值是 `NUMA_BW_DEFAULT_MAX_MBPS = 50000.0`（50GB/s，一个保守估计），
`numa_bw_set_max_bandwidth()` 可以用真实的 C-TAP 测量结果或配置文件覆盖它——
利用率的分母直接决定了「多大的实时带宽算作饱和」，这个基线不准确会让下游的
`PRESSURE_AWARE`/`evict_numa` 判断系统性偏差。

### 3.2 三种后端

```text
       ┌─────────────────────────────────────────────────────────┐
       │             numa_bw_monitor_init() 后端自动探测          │
       └────────────────────────────┬────────────────────────────┘
                                    │
                                    ▼
           /sys/fs/resctrl/mon_data/mon_L3_00 存在?
              ├── [是] ──► 启用 RESCTRL 后端 (Intel RDT 硬件计数器，最高精度)
              └── [否] ──┐
                         ▼
           /sys/devices/system/node/node0/numastat 存在?
              ├── [是] ──► 启用 NUMASTAT 后端 (内核分配命中/未命中页折算，中等精度)
              └── [否] ──┐
                         ▼
                   启用 MANUAL 兜底后端 (静态 50% 利用率占位，最低精度)

 [采样流转与指标消费 (serverCron 每秒触发)]
 ┌──────────────────────┐
 │ sysfs 伪文件系统读取 │ ──► 计算瞬时带宽 (MB/s) ──► bw_usage = current / max
 └──────────────────────┘                                   │
                               ┌────────────────────────────┴────────────────────────────┐
                               ▼                                                         ▼
                 numa_configurable_strategy                                         evict_numa
                 (PRESSURE_AWARE 选低带宽节点)                                     (降级评分占 30% 权重)
```

模块支持三种采样后端，`detect_best_backend()` 在 `numa_bw_monitor_init()` 里
按优先级自动探测（探测不到就退到下一档，永远不会因为某个后端不存在而初始化
失败）：

| 后端 | 检测条件 | 数据来源 | 精度 |
| --- | --- | --- | --- |
| `NUMA_BW_BACKEND_RESCTRL` | `/sys/fs/resctrl/mon_data/mon_L3_00` 存在 | Intel RDT resctrl 的 `mbm_total_bytes`（内存带宽监控计数器，直接是字节数） | 最高——硬件计数器，单位是真实字节 |
| `NUMA_BW_BACKEND_NUMASTAT` | `/sys/devices/system/node/node0/numastat` 存在 | 该文件里 `numa_hit` + `numa_miss` 两个计数器之和，按 4KB 页折算成字节 | 中等——是页级别的近似值，且 `numa_hit`/`numa_miss` 统计的是"分配落在本地/远端节点"的页数，不是严格意义上的总线带宽 |
| `NUMA_BW_BACKEND_MANUAL` | 前两者都不可用时的兜底 | 不采样，`bw_usage` 保持用户通过 `numa_bw_set_max_bandwidth()` 设置的值（该函数在 manual 后端下会顺带把 `bw_usage` 设为固定的 0.5，即假设 50% 利用率） | 最低——是一个静态占位值，不反映真实负载 |

三种后端的采样函数（`sample_resctrl()`/`sample_numastat()`/`sample_manual()`）
共享同一套差值计算逻辑：记录上一次采样的累计值 `total_bytes_prev` 和时间戳
`last_sample_us`，本次采样时用 `(当前累计值 - 上次累计值) / 经过的秒数` 算出
瞬时带宽（`current_bw_mbps`），再除以 `max_bandwidth_mbps` 并裁剪到
`[0.0, 1.0]` 得到 `bw_usage`。两个例外：

- **首次采样**：`last_sample_us == 0` 时只记录基线值，不计算带宽（避免用
  "从进程启动到现在"的整段时间去算平均带宽，那样会严重低估瞬时值）。
- **计数器回绕/重置**：如果本次读到的累计值比上次还小（`curr_bytes <
  node->total_bytes_prev`），直接把 `current_bw_mbps` 置 0 并跳过计算，而不是
  用一个巨大的负数差值算出荒谬的带宽——resctrl/numastat 的底层计数器在某些
  内核版本下确实会被重置或回绕。

### 3.3 采样节流

`numa_bw_monitor_sample()` 本身不假设调用者按固定节奏调用它——它自己检查
`g_bw_monitor.nodes[0].last_sample_us` 距现在是否已经过了
`sample_interval_ms`（默认 1000ms），没到点就直接返回。这让它对调用频率不敏
感：即使某个未来的调用方每 100ms 调一次，实际采样仍然只按 1 秒一次的节奏发生，
`serverCron` 目前正好也是每秒调用一次，二者天然对齐。

## 4. 质量与性能特性（Quality & Performance Characteristics）

- **开销**：每次采样是每个节点一次 `fopen`/`fscanf`/`fclose`（resctrl 或
  numastat 后端），文件都在 `/sys` 下的伪文件系统里，属于内核直接返回内存中的
  计数器值，没有真实磁盘 I/O；manual 后端则完全不做文件访问。在 1 秒一次、
  节点数通常个位数的量级下，这个开销相对于 `serverCron` 其他工作可以忽略。
- **优雅降级**：三个后端的探测与切换在 `numa_bw_monitor_init()` 里一次性完成，
  运行期不会再切换后端；一旦某个环境完全没有 resctrl 也没有 numastat（比如
  本项目自己的开发主机、大多数容器化环境），会静默落到 manual 后端而不是初始
  化失败——这意味着 `PRESSURE_AWARE` 策略和 `evict_numa` 的带宽评分在这种环境
  下永远拿到一个静态的 0.5，不代表真实压力，使用者需要知道这一点（见下面的
  已知限制）。
- **单点写者**：`g_bw_monitor` 是一个静态全局变量，采样只发生在 `serverCron`
  线程（Redis 主线程）里，没有并发写入，因此不需要任何锁。

## 5. 与其他模块的关系（Relations to Other Modules）

```text
serverCron（每秒）
  └─> numa_bw_monitor_sample()          [本模块]
        │
        ├─> numa_bw_get_usage(node) ──> numa_configurable_strategy 的
        │                                PRESSURE_AWARE 策略：优先选带宽利用率低的节点
        │
        └─> numa_bw_get_usage(node) ──> evict_numa 的加权评分：
                                          score = 距离(40%) + 压力(30%) + 带宽(30%)
                                          （带宽利用率越高，越不适合作为降级目标）
```

`numa-bw-saturation-threshold`（`redis.conf` 约 1210 行）定义"利用率超过此
阈值（默认 95）的节点不会被选为降级目标"，读取的正是本模块 `bw_usage` 的值。

## 6. 未解决问题与已知限制（Open Issues & Known Limitations）

- **numastat 后端衡量的其实是"命中率"，不是总线带宽**：`numa_hit`/
  `numa_miss` 统计的是内存分配请求落在本地/远端节点的次数，用页大小（4KB）
  折算成"字节"再除以时间得到的"带宽"，本质上是一个分配速率的代理指标，
  和真实的内存总线带宽（resctrl 的 `mbm_total_bytes` 才是）存在系统性偏差，
  在分配频繁但访问不频繁的负载下会高估压力，反之会低估。
- **manual 后端不是真的"手动"，而是一个静态假设**：目前唯一设置真实值的入口
  `numa_bw_set_max_bandwidth()` 在 manual 后端下会把 `bw_usage` 硬编码成 0.5，
  没有任何外部接口可以在运行时把它设成别的观测值——也就是说 manual 后端目前
  只是"假装有 50% 压力"的占位符，而不是一个可以接入外部监控系统真实读数的
  通道。这在只有 1 个物理 NUMA 节点的开发环境下几乎不影响正确性（因为
  `PRESSURE_AWARE`/`evict_numa` 在单节点下没有别的节点可选，本项目自身的开发
  主机就是单 NUMA 节点），但在多节点、没有 resctrl 权限的生产环境下会让
  "带宽感知"名不副实。
- **不支持超过 `NUMA_BW_MAX_NODES`（16）个节点**：`numa_bw_monitor_init()` 里
  `max_node + 1 >= NUMA_BW_MAX_NODES` 时直接初始化失败——对当前的 CPU 拓扑
  规模够用，但这是一个需要留意的硬编码上限，不是动态数组。
- **计数器回绕的处理是"跳过这一轮"而不是"修正"**：出现回绕时当轮
  `current_bw_mbps` 直接置 0，会在那一秒产生一次误报的"零利用率"，而不是
  尝试估算回绕前后的真实增量——对每秒采样、长期运行的场景影响很小，但如果
  未来有人把采样间隔拉长（比如几十秒一次），这种"整轮清零"的影响会被放大。
