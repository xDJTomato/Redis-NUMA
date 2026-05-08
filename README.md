本文档介绍 Redis 6.2.21 上新增的 NUMA 扩展功能。原版 Redis 文档见
[REDIS_ORIGINAL_README.md](docs/devlog/original/REDIS_ORIGINAL_README.md)。

项目简介
--------

本项目是 Redis 6.2.21 的修改版本，增加了透明的 NUMA 感知内存分配与 CXL
（Compute Express Link）内存分层功能。所有标准 Redis 命令和 API 保持不变。
扩展在后台运行：每次 `zmalloc` 调用会自动选择最优 NUMA 节点，16 字节的
元数据前缀追踪每个对象的访问热度，后台迁移引擎将热数据迁回本地 DRAM，
冷数据推至 CXL 内存。

项目面向双路服务器和 CXL 内存扩展设备，其中 Node 0 为低延迟 DRAM，
Node 1 为大容量 CXL 内存。

编译
----

需要安装 `libnuma-dev`（Debian/Ubuntu）或 `numactl-devel`（CentOS/RHEL）：

    % cd src
    % make clean && make -j$(nproc)

编译强制使用 `MALLOC=libc` 并链接 `-lnuma`。jemalloc 与 NUMA 分配器不兼容，
不可使用。

运行
----

按常规方式启动服务器：

    % ./src/redis-server redis.conf

在 `redis.conf` 中添加以下配置以启用 NUMA 迁移：

    numa-enabled yes
    numa-migrate-config /path/to/composite_lru.json

验证 NUMA 是否已激活：

    % ./src/redis-cli NUMA CONFIG GET

`composite_lru.json` 配置示例：

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

工作原理
--------

所有内存分配经过两层路径：

* 4 KB 以内的对象由 **Slab 分配器** 处理：64 KB 的 slab，24 级
  jemalloc 风格的 size class（16 B 到 4096 B），3072 位位图管理，
  原子 CAS 操作——完全无锁。

* 超过 4 KB 的对象直接走 `numa_alloc_onnode()` 系统调用。

在返回指针之前，`zmalloc` 在对象头部前置 16 字节的前缀
（`numa_alloc_prefix_t`），记录分配大小、Slab 来源标记、NUMA 节点 ID、
热度等级（0–7）、访问计数和最近访问时间戳。调用者看不到这个前缀——
它位于返回指针的前方，`zfree` 通过简单的偏移量回溯即可找到。

分配目标节点由可配置的策略决定。默认策略是**压力感知权重交错**
（weighted interleave）：每秒 `serverCron` 从
`/sys/devices/system/node/nodeN/meminfo` 读取各节点内存压力，转换为
权重（`max(1, (1 − pressure) × 100)`），通过 `atomicSet` 发布。分配路径
通过 `atomicGet` 读取权重，做加权随机选择——热路径上没有任何锁。

共有十种策略可用：

    local_first         始终选择 node 0
    interleaved         随机选择（每线程独立种子）
    round_robin         线程本地计数器轮询
    weighted            静态权重，短暂持锁
    pressure_aware      选择利用率最低的节点
    cxl_optimized       小对象分配到本地，大对象分配到远端
    weighted_interleave 压力感知权重随机（默认策略，无锁）
    adaptive            尚未实现（回退到 node 0）
    latency_aware       尚未实现（回退到 node 0）

热度追踪与迁移
--------------

每次 `lookupKey` 命中 Key 时，都会调用 `composite_lru_record_access()`。
该函数执行以下操作：

1. 从对象前缀中读取当前热度。
2. 根据空闲时间执行阶梯式惰性衰减：小于 10 秒不衰减；小于 60 秒衰减 1；
   小于 5 分钟衰减 2；小于 30 分钟衰减 3；30 分钟及以上清零。
3. 热度加 1（上限 7）。
4. 将新热度、访问计数和时间戳写回前缀。
5. 将 Key 同步写入字典（`key_heat_map`），供扫描通道迭代。
6. 若热度刚越过迁移阈值，且 Key 位于远程节点，则将 Key 名称的 SDS
   副本插入环形候选池。

迁移由 `serverCron` 每秒触发，通过两个通道执行：

* **快速通道** ——处理候选池。对每个条目重新读取前缀中的当前热度，
  检查目标节点的内存压力和带宽，条件满足则调用
  `numa_migrate_key_by_name()`。

* **扫描通道** ——分批渐进遍历 `key_heat_map`。远程节点上的热 Key
  被拉回本地 DRAM；当本地节点内存压力过高时，本地冷 Key 被推到
  远程节点（CXL）。

实际迁移过程：在目标节点通过 `numa_zmalloc_onnode` 分配新内存，
`memcpy` 复制数据，原子替换 `val->ptr`，释放旧内存。目前仅 STRING
类型适配器完整实现，HASH、LIST、SET、ZSET 适配器为存根。

NUMA 命令
---------

