# 更新日志

本 fork 的所有重要变更都记录在这里，格式参考
[Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)。

> 本文档是 [`CHANGELOG.md`](CHANGELOG.md) 的中文版本；英文版是权威原文，两者并存。

## [未发布] — 给一个已确认"卡 GitHub runner"的 stock 测试打上 slow 标签

CI 的 `build-and-test` 和 `sanitizer-address` 两个 job 都在最后一个测试文件
`tests/unit/maxmemory.tcl` 上稳定卡住 15 分钟以上，然后以 I/O error 失败——连续
复现了两次，完全一致。本地跑（甚至人为限制成 2 核，对齐 runner 的核数）同一个测试
只要个位数秒。所以在动手改任何代码之前，先验证这到底是不是这个 fork 的代码引入
的：用一个一次性的诊断 GitHub Actions workflow，clone 并构建**完全未修改的上游**
`redis/redis` `7.2.6` tag（零 fork 改动），在同一个 `ubuntu-latest` runner 上只跑
这一个测试文件。结果在完全相同的地方卡住，因为完全相同的原因失败（被 15 分钟的
`timeout` 包装杀掉）。证实这是 `test_slave_buffers` 在 `cmd_count=1000000`
这个用例（100 万条流水线 `SETRANGE`，打向一个被 `SIGSTOP` 冻住的从库）在 GitHub
共享 runner 上本来就有的特性，不是 NUMA fork 引入的回归——这个结论没有导致 fork 里
任何代码被修改。

### 变更

- `tests/unit/test_slave_buffers`（`tests/unit/maxmemory.tcl`）：加上了 `slow`
  标签，这样 `./runtest --tags -slow`（CI 已经在传这个参数）会跳过它。两处调用
  （`slave buffer are counted correctly` 和 `replica buffer don't induce
  eviction`）共用同一个 proc，会一起被跳过。

## [未发布] — 第一次跑通 CI：构建系统与警告修复

第一次真正跑重写后的 `ci.yml`（见下一条）就直接失败了——此前从没有人用 CI 这种更
严格的设置真正构建过这个 fork。

### 修复

- **CI 里的 `make ... REDIS_CFLAGS='-Werror'` 悄悄破坏了 NUMAflow 的构建**。
  `src/Makefile:124` 用 `REDIS_CFLAGS+=-I../numaflow/include` 让 Redis 内核的构建
  能找到 NUMAflow 的头文件——但 GNU Make 不允许 makefile 里的 `+=` 修改一个已经在
  `make` 命令行上设置过的变量，所以在命令行上传入*任何* `REDIS_CFLAGS=...`（不只是
  `-Werror`）都会悄悄丢掉这个 include 路径，导致所有 `#include` 了某个 `nf_*.h` 的
  文件构建失败。本地没能提前发现，因为文档里写的构建命令（`make -j$(nproc)`，不带
  `REDIS_CFLAGS`）根本不会触发这个问题。修复方式是把 `ci.yml` 的 `build-and-test`
  与 `sanitizer-address` job 里的 `REDIS_CFLAGS='-Werror'` 整个去掉，改为完全匹配
  项目实际文档化的构建命令。
- `src/zmalloc.c`：`numa_tcache_free_miss` 这个计数器和它的三个兄弟
  `numa_tcache_alloc_hit`/`numa_tcache_alloc_miss`/`numa_tcache_free_hit` 一起声明，
  但和另外三个不一样，从来没有在任何地方被真正递增过——这是一个真实的埋点缺失
  bug，不是死代码。现在在 `numa_free_with_size()` 里 tcache-free-miss 的那个分支
  （一次池分配的 free 没能塞进 tcache、转而走 slab 归还路径）补上了这次递增。
- `src/numa_pool.c`：删除了 `bitmap_find_first_free()`——一个完全没被调用的、更早
  版本的位图扫描实现，已经被四处调用点都在用的 `bitmap_find_and_set()` 取代。
- `numa_slab_free()`（`src/numa_pool.c`/`.h`）的 `total_size` 和 `node` 两个参数
  函数体内根本没用到（它需要的一切都从指针所在 slab 的 header 里自己算出来）。
  把签名精简成 `(void *ptr)`，并同步更新了 `src/zmalloc.c` 里的两处调用。
- `src/object.c`：`trimStringObjectIfNeeded()` 前面有一行重复的注释起始行（连续两行
  `/* Optimize the SDS string...`）——是一次合并/编辑留下的痕迹，不是有意的文档
  改动。
