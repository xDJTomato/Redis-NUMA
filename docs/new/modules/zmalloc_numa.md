# zmalloc — NUMA 分配集成点

> 这不是 `ARCHITECTURE.md` 里列出的十个 NUMA 模块之一，而是所有模块最终都要经过
> 的**关键集成点**：Redis 原生的内存分配入口 `src/zmalloc.c`/`zmalloc.h`。十个模
> 块里除了 `numa_command`（纯命令路由），几乎都通过读写这里定义的元数据、或调用
> 这里暴露的接口来工作。

## 职责

`zmalloc.c/h` 是 Redis 内部所有内存分配/释放的唯一入口——`robj`、SDS、字典节点、
listpack、quicklist 节点等等，最终都通过 `zmalloc()`/`zfree()`/`zrealloc()` 走到
这里。本集成点要解决的问题是：**在不改变这个入口对外行为（同样的函数签名、同样
的返回语义）的前提下，让每一次分配都带上 NUMA 元数据，并让分配本身按策略落到指
定的 NUMA 节点上**——这样上层的 `numa_key_migrate`、`numa_composite_lru` 等模块才
有"元数据"和"迁移目标"可用，而不需要为每个 Redis 对象单独维护一张外部映射表（那
样代价是一次额外的哈希查找，而不是一次指针运算）。

## 接口

对上层（Redis 核心其余代码、字典实现等）保持与原生 Redis 完全一致的签名：

| 函数 | 说明 |
| --- | --- |
| `zmalloc(size)` | NUMA 感知分配，返回用户指针或 `NULL` |
| `zfree(ptr)` | NUMA 感知释放 |
| `zrealloc(ptr, size)` | 重新分配，返回新用户指针或 `NULL` |
| `zmalloc_local(size)` / `zcalloc_local(size)` / `ztrycalloc_local(size)` | `dict.c` 专用的分配入口（见下方「与其他模块的关系」） |

面向 NUMA 模块的元数据读写接口（供 `numa_composite_lru`/`numa_key_migrate` 等使
用，均基于同一个 `numa_get_prefix()` 指针运算）：

| 函数 | 功能 |
| --- | --- |
| `numa_get_hotness(ptr)` / `numa_set_hotness(ptr, h)` | 读/写热度（0-7） |
| `numa_get_node_id(ptr)` / `numa_set_node_id(ptr, n)` | 读/写当前所在 NUMA 节点 |
| `numa_get_last_access(ptr)` / `numa_set_last_access(ptr, t)` | 读/写上次访问时间（LRU 时钟低 16 位） |

## 内部结构与关键路径

### PREFIX 元数据内联

不用外部字典记录"这块内存归哪个节点、多热"，而是把一个 16 字节的
`numa_alloc_prefix_t` 直接内联在每次分配返回的用户指针**前面**：

```c
typedef struct {
    size_t size;           // 8B  实际分配大小
    char from_slab;        // 1B  来源标记（0=Direct, 1=Slab）
    char node_id;          // 1B  NUMA 节点 ID
    uint8_t hotness;       // 1B  热度级别（0-7）
    uint8_t access_count;  // 1B  访问计数（循环计数）
    uint16_t last_access;  // 2B  LRU 时钟低 16 位
    char migrated;         // 1B  迁移亲和标记
    char reserved[1];       // 1B  保留对齐
} numa_alloc_prefix_t;      // 共 16 字节，见 src/zmalloc.c
```

`zmalloc()` 对外返回的指针实际指向 `raw_ptr + PREFIX_SIZE`；`zfree()`/热度读写
一律先做 `numa_get_prefix(user_ptr) = user_ptr - PREFIX_SIZE` 找回这个前缀。这样
「查某个对象的 NUMA 元数据」就是一次指针减法，而不是一次哈希表查找——这是整个热
度追踪体系性能可接受的前提，也是为什么本项目必须 `#define NO_MALLOC_USABLE_SIZE`
（`zmalloc.h`）：Redis 原生用 `malloc_usable_size()` 统计"这块内存实际可用多
大"，但加了 16 字节前缀之后这个系统调用返回的大小已经不对，必须整体绕开、自己
在 PREFIX 里记账。

