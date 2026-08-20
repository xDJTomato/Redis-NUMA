# 架构（中文版）

> 本文档是 [`ARCHITECTURE.md`](ARCHITECTURE.md) 的中文版本，内容与英文原版保持同步；
> 英文版为权威原文，如需引用具体行号请以英文版为准。更详细、面向初学者的讲解见
> [`docs/GUIDE.zh-CN.md`](docs/GUIDE.zh-CN.md) 第 3 章。

## NUMA 模块层（叠加在 Redis 内核之上）

`src/` 下共八个模块，全部由 `#ifdef HAVE_NUMA` 包裹（16 槎位 vtable 策略框架及其
原生的 Composite LRU / TinyLFU 实现已经退役——见
`docs/new/09-architecture-decisions.md` 的 ADR-08）：

1. **numa_pool**（`numa_pool.c/h`）——自定义内存分配器。33 个尺寸类（8B~64KB），
   基于原子位图管理的两级 Slab 分配（小/大 slab），配合 Thread-Local Cache 实现
   无锁快速路径。
2. **numa_migrate**（`numa_migrate.c/h`）——通过 `numa_zmalloc_onnode` + `memcpy`
   实现 NUMA 节点间的底层块迁移（注：实际的按 key 迁移由下面的 numa_key_migrate
   独立实现，并不调用这个模块的迁移函数）。
3. **numa_key_migrate**（`numa_key_migrate.c/h`）——按 key 迁移（以一个 `robj` 为
   单位）。`numa_key_migrate_touch()` 在每次真实访问时无条件更新中立的 zmalloc
   前缀热度信号——这是 NUMAflow 的桥接（`numa_flow.c`）通过 `enumerate()` 读取的
   唯一 ground truth。为全部 5 种 Redis 类型提供完整的类型适配器：STRING（RAW/
   EMBSTR）、HASH（listpack/ziplist/hashtable）、LIST（quicklist，区分 LZF 压
   缩/原始，以及 `QUICKLIST_NODE_CONTAINER_PLAIN`/`PACKED` 两种节点容器子路径）、
   SET（intset/hashtable）、ZSET（listpack/ziplist/skiplist）。
4. **numa_configurable_strategy**（`numa_configurable_strategy.c/h`）——在
   `zmalloc` 层提供 7 种独立的分配节点选择行为（LOCAL_FIRST、INTERLEAVE、
   ROUND_ROBIN，WEIGHTED/WEIGHTED_INTERLEAVE 共用同一份加权随机实现、只是权重
   来源不同，PRESSURE_AWARE、CXL_OPTIMIZED）。ADAPTIVE/LATENCY_AWARE 是内核侧
   的占位（行为等同 LOCAL_FIRST；通过启动日志和 `NUMA CONFIG GET` 的
   `strategy_note` 字段自报告）——真正的实现是 NUMAflow 里对应的
   `alloc_adaptive`/`alloc_latency_aware` 原子操作。
5. **numa_command**（`numa_command.c/h`）——统一的 `NUMA` 命令：`NUMA MIGRATE`、
   `NUMA CONFIG`、`NUMA FLOW`，通过 `src/commands/numa.json`（Redis 7 的
   声明式命令自省系统）注册。
6. **numa_bw_monitor**（`numa_bw_monitor.c/h`）——实时的每节点带宽监控
   （resctrl/numastat/manual 三种后端），以及被 `evict_numa` 和
   `numa_configurable_strategy` 共用的唯一权威节点压力取值函数
   （`numa_bw_get_node_pressure()`），让两者对节点负载的判断不会互相矛盾。
7. **evict_numa**（`evict_numa.c`，接口声明在 `evict.h` 里——并没有独立的
   `evict_numa.h`）——NUMA 感知的淘汰：在真正淘汰 key 之前，
   先把它降级到压力更小的节点。加权评分：距离（40%）+ 压力（30%）+ 带宽（30%）；
   压力信号来自 `numa_bw_monitor`。
8. **numa_flow.c**（仅 `HAVE_NUMA` 下编译）——Redis 侧接入 NUMAflow 原子操作引擎
   的桥接层。迁移策略（`caat`/`composite_lru`/`tinylfu`/`noop`）*只*在这里以
   NUMAflow 的 DAG 预设形式实现（`numaflow/src/nf_strategy.c`）——不再有任何内
   核原生实现。实现了 `nf_bridge.c` 所要求的两个桥接回调，启动时按
   `numa-flow-default-strategy`（默认 `caat`）自动加载为 `default` 工作流条目
   （除非 `numa-enabled no`），并暴露 `NUMA FLOW
   LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT/DEFAULT`。`serverCron` 会按配置的时间间
   隔运行已加载的工作流。

## 与 Redis 内核的关键接触点