- `src/server.c`：修复了一处面向客户端错误字符串里的拼写错误（"interact"
  一词多打了一个 "r"），这个笔误让两处 `CLIENT_SLAVE` 拒绝调用点的文案
  彼此不一致（也和 `tests/integration/replication.tcl` 里已经按正确拼写写好的日志
  匹配断言不一致）。
- `tests/unit/test_numa_command.sh`：把循环变量 `strat` 改名为 `strategy_name`
  （`strat` 不是一个单词，是 codespell 的一个误报）。

## [未发布] — 仓库 CI/CD 与开源脚手架清理

### 变更

- `.github/workflows/ci.yml` 从原样照抄的上游 `redis/redis` CI 重写为真正针对本
  fork 的 job：安装 `libnuma-dev` 并构建/测试 Redis 内核（`make test` +
  `runtest-moduleapi`），新增一个覆盖 NUMA 分配器手动内存管理的 ASan 构建
  （`SANITIZER=address`），以及一个在 Linux 和 macOS 上都跑的 NUMAflow 构建+测试
  job。删除了对本项目无意义甚至完全错误的 job：32 位、Debian-old、通用 macOS，
  以及一个 CentOS 7 jemalloc 构建（jemalloc 与 NUMA 分配器不兼容，而且该 job 从未
  安装 `libnuma-devel`）。
- 删除 `.github/workflows/daily.yml` 与 `.github/workflows/external.yml`——上游的
  定时 fleet 回归测试工作流，用 `github.repository == 'redis/redis'` 卡死，在这个
  fork 上从来不会真正运行。
- `.github/workflows/codeql-analysis.yml`——去掉同样的 `redis/redis` 专属门槛，
  让 CodeQL 在本仓库真正跑起来。

### 新增

- `.github/PULL_REQUEST_TEMPLATE.md`。
- 仓库根目录新增 `LICENSE`（与 `COPYING` 内容相同的 BSD-3-Clause 文本），方便习惯
  直接找 `LICENSE` 的工具/人。
- `docs/legacy/`——收纳从仓库根目录移出的 `00-RELEASENOTES`、`MANIFESTO`、`BUGS`、
  `INSTALL`；这些是未经修改的上游 Redis 遗留文件，仅作历史存档保留。

## [未发布] — 相对性能基准：真实放置轨迹 x 标定代价模型（ADR-12）

### 新增

- `numaflow replay --trace <name>=<file.json> ...`（`numaflow/src/nf_cli.c`）：
  新 CLI 子命令，把一份真实放置轨迹（`{key,size,access_count,origin_node,
  final_node}` 的 JSON 数组）喂进 NUMAflow 已有的纯函数代价模型
  （`nf_numa_access_cost`/`nf_numa_migrate_cost`），支持和 `eval` 相同的
  `--cxl-latency-ns`/`--cxl-bandwidth-mbps` 标定，输出和
  `bench_<workload>.json` 的 `migration` 数组同构的 JSON——`numaflow/eval/
  report.py`、`tests/report/generate_full_report.py` 不需要改一行代码即可
  多画一张对比面板。
- `tests/vm/collect_relative_trace.sh`（guest 内运行）+
  `tests/vm/relative_perf_bench.sh`（开发机上运行）：在真实双节点 QEMU
  guest 里对 noop/composite_lru/tinylfu/caat 四个策略采集真实放置轨迹
  （fill 后即时快照 + 手动触发 `NUMA FLOW RUN default` 若干次 + 最终快照），
  取回后跑 `numaflow replay` 产出 `results/bench_relative_perf.json` /
  `bench_relative_perf_cxlcal.json`，并打印"这是建模投影不是实测延迟"的
  双语免责声明。
- 详见 ADR-12（`docs/new/09-architecture-decisions.md`），包括实现过程中
  发现并修正的两个方法论错误：(1) 假设所有 key 起始节点是 0（实际上
  `local_first` 分配策略下起始节点取决于分配调用发生时线程被调度到哪个
  vCPU），(2) `local_hit_ratio` 一开始按 `final_node==origin_node` 算，导致
  任何从不迁移的策略都结构性地恒为 100%，与实际放置质量无关。

## [未发布] — 迁移路径在真实双 NUMA 节点上的首次验证（ADR-11）