### 分配路径：三级，按大小分流

| 大小 | 路径 | 关键函数 | 说明 |
| --- | --- | --- | --- |
| ≤ 64KB | tcache → Slab | `fast_size_class()` → 命中 tcache bin，否则 `numa_slab_alloc()` | 无锁：TLS 命中或原子 CAS |
| 64KB < 大小 ≤ 2MB | Direct Cache → Direct | `direct_cache_pop()` 命中，否则 `numa_alloc_onnode()` | 无锁 TLS 命中，未命中才走系统调用 |
| > 2MB | Direct | `numa_alloc_onnode()` | 系统调用（mmap + mbind） |

节点选择本身委托给 `numa_configurable_strategy` 的 `numa_config_get_best_node(size)`
（默认策略 `WEIGHTED_INTERLEAVE`：按节点压力做加权随机选择，压力越大的节点被选
中概率越低）——`zmalloc.c` 自己不判断"哪个节点更好"，只负责按选定节点执行分配
并写好 PREFIX。

### 释放路径：镶嵌对称

`zfree()` 先用 `numa_get_prefix()` 读出 `from_slab`/`node_id`，再对称走
tcache（先缓存，可能之后被复用）或直接归还 slab/系统。

### 两级 TLS 缓存：为什么需要它们

**tcache**（`src/zmalloc.c` 的 `tls_tcache`）：33 个 size class 各一个 bin
（`TCACHE_BIN_MAX=64`），服务 ≤64KB 的小对象。动机是实测发现：64 线程高并发下，
每次分配/释放都要跟 slab 分配器的共享状态打交道（位图 CAS、`current_slab` 原子加
载），这类共享状态操作在高并发下成为瓶颈，一度导致 NUMA 版比 vanilla libc malloc
慢 25%-29%。上线 tcache 之后 Phase 2 的差距从约 11.5% 收窄到约 5.9%。

**Direct Cache**（`tls_direct_cache`）：服务 64KB～2MB 的大对象，避免每次都走
`numa_alloc_onnode()`/`numa_free()`（mmap + mbind + page fault，系统调用级开
销）。以 FIFO 方式缓存最近释放的大对象，按「同节点 + 同大小」匹配复用；命中率、
未命中数、驱逐数都通过原子计数器暴露给 `NUMA CONFIG STATS`。

两级缓存都是 `__thread` 变量（`tls_tcache_inited` 标记首次访问时才 `memset`
初始化），彼此之间没有共享状态、不需要加锁。

**统计一致性上的一个真实教训**：早期版本里，对象进 tcache 时立即从
`used_memory`/`used_memory_node` 里减掉，命中时再加回来——如果对象在缓存期间被
迁移到另一个节点（`prefix->node_id` 被改写但计数器没有同步跟着变），`used_memory_node`
就会出现负值。修复方式是把「记账」推迟到真正的 drain/flush（缓存对象被移出、
真正归还给 slab/系统的那一刻）——缓存期间不管命中还是未命中，计数器都保持不
动。这类"缓存状态和统计计数器要不要同步移动"的问题，在任何带缓存层的分配器改
造里都值得留意。

## 质量与性能特性

- **线程安全**：PREFIX 本身的读写发生在 Redis 单线程事件循环里，不需要额外同
  步；热度更新集中在 `composite_lru_record_access()` 里串行执行；跨节点共享的统
  计计数器用 `atomicIncr`/`atomicDecr` 无锁更新；两级 TLS 缓存天生每线程独立，没
  有共享状态竞争。