- **`zmalloc.c`/`zmalloc.h`**——所有 `zmalloc`/`zfree`/`zrealloc` 在 NUMA 可用时
  都会被路由到 NUMA 分配器。每次分配都携带一个 16 字节的 `numa_alloc_prefix_t`
  前缀，记录大小、节点、热度和访问元数据。`NO_MALLOC_USABLE_SIZE` 被强制启用。
  `dict.c` 使用的 `zmalloc_local()`/`zcalloc_local()`/`ztrycalloc_local()`
  同样会对 `numa_ctx.numa_available` 做判断——和 `zmalloc()`/`zcalloc()` 一样——
  因为并不是每一个 Redis 二进制（`redis-cli`、`redis-benchmark`、
  `redis-check-rdb`/`aof`、`redis-sentinel`）都会调用 `numa_init()`。
- **`server.h`**——`redisServer` 结构体上的 NUMA 统计计数器与配置字段。NUMA 头
  文件在 `#ifdef HAVE_NUMA` 下被引入。
- **`server.c`**——`numa_init()` 在 `main()` 中、`initServer()` **之前**运行。
  按键迁移/带宽监控/NUMAflow 桥接的初始化在 `initServer()` **之后**运行，
  NUMAflow 的默认策略也在这里自动加载。周期性的策略执行发生在 `serverCron` 里。
- **`evict.h`/`evict.c`**——在真正淘汰一个 key 之前，淘汰循环里插入了一次无状态
  的降级尝试调用（`evictionTryNumaDemote`）；`evictionPoolEntry` 结构体本身未被
  修改。

## 模块依赖顺序（从下到上）

```
libnuma
  -> numa_pool
    -> numa_migrate
      -> numa_key_migrate
        -> numa_bw_monitor
          -> numa_configurable_strategy
            -> numa_flow（NUMAflow 桥接）
              -> numa_command
                -> evict_numa
                  -> server.c
```

新增模块时请遵循这个顺序，详细的分步清单见
[`CONTRIBUTING.md`](CONTRIBUTING.md)。

## NUMAflow 子系统（`numaflow/`）

纯 C11 实现，不依赖 Redis/libnuma——可以在任何带 C11 编译器的平台上构建和测试：

```bash
cd numaflow && make && make test && make report
./build/numaflow ops          # 列出 36 个原子操作
./build/numaflow strategies   # 列出 13 个内置策略（CAAT 为默认策略）
./build/numaflow eval --workload zipf --cxl-latency-ns 125 --cxl-bandwidth-mbps 25000
./build/numaflow replay --trace caat=trace_caat.json --trace noop=trace_noop.json
python gui/server.py          # N8N 风格的 DAG 编辑器，http://127.0.0.1:8090
```

目录结构：

- `include/` + `src/`——引擎本身（`nf_ops.c` 对应 36 个原子操作，`nf_strategy.c`
  是包含 CAAT 的策略目录，`nf_bench.c` 是基于合成访问轨迹的公平评测框架，
  `nf_cli.c` 是 CLI 分发（`ops/strategies/templates/template/workflow/run/
  dump-ops/dump-templates/eval/replay`），`nf_track.c` 是
  CMS + Doorkeeper + EWMA 反馈回路，`nf_bridge.c` 是与存储无关的桥接契约与迁移
  应用逻辑，`nf_adapt.c` 是自适应 DAG（参数爬山 + 结构选择），`numa_shim.c` 是
  可移植的 libnuma 仿真层——也提供 `eval` 和 `replay` 共用的纯函数代价模型
  `nf_numa_access_cost`/`nf_numa_migrate_cost`）。`replay` 把一份**真实**放置
  轨迹（不是合成的）喂进 `eval` 用的同一套可标定代价模型，产出和 `eval` 的
  `bench_<workload>.json` 同构的结果。
- `tui/nf_tui.c`——交互式 TUI。
- `gui/`——基于 Web 的 DAG 编辑器及其 Python 桥接后端。
- `eval/report.py`——纯 stdlib 实现的 SVG/HTML 报告生成器，用于展示公平评测
  框架产出的 `bench_*.json`。
- `tests/`——单元测试、桥接/自适应测试与冒烟测试。

## 外部验证层

模块树之外还有两个可选的、尽力而为（best-effort）的验证层，在当前环境不支持时
会优雅降级（记录日志并跳过，绝不伪造结果）：

- **`tests/vm/boot_numa_vm.sh`**——在 QEMU 里启动一个带 2 个模拟 NUMA 节点的小型
  云镜像（如果没有 `/dev/kvm` 就用纯 TCG 软件模拟），并在其中运行
  `redis-server` + `NUMA` 命令族 + `redis-benchmark`，用一个真实（即便是模拟的）
  多节点拓扑去实际跑一遍 NUMA 代码路径。
- **`tests/vm/placement_quality.sh`** / **`tests/vm/relative_perf_bench.sh`**
  （需要 `boot_numa_vm.sh --keep` 保持 guest 运行）——上面那个冒烟测试不会跑
  够久、跑够真实的迁移策略去验证放置效果，这两个工具在同一个真实（即便延迟是
  平的）≥2 节点拓扑上补上这一环。`placement_quality.sh` 在 guest 内按策略测量
  热 key 是否留在本地、冷 key 是否被挪走（第一次这样跑的时候就测出并修复了真实
  迁移执行路径里两个此前零覆盖的 bug——SDS key 查找、tick/recency 截断，见
  ADR-11）。`relative_perf_bench.sh` 编排全部四个策略，从每个策略采集一份真实
  的按 key 放置轨迹，喂进 NUMAflow 标定过的代价模型（`numaflow replay`），算出
  一个*建模*的相对 ns 级投影——不是实测延迟，因为 QEMU 的两个 `-numa node` 背后
  是同一块宿主机 DRAM（见 ADR-12）。
