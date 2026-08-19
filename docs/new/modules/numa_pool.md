# numa_pool — NUMA 感知的 Slab 分配器

> Building Block 详情表 · 对应 arc42 §5（Building Block View）
> 源文件：`src/numa_pool.c` / `src/numa_pool.h`

## 职责 (Responsibility)

`numa_pool` 是整个 NUMA 模块链的最底层：它决定"一次内存分配请求，物理上落在哪个
NUMA 节点、以什么方式管理"。它要解决的核心问题是——通用分配器（`malloc`/
jemalloc）完全不知道 NUMA 拓扑，也不会对 8B～64KB 这个 Redis 最常见的对象尺寸区
间做专门优化。`numa_pool` 用一套 **jemalloc 风格的两级 Slab 分配器**覆盖这个区
间，为更上层的 `zmalloc` 集成（见 `modules/zmalloc_numa.md`）提供按节点、按大小
class 的 O(1) 无锁分配能力，同时通过 16 字节的 PREFIX 元数据为再上层的按 key 迁移
（`numa_key_migrate`）提供热度/节点归属的记账基础。

## 接口 (Interface)

对外暴露的核心函数（均声明在 `numa_pool.h`）：

| 函数 | 作用 |
| --- | --- |
| `int numa_slab_init(void)` | 初始化所有 NUMA 节点的 Slab 分配器；0 成功，-1 失败 |
| `void numa_slab_cleanup(void)` | 清理全部 Slab，释放内存 |
| `void *numa_slab_alloc(size_t size, int node, size_t *total_size)` | 从 Slab 分配一个对象（8B～64KB），返回含 PREFIX 的指针，失败返回 NULL |
| `void numa_slab_free(void *ptr, size_t total_size, int node)` | 释放一个由 `numa_slab_alloc` 分配的对象（原子位图操作） |
| `static inline int should_use_slab(size_t size)` | `size <= 65536` 时返回 1——调用方用它判断走 Slab 路径还是 Direct 路径 |
| `int numa_pool_num_nodes(void)` | 返回 NUMA 节点数量 |
| `int numa_pool_get_node(void)` | 返回当前 CPU 所在的 NUMA 节点 |
| `int numa_pool_available(void)` | NUMA 是否可用（供上层做降级判断） |

关键常量（`numa_pool.h`，均已核对与当前源码一致）：

```c
#define NUMA_POOL_SIZE_CLASSES 33            // jemalloc 风格 33 级大小 class（8B-64KB）
#define SLAB_SIZE (64 * 1024)                // 64KB 小 slab（≤4KB 对象）
#define LARGE_SLAB_SIZE (2UL * 1024 * 1024)   // 2MB 大 slab（>4KB 对象）
#define SLAB_MAX_OBJECT_SIZE 65536            // Slab 处理 8B-64KB 的对象
#define SLAB_BITMAP_SIZE 96                   // 3072bit 位图（96 × 32bit）
#define SLAB_EMPTY_CACHE_MAX 8                // 空闲 slab 不再被急切释放的保留上限
```

## 内部结构与关键路径 (Internal Structure & Key Paths)

### 33 级大小分类

```c
const size_t numa_pool_size_classes[NUMA_POOL_SIZE_CLASSES] = {
    8, 16, 24, 32, 48, 64, 80, 96, 128,                        /* 小对象 */
    160, 192, 256, 320, 384, 512, 640, 768,                    /* 中对象 */
    1024, 1280, 1536, 2048, 2560, 3072, 4096,                  /* 大对象 */
    5120, 6144, 7168, 8192, 10240, 12288, 16384, 32768, 65536  /* 超大对象，走大 slab */
};
```

| 级别 | 大小范围 | 典型用途 |
| --- | --- | --- |
| 0–6 | 8B–128B | SDS 短字符串、Redis 对象头、dictEntry |
| 7–11 | 160B–384B | SDS 中等字符串、小 hash/zset 元素 |
| 12–15 | 512B–1024B | SDS 长字符串、robj 包装 |
| 16–23 | 1280B–4096B | 大 SDS、多字段元素、8KB value（YCSB 典型负载） |
| 24–32 | 5120B–65536B | 超大对象，走大 slab 路径 |

### 两级 Slab 设计

为消除 >4KB 对象的 per-object page 对齐浪费，分配器把 33 级大小 class 分成两层：