- **为什么不能用 jemalloc**：本集成点是直接接管 `zmalloc`/`zfree` 语义的，如果
  同时链接 jemalloc，两套分配器会对同一段内存的元数据产生冲突写入，导致堆损
  坏——这也是为什么 `src/Makefile` 强制 `MALLOC=libc`（见 `ARCHITECTURE.md`）。
- **Size class 查找的复杂度**：从原来 O(33) 的线性扫描换成一张覆盖 0~65536、步
  长 8 字节的查找表（`g_class_lookup`），`numa_init()` 里预计算一次，换来 O(1) 的
  size class 定位。

## 与其他模块的关系

- **`numa_pool`**：`zmalloc()`/`zfree()` 的 slab 路径最终调用
  `numa_slab_alloc()`/`numa_slab_free()`。
- **`numa_configurable_strategy`**：每次分配调用
  `numa_config_get_best_node(size)` 决定目标节点。
- **`numa_composite_lru`**（以及其他迁移策略）：通过本集成点暴露的
  `numa_get_hotness`/`numa_set_hotness` 等接口读写 PREFIX 里的热度字段；
  `db.c` 的 `lookupKey()` 在每次访问时调用 `composite_lru_record_access()`，
  该函数直接对 `numa_get_prefix()` 拿到的 PREFIX 做阶梯式惰性衰减 + 热度递增。
- **一个必须记住的边界情况——不是所有二进制都会初始化 NUMA 状态**：`numa_init()`
  只在 `redis-server` 的 `main()` 里被调用。`redis-cli`、`redis-benchmark`、
  `redis-check-rdb`/`redis-check-aof`、`redis-sentinel` 同样链接了 `zmalloc.o`，
  但从不调用 `numa_init()`，所以这些进程里 `numa_ctx.numa_available` 一直是
  0。`zmalloc()`/`zcalloc()`/`zrealloc()` 本身已经对这个 flag 做了判断、退化成
  普通 `malloc()`——但 `dict.c` 用的 `zmalloc_local()`/`zcalloc_local()`/
  `ztrycalloc_local()` 曾经没有做同样的判断，会无条件调用 `numa_alloc_dram()`，
  在从未初始化过的全局状态上运行 slab 分配器逻辑。这不是一个假设的风险——它是
  Redis 6.2.21→7.2.6 合并过程中被 `tests/unit/cluster/cli.tcl`（`redis-cli
  --cluster create`）真实触发过的 SIGSEGV，完整记录见
  `docs/redis7-migration.md`「一个被合并暴露的历史 bug」一节。修复方式是给
  `numa_alloc_dram()` 补上和 `zmalloc()` 同样的 `numa_ctx.numa_available` 判断 +
  普通 `malloc` 退化路径。**教训对任何新代码同样适用**：不要假设
  `numa_ctx.numa_available` 一定是 1，任何直接操作 PREFIX 或调用底层分配函数的
  新代码，都要显式处理"NUMA 未初始化"这一分支。

## 未解决问题与已知限制

- 两级 TLS 缓存的命中率依赖工作负载的大小分布——`NUMA CONFIG STATS` 暴露了命中
  /未命中/驱逐计数器，但目前没有自动根据命中率调整 `TCACHE_BIN_MAX`/
  `DIRECT_CACHE_MAX` 的机制，需要的话得手动改常量重新编译。
  128KB～1MiB 区间的吞吐差距是 Direct Cache 引入的直接动机，具体数字见
  `docs/test/benchmark_results.txt`。
  - Direct Cache 目前只按「同节点 + 同大小」精确匹配，不做近似大小匹配，未命中
  即直接回落到系统调用路径，没有中间的"近似复用后 realloc 收缩"策略。
- PREFIX 结构是编译期固定的 16 字节布局；如果未来要在 PREFIX 里加新字段（比如
  更细粒度的迁移状态位），需要同时评估它对 33 级 size class 对齐和内存开销占比
  的影响，不是可以随意追加的。