- **`external/CXLMemSim`**（`SlugLab/CXLMemSim`，未被 vendor 进本仓库历史）——一
  个设备级的 CXL 内存时序仿真器，自带改版 QEMU。`tests/cxl/run_cxlmemsim.sh` 用
  来验证 QEMU↔`cxlmemsim_server` 的链路（一个 CXL Type2 端点通过 TCP 连接到
  server 并交换仿真拓扑）。此外还实际尝试过一次客户机启动测试（用发行版自带、
  已经内置了 `cxl_pci`/`cxl_acpi`/`cxl_mem` 内核模块的 Debian 12 云镜像，跑在挂
  了 `cxl-type2` 端点的 CXLMemSim 改版 QEMU 之上）：客户机能在 PCI 总线上正确识
  别出设备（`0d:00.0 CXL [0502]: Intel Corporation Device [8086:0d92]`），但发
  行版自带的 `cxl_pci` 驱动无法绑定这个设备（`echo ... > bind` 返回
  `I/O error`，dmesg 里没有任何日志）——CXLMemSim 自己的
  `qemu_integration/launch_qemu_vcs_dcd_gfam.sh` 脚本证实这是预期行为：它默认使
  用的 `KERNEL_IMAGE` 是一个自定义打过补丁的
  `/root/linux-cxl-type2/arch/x86/boot/bzImage`——也就是说，要把这块 Type2 设备
  的内存暴露成客户机可见的 RAM/NUMA 容量，需要用 CXLMemSim 自己打过补丁的
  Linux 源码树编译出来的内核（本环境中没有），而不是一个普通的发行版内核。因
  此，`redis-server` 在这里从未真正碰到过 CXL 仿真内存；构建那个专用内核被判断
  为超出这一轮验证的范围。如果需要真实 Redis 层面的 DRAM-vs-远端内存对比，见
  `tests/ycsb/scripts/eval_cxl_memory.sh`，它在 2 个真实 NUMA 节点间用
  `numactl --membind`（在上面提到的 VM 里可以这样跑；本 fork 自己的开发主机只
  有 1 个物理 NUMA 节点）。

同一个脚本还会运行 `tests/cxl/cxlmemsim_workload_bench.cpp`，它直接通过
CXLMemSim 自己的 `CXLMemExpander::calculate_latency`/`calculate_bandwidth`
C++ 模型（链接 `libcxlmemsim.a`）重放 NUMAflow 的四种工作负载形状
（zipf/uniform/hotspot/temporal），而不是走 NUMAflow 简化的单一延迟/单一带宽
成本模型。如果你要改动这个文件，有两点值得了解：

- 如果不先对轨迹里的每次访问调用 `CXLMemExpander::insert()`，直接跑
  `calculate_latency`/`calculate_bandwidth`，会让模型对工作负载的"形状"完全不
  敏感（这是实测发现的——最初的版本里，四种工作负载会输出逐位相同的结果）：
  `insert()` 才是把"第一次访问某地址"分类为 store、"重复访问"分类为 load 的
  地方，而这个 load/store 比例正是 `calculate_bandwidth()` 拥塞模型真正读取的
  输入。bench 会在调用任一计算函数之前，先把整条轨迹跑一遍 `insert()`，这正是
  让偏斜负载（zipf/hotspot：对一个小热点集合的大量重复访问）产生和 uniform 明
  显不同数字的原因。
- `calculate_latency()` 内部会先调用 `update_address_cache()`，这一步会设置
  `is_address_local()` 检查的同一个 `cache_valid` 标志，但**不会**填充
  `address_ranges` 这个真正被读取的向量——在一个全新的 endpoint 上，如果先调用
  `calculate_latency()`，之后两个计算函数都会静默返回 0。bench 的做法是先调用
  `calculate_bandwidth()` 作为规避。这是 CXLMemSim 自身缓存失效逻辑里的一个真
  实 bug，不是我们这边用错了；由于 `external/CXLMemSim` 没有被 vendor 进本仓
  库，我们选择绕过而不是去 patch 它。
- NUMAflow 自己的模型也可以用真实采集到的 CXLMemSim 数字来校准，替换
  `numa_shim.c` 里合成的默认一级参数，方式是
  `numaflow eval --cxl-latency-ns <n> --cxl-bandwidth-mbps <n>`（参见
  `run_full_validation.sh` 里 NUMAflow 那一步，它会用两种方式各跑一遍每种工作
  负载，把校准后的结果写到 `results/bench_<workload>_cxlcal.json`，和合成默认
  值的 `results/bench_<workload>.json` 放在一起）。

这些验证层如何融入 `run_full_validation.sh`，见 [`TESTING.md`](TESTING.md)。
