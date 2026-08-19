# 8. 贯穿性概念（Crosscutting Concepts）

> arc42 第 8 章。本章收集**不属于任何单一模块、但反复出现在多个模块里**的设计
> 惯例——理解这些惯例比理解任何一个模块的内部实现更重要，因为它们是贯穿全部
> 十个 NUMA 模块的「隐性契约」。违反其中任何一条，最常见的后果就是启动期崩溃、
> 编译期隐藏错误，或者只在特定二进制/特定负载下才会触发的堆损坏。

## 8.1 初始化顺序约定

**规则**：`numa_init()` 必须在 `initServer()` **之前**调用；策略框架 /
按键迁移 / 带宽监控等模块的初始化必须在 `initServer()` **之后**调用。

```
main()
  ├─ numa_init()          // 必须在 initServer() 之前
  ├─ initServer()
  ├─ numa_key_migrate_init()      // 必须在 initServer() 之后
  ├─ numa_strategy_slots_init()   // 同上
  └─ numa_bw_monitor_init()       // 同上
```

原因：`numa_init()` 只负责建立最底层的分配器状态（slab/direct-cache），不依赖
Redis 的任何全局状态；而策略/迁移/带宽监控模块反过来要读取 `initServer()`
建立的 `server` 全局结构（配置、事件循环等）。颠倒这个顺序——例如在
`numa_pool` 初始化完成之前就调用迁移函数——在实践中是这个 fork 里**最常见的
启动期崩溃原因**（见 [`CONTRIBUTING.md`](../../CONTRIBUTING.md) 的新增模块
checklist 第 4 条）。这也是为什么 [09-architecture-decisions.md](09-architecture-decisions.md)
把它列为独立的架构决策，而不只是一条编码规范。

## 8.2 `_serverLog` 约定

**规则**：NUMA 模块内部不要直接调用 `serverLog()`，而要用

```c
extern void _serverLog(int level, const char *fmt, ...);
```

这是 Redis 内部既有的约定（`numa_composite_lru.c`、`numa_bw_monitor.c` 等模块
都遵循）。对新增模块的作者来说，这是一条容易忽略、但会在编译期就报错的规则，
不遵守的成本很低（编译失败），但因为它出现在**每一个**模块的代码里，值得单独
列为贯穿性概念，而不是让读者在十份模块文档里各自发现一遍。

## 8.3 PREFIX 元数据内联模式

**问题**：libnuma 的 `numa_alloc_onnode()` 分配出的内存，事后无法查询"这块内存
有多大、在哪个节点上"——不像 glibc 的 `malloc_usable_size()`。所有 NUMA 模块都
需要这两个信息（迁移时要知道大小才能 `memcpy`，需要知道当前节点才能判断要不要
搬）。

**解法**：每一次分配都在返回给调用者的指针**前面**多分配 16 字节，塞进一个
定长的元数据结构（定义在 `src/zmalloc.c`，`HAVE_NUMA` 分支下）：

```c
typedef struct {
    size_t  size;           /* 8B — 实际分配的内存大小 */
    char    from_pool;      /* 1B — 来源：0=直接分配，1=slab */
    char    node_id;        /* 1B — 所在 NUMA 节点 */
    uint8_t hotness;        /* 1B — 热度等级 0-7，0=冷，7=热 */
    uint8_t access_count;   /* 1B — 访问计数（循环计数器） */
    uint16_t last_access;   /* 2B — LRU 时钟低 16 位（上次访问时间） */
    char    migrated;       /* 1B — 迁移亲和标志：迁移过的对象在 UPDATE 时保持节点亲和 */
    char    reserved[1];    /* 1B — 预留 */
} numa_alloc_prefix_t;   // sizeof == 16
```

`PREFIX_SIZE` 就是 `sizeof(numa_alloc_prefix_t)`；`numa_get_prefix(ptr)` 通过
指针减法（`(char*)ptr - PREFIX_SIZE`）拿到它。**这也是为什么整个项目必须
`#define NO_MALLOC_USABLE_SIZE`**（`src/zmalloc.h`）：一旦在指针前面塞了这段
元数据，`malloc_usable_size()` 系统调用返回的"可用大小"就不再准确（它不知道
前面还有 16 字节属于我们自己的记账），必须整条路径绕开它，自己维护统计。这个
模式同时解释了 `numa_pool`（分配路径写入 PREFIX）、`numa_key_migrate`（迁移时
读 PREFIX 判断当前节点/热度）、`evict_numa`（淘汰前读 PREFIX 判断是否值得
降级）三个模块为什么共享同一套结构体，而不是各自维护一份。

## 8.4 按编码类型的迁移适配器模式

**问题**：Redis 的每一种数据类型在内部都有不止一种编码（encoding）——迁移一个
`robj` 不是简单的一次 `memcpy`，必须知道具体编码才能正确搬运。

**解法**：`numa_key_migrate` 对全部 5 种类型的全部编码都实现了专门的适配器，
这个模式在模块内部反复出现：

| 类型 | 涉及编码 |
| --- | --- |
| STRING | RAW、EMBSTR |
| HASH | listpack、ziplist（RDB 兼容）、hashtable |
| LIST | quicklist（LZF 压缩/原始，`PLAIN`/`PACKED` 节点容器子路径） |
| SET | intset、hashtable |
| ZSET | listpack、ziplist、skiplist |