在 QEMU 双 NUMA 节点 guest（`tests/vm/boot_numa_vm.sh`）里第一次真正执行迁移路径，
立刻暴露两个此前完全无法被发现的 bug——开发主机只有 1 个 NUMA 节点，
`numa_pool_num_nodes()==1` 导致 `migrations` 恒为 0，整条执行路径零覆盖。

### 修复

- **NUMAflow 驱动的迁移 100% 静默失败**（`applied=0`，尽管策略正确决策了几十次
  迁移）。`numa_migrate_key_by_name()` 内部做 `dictFind(db->dict, keyname)`，而
  `db->dict` 用 SDS 键——`dictSdsHash()` 和 `dictSdsKeyCompare()` 都会对**查找键**
  调用 `sdslen()`。NUMAflow 桥接（`src/numa_flow.c` 的 `numa_flow_apply`）传的是
  `nf_item_t.key`，一个普通 `char[]`，于是 `sdslen()` 把指针**前面**的字节当 SDS
  头读出垃圾长度，每次查找必然 miss（而且是越界读）。原契约"必须传 SDS"只写在
  头文件注释里，而签名是 `const char *`，把这个陷阱完全隐藏了。修复：新增
  `numa_key_migrate_dict_find()` 在函数内部归一化，两种形式都安全。
  实测：修复前 `successful_migrations=0`，修复后 `=50`。
- **`composite_lru` 预设永远不迁移任何数据**（迁移次数恒为 0）。
  `src/numa_flow.c` 把 `br.ctx.tick` 设成完整的 24 位 `server.lruclock`（当前约
  860 万），而 `nf_item_t.recency` 来自 zmalloc 前缀的 **uint16_t** `last_access`
  （只有低 16 位，0–65535）。于是 DAG 里所有 `idle = ctx->tick - it.recency`
  的计算（`op_score_hotness` / `op_decay_hotness`）都得到数百万秒的空闲时间，
  `nf_staircase_decay()` 恒定返回最大衰减值，hotness 被永久压到 3，被
  `filter_hot threshold=5` 全部滤除。`caat`/`tinylfu` 不受影响，因为它们用 CMS
  频率而非 hotness 做门控——这也解释了为什么只有 composite_lru 表现异常。
  修复：把 `ctx.tick` 截断到 16 位以匹配前缀精度。

### 新增

- `tests/vm/placement_quality.sh` — 在真实双节点 guest 里测量**放置质量**的对比
  脚本。测的不是吞吐/延迟（QEMU 的两个 `-numa node` 背后是同一块宿主机 DRAM，
  没有真实延迟差，测不出迁移收益），而是策略把数据放在哪：`hot_local_ratio`
  （热 key 驻留本地节点比例）、`cold_off_ratio`（冷 key 被挪离本地节点比例）、
  实际迁移次数。这个指标不依赖任何延迟建模，跨策略可比。

## [未发布] — NUMAflow 桥接层可扩展性修复（ADR-10）

### 修复

- **`numa_flow_cron()` 每个 tick 都会对整个 keyspace 做一次完整、无上限的
  `dictGetSafeIterator()` 遍历**（`numa-flow-interval-sec`，默认 1 秒）。在几十万
  key 规模的数据集上，这让每个 tick 都变成一次 O(keyspace 大小) 的同步扫描，阻塞
  Redis 单线程的命令处理——服务器对一条普通的 `PING` 都会失去响应达数分钟。这个
  问题在桥接层设计里一直潜伏，但在 ADR-08 让 NUMAflow 默认自动加载并运行之前从未
  被触发（此前是需要手动 opt-in 的，所以没人在真实的大 keyspace 上跑过它）。修复
  方式是换成有界、可恢复的扫描：`dictScan()`（`SCAN` 命令背后同一个原语）每个
  tick 最多累积 `NUMA_FLOW_SCAN_BATCH`（4096）个 key，并把游标持久化在工作流条目
  上，这样对整个 keyspace 的一次完整遍历会分摊到很多个 tick 上，而不是一次做完
  ——这和已退役的原生 composite_lru 模块出于同样的原因使用的"渐进式字典扫描"
  思路一致。