- **小 slab**（class 0–23，≤4KB 对象）：`SLAB_SIZE = 64KB`，位图管理（`SLAB_BITMAP_SIZE=96` × 32bit = 3072 bit）。
- **大 slab**（class 24–32，>4KB 对象，源码用 `is_large_slab_class()`：`class_idx >= 24`）：`LARGE_SLAB_SIZE = 2MB`，用 `mmap` + `mbind` + `munmap` 的 memkind 风格分配，消除 page 对齐浪费。

大 slab 的分配流程（源码 `numa_pool.c`，`alloc_size = LARGE_SLAB_SIZE * 2`，即先申请 4MB 再裁剪）：

1. `mmap(NULL, 4MB)` 分配 4MB 虚拟地址空间；
2. 计算 2MB 对齐偏移 `aligned_addr = (raw_addr + LARGE_SLAB_SIZE - 1) & ~(LARGE_SLAB_SIZE - 1)`；
3. `munmap` 释放头部和尾部未对齐区域，只留 2MB 对齐部分；
4. `mbind(aligned_addr, LARGE_SLAB_SIZE, node)` 绑定到目标 NUMA 节点；
5. 在 2MB slab 内按 size class 细分对象，位图管理。

**为什么要有大 slab**：8KB 对象如果走 Direct 路径（`numa_alloc_onnode`），`mmap` 会按 page（4KB）对齐，12288B 的实际系统调用会占 3 个 page（12KB），每对象浪费 4KB（50%）；100 万个 8KB key 就多出约 3.9GB 常驻内存。走大 slab 路径后，一个 2MB slab 可以装 256 个 8KB 对象，RSS 只需要 2MB；100 万个同样的 key 只需要约 7.8MB slab 内存。

### 核心数据结构

```c
typedef struct numa_slab_header {
    uint32_t magic;                  // 校验用魔数
    uint32_t class_idx;              // 大小分类索引
    struct numa_slab *slab;          // 回指 slab 结构
    void *raw_memory;                // 未对齐的原始内存（numa_free 时用）
} numa_slab_header_t;

typedef struct numa_slab {
    void *memory;                            // Slab 内存基址（64KB/2MB 对齐）
    uint32_t bitmap[SLAB_BITMAP_SIZE / 32];  // 3072bit 位图
    uint16_t free_count;                     // 剩余空闲槎位数
    struct numa_slab *next;                  // 同状态链表指针
    uint16_t class_idx;                      // 大小分类索引
} numa_slab_t;

typedef struct {
    size_t obj_size;              // 该级别的对象大小
    numa_slab_t *partial_slabs;   // 部分使用的 slab 链表
    numa_slab_t *full_slabs;      // 已满的 slab 链表
    numa_slab_t *empty_slabs;     // 空闲 slab 缓存
    uint32_t slab_count;          // 当前 slab 总数
    pthread_mutex_t lock;         // 仅切换链表时持锁
} numa_slab_class_t;
```

16 字节的 PREFIX 元数据（挂在每次分配返回指针的前面，释放时通过 `(numa_alloc_prefix_t *)ptr - 1` 找回）：

```c
typedef struct {
    size_t size;           // 实际对象大小
    char from_slab;        // 来源标记（0=Direct, 1=Slab）
    char node_id;          // NUMA 节点 ID
    uint8_t hotness;       // 热度级别（0-7）
    uint8_t access_count;  // 访问计数
    uint16_t last_access;  // LRU 时钟低 16 位
    char migrated;         // 迁移亲和标记
    char reserved[1];
} numa_alloc_prefix_t;     // 总计 16 字节
```

### Thread-Local Cache（tcache）

为了消除 free 路径上的位图 CAS 竞争，每线程有一份独立、完全无锁的缓存：

```c
#define TCACHE_BIN_MAX     64   // 每个 size class 的 tcache 容量
#define TCACHE_DRAIN_COUNT 32   // 每次 drain 释放的对象数
```

tcache 的计数一致性规则（这是历史上出现过 `used_memory_node` 变成负数的地方，现
在的规则是刻意设计的，不是随意的）：

- **put 时**：只把对象放进 tcache，**不递减** `used_memory`/`used_memory_node`；
- **hit 时**：只从 tcache 取出对象，**不递增**统计；
- **drain/flush 时**：真正把对象释放回 slab，**这时才递减**统计。

这样保证同一块内存的"计数"只在它真正离开进程可用内存时才变化一次，不会因为在
tcache 里进出而被重复计数。

### 分配 / 释放路径