任何触碰 Redis 值本身的新模块（不仅是迁移，未来任何需要"理解 value 内部结构"
的功能）都要面对同一个决策：是像 `numa_key_migrate` 一样为每种编码写专门分支，
还是退化成"整块 `memcpy`"（对 hash/zset 的 listpack/ziplist 恰好可行，因为它们
本身就是一段连续内存；对 hashtable/skiplist 这类多重指针结构则不行）。这条模式
本身，比任何一个具体类型的迁移代码更值得在这里强调。

## 8.5 JSON 热加载模式

**问题**：调整迁移策略的参数（阈值、批量大小等）如果每次都要重启 Redis，对
线上调优极不友好。

**解法**：Composite LRU 的全部可调参数放在外部 `composite_lru.json` 文件里，
启动时自动加载，运行期可以用 `NUMA CONFIG LOAD [/path]` 不重启地重新加载
（`src/numa_command.c` 里 `NUMA CONFIG LOAD` 分支）。这是这个项目里**唯一**的
热加载配置面——其它策略/分配相关的参数都是走标准 `CONFIG SET`（有 Redis
自身的配置系统兜底），而 Composite LRU 选择自建一条 JSON 路径，是因为它的
参数结构（阶梯衰减表、双通道权重等）不是标量，套用 Redis 单值配置项会很别扭。
未来如果一个新策略需要类似的复合结构化参数，这个模式（外部 JSON + 显式
`LOAD` 命令）是现成的参照，而不必发明第二套机制。

## 8.6 渐进式扫描：不阻塞单线程事件循环

**问题**：Redis 是单线程处理命令的；任何一次策略执行如果同步扫一遍整个
keyspace，都会让所有客户端请求排队等待，等同于一次长时间的"stop-the-world"。

**解法**：Composite LRU 的慢路径扫描每一轮只推进字典迭代器一小段
（`composite_lru_execute_step()` 用 `budget`/`deadline_us` 控制这一轮最多处理
多少 key、最多花多少时间），下一轮 `serverCron`/AE 调度再接着扫，而不是一次
扫完。这个"渐进式、有预算、可续跑"的模式后来被进一步推广成
[`modules/ae_strategy_scheduler.md`](modules/ae_strategy_scheduler.md) 里描述
的、更通用的 `execute_step(strategy, deadline_us, budget)` 接口——也就是说，
渐进式扫描不是 Composite LRU 一次性的技巧，而是整个策略框架现在的标准执行
契约：**任何策略的一步执行都必须能在有限时间/有限数量内返回**，不管它用
`serverCron` 调度还是用 AE time event 调度。

## 8.7 内核占位 + NUMAflow 完整实现的分层模式

**问题**：`numa_configurable_strategy` 里的 `ADAPTIVE` 和 `LATENCY_AWARE` 两种
分配策略，如果要做到"真正自适应"，逻辑会相当复杂（需要运行时反馈、参数调优），
而内核分配路径是这个项目里最不适合塞复杂逻辑、最需要保持简单可预测的地方。

**解法**：内核里这两个策略保持占位实现，完整版本刻意放进独立子系统
NUMAflow 的 `alloc_adaptive`/`alloc_latency_aware` 原子操作里，通过
`NUMA FLOW` 命令桥接回 Redis（见 `src/numa_flow.c`）。这是一个有意的架构分层
决策——内核关键路径只做"简单、可预测、可审计"的事，任何需要迭代、需要复杂
调参的逻辑都下放到可以独立编译、独立测试、甚至可以在没有真实 NUMA 硬件的机器
上开发的 NUMAflow 里。详见 [09-architecture-decisions.md](09-architecture-decisions.md)
中对应的决策记录。

## 8.8 优雅降级作为一种测试哲学

**问题**：这个项目的开发环境经常缺东西——没有 `/dev/kvm`、只有 1 个物理
NUMA 节点、没有预装 JDK、CXLMemSim 需要一个未打补丁的内核才能真正碰到模拟
内存……如果每个验证脚本在缺条件时都直接失败退出（或者更糟，悄悄跳过却报告
"通过"），会让人没法信任任何一次验证结果。

**解法**：这不是某一个测试脚本的individual 特性，而是贯穿本仓库**所有**验证
脚本的统一原则——环境不支持时，记录成 `skipped` 并写明原因，`exit 0`，绝不
伪造一个"通过"的假象。`run_full_validation.sh`、`tests/vm/boot_numa_vm.sh`、
`tests/cxl/run_cxlmemsim.sh` 全部遵循这一条。把它放进贯穿性概念而不是
[10-quality-requirements.md](10-quality-requirements.md) 的某一条场景，是因为
它不仅是一个质量目标，更是这个项目所有测试代码共享的一段"如何写降级逻辑"的
实现模式：先检测前置条件，检测失败就写状态文件 + log 原因 + 正常退出，检测
成功才继续往下跑真实验证。

---

**参见**：[05-building-block-view.md](05-building-block-view.md)（模块总览）、
[06-runtime-view.md](06-runtime-view.md)（这些模式在具体调用链里的体现）、
[09-architecture-decisions.md](09-architecture-decisions.md)（为什么做出这些
设计选择，而不只是它们是什么）。