- **`numaflow/src/nf_bridge.c` 内部 `kn_map_t`（用于对比 enumerate 时状态与 DAG
  最终结果的 key -> 原始节点查找表）里一个真实存在的、会把 CPU 跑满的死循环**。
  `kn_init()` 把容量硬编码为约 2049 个槽位，且没有扩容逻辑；`kn_put()` 的线性探测
  插入循环（`while (m->keys[i]) i++`）一旦表满就永远不会终止，因为已经找不到空槽
  了。上面的扫描修复（每 tick 4096 个 key）是第一次真正把调用量推过这个隐藏的
  2049 槽位上限——这个 bug 比本次修复会话本身更早存在，但此前从未被触发过
  （numaflow 自己的基准测试 harness `nf_bench.c` 直接调用 DAG 执行器，完全不经过
  这张表）。修复方式是把 `kn_map_t` 改成一个真正的自动扩容哈希表（负载因子达到
  70% 时翻倍并重新哈希）。通过 `redis-benchmark` 在约 70 万–100 万 key 规模下验证：
  吞吐健康（约 70 万 req/s），`PING` 在整个过程中及之后都保持响应，完整的 Redis
  `make test`、numaflow 自身的测试套件、以及 `test_numa_command.sh` 均通过。完整
  诊断过程见 `docs/new/09-architecture-decisions.md` 的 ADR-10，包括为什么上一次
  session 的修复验证（正确但规模太小）没能捕获这个问题。

## [未发布] — NUMA 迁移策略收敛（ADR-08）

### 变更（破坏性）

- **三个迁移策略（`caat`/`composite_lru`/`tinylfu`）现在唯一存在于 NUMAflow
  原子操作引擎中**（`numaflow/src/nf_strategy.c`）——内核原生实现
  （`src/numa_strategy_slots.{c,h}`、`src/numa_composite_lru.{c,h}`、
  `src/numa_tinylfu.{c,h}`）已被删除。详见
  [`docs/new/09-architecture-decisions.md`](docs/new/09-architecture-decisions.md)
  的 ADR-08。
- `NUMA STRATEGY` 命令整体移除（slot 的 insert/enable/disable/schedule/
  status/list）。改用 `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT` 以及新增的
  `NUMA FLOW DEFAULT <caat|composite_lru|tinylfu|noop>`。
- 移除 `numa-migrate-config` / `NUMA CONFIG LOAD`（composite-lru JSON 热重载）。
  替换为 `numa-flow-default-strategy`（默认 `caat`）与
  `numa-flow-interval-sec`，启动时自动作为 NUMAflow 的 `default` 工作流条目
  加载——现在不需要手动 `NUMA FLOW LOAD` 就能得到迁移行为。`composite_lru.json`
  仍保留在仓库中，但只作为字段名参考；内核不再读取它。
- `NUMA CONFIG GET`/`NUMA MIGRATE STATS` 的响应不再包含
  `access_tracking_enabled`/`locality_stats_enabled`/`debug_logging_enabled`/
  `composite_*`/`accesses_local`/`accesses_remote`/`tinylfu_*` 字段（它们的数据
  来源已不存在）。`NUMA MIGRATE SCAN` 现在改为执行一次 NUMAflow 的 `default`
  条目，而不是一次 composite-lru 扫描；`COUNT` 参数为兼容旧 CLI 而保留，但会被
  忽略。
- 默认迁移行为从 Composite LRU 改为 CAAT（Cost-Aware Adaptive Tiering）——
  ADR-04 自己的评测显示 CAAT 的净代价比 Composite LRU 低约 37%。

### 修复

- **TinyLFU 和 CAAT 通过 Redis 集成（`src/numa_flow.c`）实际上从未真正迁移过
  任何数据**，这个问题在本次收敛之前就已经存在——`cms_estimate`（Count-Min
  Sketch 的频率读取）永远返回 0，因为桥接层里没有任何地方调用
  `nf_tracker_observe()`（CMS 的写入入口）；只有 numaflow 自己的独立基准测试
  harness（`numaflow/src/nf_bench.c`）会调用它，这也是为什么 `ADR-04` 的评测
  数字是真实的，但真实的桥接路径却一直静默失效。修复方式是让追踪器从
  `numa_key_migrate_touch()` 同一条真实访问路径喂数据：新增
  `numa_flow_observe_access()`（`src/numa_flow.c`/`.h`），在 `src/db.c` 里每次
  真实 key 访问时调用。