分配请求先看 `should_use_slab(size)`：`size ≤ 64KB` 走 Slab 路径，否则走 Direct
路径（直接 `numa_alloc_onnode`）。Slab 路径：二分查找 size class → 查 tcache（命
中且节点匹配直接返回）→ 遍历 `partial_slabs`，位图找空闲 bit → 找不到就加锁，从
`empty_slabs` 取一个，或新建一个 slab（小 slab 直接 `numa_alloc_onnode`，大 slab
走上面的 mmap+mbind+munmap 流程）。

释放路径：先看能否放进 tcache；放不进的走真正释放——按大小判断是大 slab
（`ptr_addr & ~(2MB-1)`）还是小 slab（`ptr_addr & ~(64KB-1)`）找到 slab 基址，验
证魔数，原子清除位图对应 bit，`free_count` 自增；如果 slab 从 full 变成
partial，需要持锁把它挪到 partial 链表；如果整个 slab 都空了，则视情况归还给系
统（大 slab `munmap`，小 slab `numa_free`）。

## 质量与性能特性 (Quality & Performance Characteristics)

- **分配/释放复杂度**：位图 CAS 操作，O(1)；tcache 命中路径完全无锁。
- **并发模型**：33 个 size class 各有独立锁，只在 slab 在 partial/full/empty 之
  间切换链表时才持锁；不同 class 之间完全并行，同 class 内的位图操作用原子 CAS
  而不是锁。
- **内存效率**：大 slab 设计消除了 >4KB 对象的 page 对齐浪费（实测：100 万个
  8KB 对象从约 3.9GB 浪费降到约 7.8MB slab 内存开销）。
- **降级路径**：`numa_pool_available()` 返回 0 时（单节点系统或未装 libnuma），
  上层分配回退到标准 `malloc`，`NUMA` 命令族返回友好错误，不会崩溃。
- **可观测性**：`NUMA CONFIG STATS` 暴露按节点的 `allocations`/`bytes`/`live` 计
  数，以及全局的 `alloc_slab_bytes`/`alloc_direct_bytes`/`alloc_slab_count`/
  `alloc_direct_count`。

## 与其他模块的关系 (Relations to Other Modules)

按 `ARCHITECTURE.md` 的依赖顺序，`numa_pool` 处在最底层：

```
libnuma → numa_pool → numa_migrate → numa_key_migrate → ... → server.c
```

- 依赖：仅依赖 `libnuma`（`numa_alloc_onnode`/`numa_free`/`mbind`）。
- 被依赖：`zmalloc.c` 的集成层（见 `modules/zmalloc_numa.md`）直接调用
  `numa_slab_alloc`/`numa_slab_free` 作为 `zmalloc`/`zfree` 的 NUMA 路径；
  `numa_migrate`/`numa_key_migrate` 在迁移时读取/改写 PREFIX 里的节点与热度字
  段；`numa_command` 的 `NUMA CONFIG STATS` 读取本模块维护的统计计数。

## 未解决问题与已知限制 (Open Issues & Known Limitations)

- 本文档所述的"两级 Slab + tcache + 位图管理"设计，与 `ARCHITECTURE.md`/
  `CLAUDE.md` 目前文字描述的"bump-pointer O(1) 分配 + <30% 利用率 chunk 压缩"
  **不一致**——经核对当前 `src/numa_pool.c`/`.h`，源码中不存在任何 bump-pointer
  分配器或按利用率阈值触发的 chunk 压缩逻辑（`grep -i "bump\|compact"` 只命中一
  处无关注释）。这两份顶层文档的措辞看起来描述的是本模块更早期的设计，已经过
  时；本文档以实际源码为准。**建议单独修正 `ARCHITECTURE.md`/`CLAUDE.md`（及其
  中文版）里 numa_pool 的这一句描述**，这超出本文档自身的范围，留给后续统一处
  理。
- tcache 的计数一致性修复（v5.0）是针对一个已知历史 bug（`used_memory_node` 出
  现负值）的修复，如果未来再修改 put/hit/drain 三条路径的计数时机，需要重新验
  证这个不变式。
- 大 slab 阈值固定在 class 24（5120B）而不是严格的 4096B——这是一个实现细节上
  的近似（`is_large_slab_class()` 用 `class_idx >= 24` 判断），不是四舍五入误
  差之外的设计缺陷，但阅读代码时容易被"class 24 对应 4KB 边界"这种直觉误导。