所有操作通过统一的 `NUMA` 命令暴露，分为三个子域：

    NUMA MIGRATE KEY <key> <node>     迁移单个 Key
    NUMA MIGRATE DB <node>            迁移整个数据库
    NUMA MIGRATE SCAN [COUNT <n>]     触发渐进扫描
    NUMA MIGRATE STATS                显示迁移统计
    NUMA MIGRATE RESET                重置统计
    NUMA MIGRATE INFO <key>           显示 Key 的 NUMA 元数据

    NUMA CONFIG GET                   查看当前配置
    NUMA CONFIG SET <param> <value>   设置参数
    NUMA CONFIG LOAD [path]           加载 JSON 配置文件
    NUMA CONFIG STATS                 显示各节点分配统计
    NUMA CONFIG REBALANCE             手动触发重新平衡

    NUMA STRATEGY LIST                列出全部 16 个策略插槽
    NUMA STRATEGY SLOT <id> <name>    将策略插入指定插槽

    NUMA HELP                         显示命令帮助

策略插槽框架支持最多 16 个可插拔策略，按优先级调度（HIGH → NORMAL → LOW）。
Slot 0 是空操作占位策略（LOW 优先级），Slot 1 是 Composite LRU 迁移引擎
（HIGH 优先级），Slot 2–15 可供自定义策略使用。

源码结构
--------

NUMA 模块位于 `src/` 目录，均由 `#ifdef HAVE_NUMA` 保护：

    numa_pool.c/h                  Slab 分配器
    numa_migrate.c/h               块级内存迁移
    numa_key_migrate.c/h           Key 级别迁移，类型适配器
    numa_strategy_slots.c/h        策略插槽框架
    numa_composite_lru.c/h         Composite LRU（默认策略）
    numa_configurable_strategy.c/h 分配策略选择
    numa_command.c                 统一命令接口
    numa_bw_monitor.c/h            节点带宽监控
    evict_numa.c/h                 NUMA 感知驱逐

在 Redis 核心中的集成点：

* `zmalloc.c/h` —— NUMA 可用时，所有分配经由 NUMA 分配器路由，
  16 字节前缀在此写入。
* `server.c` —— `numa_init()` 在 `initServer()` 之前执行；策略和迁移
  模块在其后初始化；`serverCron` 每秒驱动压力权重更新和策略执行。
* `db.c` —— `lookupKey()` 调用 `composite_lru_record_access()`。

测试
----

标准 Redis 测试：

    % cd src && make test

NUMA 基准测试（基于 YCSB，三阶段：填充 → 热点 → 持续）：

    % cd tests/ycsb && ./run_bw_benchmark.sh

环境检查：

    % ./check_numa_config.sh
    % ./diagnose_numa.sh

性能数据
--------

在 QEMU 双节点虚拟机上测量（Node 0 = 4 GB DRAM，Node 1 = 8 GB CXL）：

* 填充阶段吞吐量：约 53K ops/s（weighted interleave 策略）
* 持续迁移吞吐量：约 45K ops/s
* 迁移速率：约 1524 keys/s，零过载阻断
* 内存碎片率：1.04–1.17

分配热路径完全无锁：节点选择使用 `atomicGet`，Slab 分配使用位图上的
原子 CAS，统计计数器使用 `atomicIncr`。

详细文档
--------

设计文档位于 `docs/new/` 目录：

    00-design-proposal.md          项目方案设计
    01-overview.md                 架构概览
    02-numa-pool.md                Slab 分配器内部实现
    03-zmalloc-numa.md             zmalloc 集成与 PREFIX 布局
    04-numa-migrate.md             块级内存迁移
    05-numa-strategy-slots.md      策略插槽框架
    06-numa-composite-lru.md       Composite LRU 双通道设计
    07-numa-key-migrate.md         Key 级别迁移与类型适配器
    08-numa-configurable.md        分配策略框架
    09-numa-command.md             命令参考
    10-call-chain.md               完整调用链路
    11-alloc-path-instrumentation.md  RSS 差异调查
    12-perf-root-cause-analysis.md    吞吐量根因分析
    13-lockfree-alloc-design.md       无锁分配设计

项目状态
--------

已实现：

* Slab 分配器（24 级 size class，原子 CAS，无锁）
* 压力感知权重交错默认分配策略（无锁）
* Composite LRU 双通道迁移
* 16 字节 PREFIX 内联元数据
* 策略插槽框架（16 槽，优先级调度）
* STRING 类型迁移适配器
* 统一 NUMA 命令接口
* JSON 配置热加载
* 带宽监控
* 分配路径无锁化
* NUMA 感知驱逐

尚未实现：

* HASH、LIST、SET、ZSET 类型迁移适配器
* 自适应分配策略
* 延迟感知分配策略
* 基于机器学习的迁移预测

许可证
------

BSD 3-Clause 许可证。详见 [COPYING](COPYING)。基于 Redis 6.2.21 开发。