- **CAAT 还会丢失每一个被降级、但没有同时满足晋升条件的 item**
  （`numaflow/src/nf_strategy.c` 的 `build_caat`，以及 `numaflow/src/nf_adapt.c`
  里等价的 `NF_ADAPT_AGGRESSIVE` 模板）。`nf_exec_run()` 的结果只是图中所有
  *终止（sink）节点*输出的并集；旧版是一条线性链，降级阶段的变更直接喂给晋升
  阶段的过滤器，于是一个被降级但没通过晋升过滤条件的 item，会在到达任何终止
  节点之前就被丢弃——它的降级其实已经真实执行过，但桥接层从未看到、也从未
  上报/应用。修复方式是在两个阶段各自发生任何变更之前，先按每个 item 的原始
  驻留位置分叉图结构（通过 `filter_local`/`filter_remote` 原子操作），让每个
  item 最多被变更一次，并且总能到达且仅到达一个终止节点。用一个独立的
  harness 直接驱动真实的桥接/引擎代码端到端验证了修复效果。
- 之前，per-key 热度追踪（zmalloc 分配前缀上的 `numa_get_hotness`/
  `numa_get_access_count`）只在（现已移除的）Composite LRU/TinyLFU 槎位启用时
  才会更新。现在通过 `numa_key_migrate_touch()`
  （`src/numa_key_migrate.c`/`src/db.c`）无条件更新，这样无论当前启用哪个（或
  不启用任何）迁移策略，NUMAflow 的 `enumerate()` 都能拿到一个真实信号。
- `numa_configurable_strategy` 的 `PRESSURE_AWARE` 分配策略此前读取
  `numa_config_get_node_utilization()`——一个无上限的、以 GB 为单位的字节计数
  器，而不是一个 0.0–1.0 的比例——却拿它去和一个 `1.0` 的最小种子值比较，一旦
  任何节点分配超过 1GB 就会悄悄破坏节点排序。现在改为读取 `evict_numa` 同样
  使用的、规范的 `numa_bw_get_node_pressure()` 信号。
- `evict_numa.c` 和 `numa_configurable_strategy.c` 曾各自维护一套独立的
  节点压力计算逻辑，公式和缓存 TTL 都不同。现已合并为单一的
  `numa_bw_get_node_pressure()`（`src/numa_bw_monitor.c`）。
- `WEIGHTED_INTERLEAVE` 曾是 `WEIGHTED` 加权随机节点选择循环的逐字节复制，
  唯一区别只是读取哪个权重数组。现在两种情况共用同一个
  `select_weighted_node()` 辅助函数。

## [未发布] — `feat/redis7-port` 分支上的 Redis 7 迁移

### 变更

- **Redis 内核从 6.2.21 迁移到 7.2.6**，通过一次真正的三方合并完成（用一次合成的
  嫁接（graft）操作，给这个历史全新的 fork 根提交人工接上一个与上游共同的祖先）。
  完整记录——工具链、发现并修复的每一个 bug、验证方式——见
  [`docs/redis7-migration.md`](docs/redis7-migration.md)。
- 全部 10 个 NUMA 模块都已适配 Redis 7 的 dict API（不透明的 `dictEntry`、
  `dictCreate()` 去掉 `privdata` 参数、回调函数签名改为 `dict *d` 打头）、
  listpack（与遗留的 ziplist 并存）、以及 quicklist 的 `->entry` 字段改名。
- 新增 `src/commands/numa.json`，用 Redis 7 的声明式命令自省系统注册 `NUMA` 命令。
- 文档重写以匹配合并后的真实状态：`README.md`、新增的 `ARCHITECTURE.md`、新增的
  `TESTING.md`、`CONTRIBUTING.md`、本更新日志，以及取代
  `docs/redis8-migration.md` 的 `docs/redis7-migration.md`。

### 新增

- `run_full_validation.sh` —— 单一入口脚本：编译、单元测试、NUMAflow 基准测试，
  以及（可选的）下面的 YCSB、QEMU、CXLMemSim 步骤，最终产出一份汇总的 HTML 报告。
- `tests/vm/boot_numa_vm.sh` —— QEMU（TCG）多 NUMA 节点冒烟测试：启动一个 2 节点
  的模拟客户机，并在其中运行 `redis-server` + `NUMA` 命令族 + `redis-benchmark`。
- `tests/cxl/run_cxlmemsim.sh` —— 校验 CXLMemSim（`SlugLab/CXLMemSim`，
  `external/CXLMemSim`）的设备仿真链路：跑通它自己的 CTest 套件，并确认一个 QEMU
  CXL Type2 端点通过 TCP 成功连接到 `cxlmemsim_server`。
- `tests/cxl/cxlmemsim_workload_bench.cpp` —— 直接通过 CXLMemSim 自己的
  `CXLMemExpander` C++ 模型重放与 NUMAflow 评测框架相同的
  zipf/uniform/hotspot/temporal 工作负载形态，提供一个与 NUMAflow 简化成本模型
  并列对比的真实设备模型基准点。接入过程中发现的两个 CXLMemSim 自身缓存失效顺序
  相关的真实 bug（选择绕过而非直接修补，因为 `external/CXLMemSim` 未被 vendor 进
  本仓库历史）详见 `ARCHITECTURE.md`。
- `numaflow eval --cxl-latency-ns`/`--cxl-bandwidth-mbps` —— 用一次真实 CXLMemSim
  设备链路运行测得的数字，校准 NUMAflow 自身的成本模型，替代 `numa_shim.c` 里合成
  的 tier-1 默认值；`run_full_validation.sh` 现在会以两种方式各跑一遍每种工作负载
  （`results/bench_<workload>.json` 与
  `results/bench_<workload>_cxlcal.json`）。
- `tests/report/generate_full_report.py` —— 把各步骤结果与 NUMAflow 的
  `bench_*.json` 数据汇总成一份内嵌 SVG 图表、无外部依赖的单文件 HTML 报告（扩展了
  `numaflow/eval/report.py` 的思路）；现在也会把经 CXLMemSim 校准的 NUMAflow 运行
  结果和 CXLMemSim 原生模型结果，与合成默认值的对比一起画出来。

### 修复

`git merge` 的 recursive 策略在核心合并过程中静默引入（零冲突标记）的 6 个 bug，
新测试套件恰好暴露出的 1 个历史遗留分配器保护缺失 bug，以及 1 个晚于 `7.2.6` 这个
tag 才发布的上游 CVE。逐文件的完整细节见
[`docs/redis7-migration.md`](docs/redis7-migration.md)：

- `src/zmalloc.c` —— 一个 `HAVE_NUMA` 代码块后面缺失的 `#endif`。
- `src/dict.c` —— 一段多余的重复溢出检查，引用了一个已经被移除的变量。
- `src/server.c` —— 重复的 `afterCommand()` 定义；另外，`call()` 函数里重复的
  `replicationFeedMonitors()` 调用，导致 MONITOR 客户端收到每条命令两次。
- `src/networking.c` —— 重复的 `addReplyBigNum()` 和
  `deferredAfterErrorReply()` 定义。
- `src/server.h`/`src/evict.c`/`src/evict_numa.c` —— 过时的 2 参数
  `objectComputeSize()` 原型与实际的 4 参数签名不一致，导致驱逐降级路径中的一次
  SIGSEGV。
- `src/zmalloc.c` —— `numa_alloc_dram()`（`zmalloc_local`/`zcalloc_local`/
  `ztrycalloc_local()` 底层依赖它）从未检查 `numa_ctx.numa_available`，导致除
  `redis-server` 之外、任何创建 dict 的二进制都会堆损坏（表现为
  `tests/unit/cluster/cli.tcl` 下的 `redis-cli` SIGSEGV）。
- `src/hyperloglog.c` —— cherry-pick 了 CVE-2025-32023 的修复
  （`f35b72dd1`），该修复晚于 `7.2.6` tag 才发布。
- `src/config.c` —— `numa-demote-min-size` 用 `createIntConfig()` 注册，但对应
  字段实际是 `size_t`；改为 `createSizeTConfig()`。

### 移除

- `src/redis8_compat.h` —— 死代码；整个仓库没有任何文件 include 它，真正的迁移
  路径直接走的是 Redis 7 的原生 API。
- `docs/redis8-migration.md` —— 被 `docs/redis7-migration.md` 取代。

## 更早的历史

Redis 7 迁移之前的一切（NUMA 模块本身的实现、NUMAflow 的引入、经 Linux/QEMU 验证
的 6.2.21 基线）都早于本更新日志。详细的提交历史见 `git log`——`9ff536438`
（`checkpoint(redis-6.2): Linux/QEMU verified NUMA 6.2 baseline`）是一个方便的标记
点，代表"Redis 7 迁移开始之前，最后一个已知良好的状态"。
