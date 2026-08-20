# 9. 架构决策记录（Architecture Decisions）

> arc42 第 9 章：记录那些"一旦做了就很难轻易撤回、且理由不写下来后人会重新纠结
> 一遍"的关键决策。格式统一为 **问题 → 考虑过的选项 → 决策 → 后果**。

## ADR-01：为什么强制 `MALLOC=libc`，不用 jemalloc

**问题**：Redis 默认在 Linux 上用 jemalloc（分配效率、碎片率都优于 glibc
malloc）。本项目要不要保留这个默认值？

**考虑过的选项**：
- A. 保留 jemalloc，NUMA 分配器只在 jemalloc 的 arena 机制之上做一层 NUMA 绑定；
- B. 强制切换到 libc malloc，NUMA 分配器完全接管 `zmalloc` 层。

**决策**：选 B。`src/Makefile` 第 103–110 行强制 `MALLOC=libc` 并链接 `-lnuma`。

**后果**：
- 优点：`numa_pool` 可以完全掌控每一次分配落在哪个 NUMA 节点、能在分配对象头部
  内联 16 字节 `numa_alloc_prefix_t` 元数据（热度、节点、大小），这是运行时迁移
  能力的基础；不需要理解或兼容 jemalloc 内部的 arena/tcache 状态机。
- 代价：放弃了 jemalloc 本身在通用场景下的碎片控制优势，改由 `numa_pool` 自己的
  33 级 size class + slab + chunk 压缩来承担（见
  [`modules/numa_pool.md`](modules/numa_pool.md)）。
- **如果日后有人在 `src/Makefile` 里改动编译选项，重新引入 jemalloc 会直接破坏
  NUMA 分配器**——两个分配器会互相踩踏同一块内存的元数据。这是 `CLAUDE.md`
  Key Gotchas 里明确标出的第一条。

## ADR-02：为什么用真实的 git graft 三方合并，而不是从零重写

**问题**：把 Redis 核心从 6.2.21 升级到 7.2.6，同时保留全部 NUMA 模块，有两条路：
逐个手动比对文件差异重新应用改动，或者让 Git 做一次真正的三方合并。

**考虑过的选项**：
- A. 手动 diff 每个被 NUMA 模块修改过的文件，把改动"翻译"到 7.2.6 对应位置；
- B. 让 Git 计算真正的三方合并，但本仓库导入时是一个全新的根提交，与
  `redis/redis` 没有共同历史，直接 `git merge` 会被当成两棵不相关的树；
- C. 用 `git replace --graft` 给本仓库的根提交接上一个指向上游 6.2.21 的假父
  提交，让 Git 能计算出正确的合并基点，再做真正的三方合并。

**决策**：选 C。

**后果**：
- 优点：Git 的合并算法能自动处理绝大多数无冲突的改动，比手动逐文件翻译快得多，
  且更贴近真实、可复现的工程实践。
- 代价（且是本项目吃到的真实教训）：**"没有冲突标记"不代表"合并正确"**。这次
  合并里有 6 个真实 bug 在零冲突标记的情况下混入代码，只有靠完整编译 + 完整测试
  套件才发现——完整记录见 [`docs/redis7-migration.md`](../redis7-migration.md)。
  这直接催生了 ADR-06。
- 选 7.2.6 而不是更新的 8.x：7.2.6 是许可证从 BSD-3-Clause 改为 RSALv2/SSPL 双
  许可（7.4 起）之前的最后一个稳定版本，同时已经包含影响 NUMA 模块的关键 API
  变化（不透明 `dictEntry`、listpack、quicklist 容器类型），既拿到了需要的改动，
  又不必处理更大的 8.x 重构，也保持在本项目一直使用的许可证下。

## ADR-03：为什么是 16 槎位可插拔框架，而不是硬编码一个策略

**问题**：迁移策略要不要直接写死在 `serverCron` 里？

**考虑过的选项**：
- A. 硬编码一个"最优"策略，简单直接；
- B. 设计一个基于 vtable 多态的槎位框架，允许同时注册多个策略、按需启停切换。

**决策**：选 B——`numa_strategy_slots` 提供 16 个槎位，槎位 0 固定空操作，槎位 1
默认装载 Composite LRU，槎位 2 可选装载 TinyLFU。

**后果**：
- 优点：研究/对比不同迁移算法不需要相互覆盖或来回改 `server.c`；`NUMA STRATEGY
  SLOT <id> <name>` 可以在运行时切换；每个槎位独立统计、独立开关，一个槎位的
  bug 不会直接拖垮其他槎位。
- 代价：框架本身的复杂度（vtable、槎位状态机、调度模式选择）比"就写一个函数"
  高得多，且需要一份清晰的模块依赖顺序文档（见
  [`05-building-block-view.md`](05-building-block-view.md)）来避免初始化顺序
  错误——这类顺序错误是本项目最常见的启动期崩溃原因。
- Composite LRU 和 TinyLFU 不能同时启用：两者都想对同一批 key 的迁移决策拥有
  控制权，同开会互相打架，因此设计上要求手动二选一。

## ADR-04：为什么 CAAT（晋升+降级）取代 Composite LRU/TinyLFU 成为 NUMAflow 的新默认策略

**问题**：Composite LRU（基于最近访问的双通道热度识别）和 TinyLFU（基于历史
频率的 Count-Min Sketch 估计）都已经能工作，还需要设计新策略吗？

**考虑过的选项**：
- A. 保持现状，两个策略互补即可；
- B. 设计一个同时具备晋升与降级能力的新策略。

**决策**：选 B。两者的共同局限是**只升不降**——一旦 DRAM 写满就无法继续晋升新
的热点，因为都没有"主动把不再热的数据挪出 DRAM"这一步。CAAT
（Cost-Aware Adaptive Tiering）用 `benefit = (cost(当前节点) - cost(目标节点))
× 访问率 - 迁移代价` 这个净收益公式统一决定晋升与降级：只有净收益为正的 key
才晋升，同时主动降级 DRAM 上不再热的 key。

**后果**：
- 实测（zipf 分布 / 3000 key / 12 万次访问 / DRAM 容量约束为工作集 50%）：CAAT
  本地命中率 91.1%，净代价比 TinyLFU 低约 20%，比 Composite LRU 低约 37%，代价
  是迁移次数略高（换回来的是命中率的明显提升）。这组对比之所以可信，依赖
  NUMAflow 公平评测框架对拓扑/负载/轨迹/预算/随机源五个维度的一致性约束（见
  [`docs/numaflow/README.md`](../numaflow/README.md)）。
  **[ADR-09](#adr-09为什么-tinylfucaat-通过-redis-桥接实际上从未真正迁移过数据发现于-adr-08-落地后的全策略回归测试两个独立-bug均在合并前修复) 更新**：
  这组数字仅覆盖 zipf 这一种工作负载。用仓库自带的四个基准工作负载
  （zipf/uniform/hotspot/temporal）逐一实测后发现 CAAT 的优势是**工作负载
  依赖**的——在 zipf/hotspot 这类有明显冷热分层的负载上确实全面更优，但在
  uniform（访问均匀、没有真正的"冷"数据）上净代价反而比 Composite LRU 高约
  31%（`demote_cold` 主动搬走"冷"数据这个动作，在没有真正冷热差异时变成纯粹的
  迁移开销浪费），temporal 负载上两者基本打平。完整数据见 ADR-09。
- CAAT 被选为 NUMAflow 的默认策略，[ADR-08](#adr-08为什么撤销adr-0304的分层把三个迁移策略统一收敛到-numaflow)
  之后也是内核唯一且默认自动加载的迁移策略——ADR-08 之前这里写的是"内核默认
  Composite LRU、NUMAflow 默认 CAAT，两者分层不冲突"，那个立场已被撤销。
- 相关工作对比：TieredMemDB（Intel 的 NUMA/PMEM 分层分支）选择的是相反的哲学
  ——分配时决策、不运行时迁移，简单但无法适应访问模式变化。这组对比进一步印证
  了"运行时热度感知 + 双向迁移"这条路线的合理性，也提示了两个可借鉴的点（动态
  比例反馈环、关键结构强制 DRAM），完整分析见
  [`appendix/related-work-tieredmemdb.md`](appendix/related-work-tieredmemdb.md)。

## ADR-05：为什么 `ADAPTIVE`/`LATENCY_AWARE` 在内核里是占位，真身放在 NUMAflow

**问题**：`numa_configurable_strategy` 的 9 种分配策略里，`ADAPTIVE` 和
`LATENCY_AWARE` 需要根据运行时反馈动态调整决策，这类逻辑应该直接写进内核的
`zmalloc` 路径吗？

**考虑过的选项**：
- A. 在内核里实现完整的自适应逻辑；
- B. 内核只保留占位（接口存在、行为退化为简单策略），完整实现放在 NUMAflow 的
  `alloc_adaptive`/`alloc_latency_aware` 原子操作里，通过 `NUMA FLOW` 桥接。

**决策**：选 B。

**后果**：
- 优点：`zmalloc` 是极高频调用路径，任何额外分支和状态都会被放大成实际开销；
  把复杂度隔离到独立子系统，出问题时不会波及 Redis 主进程的稳定性，也能更快
  迭代（NUMAflow 是纯 C11、无 Redis 依赖，改了就能测，不需要每次都重新编译整个
  Redis 内核）。
- 代价：功能"完整"这件事分裂在两个代码库里，文档必须明确指出"内核里看到的只是
  占位，别在这里找完整实现"——这条提示已经写进
  [`docs/README.md`](../README.md) 的事实核对表和本文档集的
  [`05-building-block-view.md`](05-building-block-view.md)。

## ADR-06：为什么"合并没有冲突标记"不能作为合并正确的证据（流程决策）

**问题**：ADR-02 选择了真三方合并，但合并本身怎么验证是对的？

**考虑过的选项**：
- A. 合并完成、无冲突标记即视为完成；
- B. 无冲突标记之后，仍然要求完整编译 + 完整测试套件通过才算完成，并把每一个
  编译错误当作"可能是合并静默损坏了代码"去核查，而不是想当然地当成正常的 API
  迁移工作量。

**决策**：选 B，这是本项目在实际吃到 6 个零冲突标记的合并 bug 之后定下的强制
流程（完整清单见 [`docs/redis7-migration.md`](../redis7-migration.md)）。

**后果**：
- `make -j$(nproc)` 编译到底 + `make test` 全量通过，成为任何跨版本合并的强制
  验收标准，写进了 [`CONTRIBUTING.md`](../CONTRIBUTING.md)。
- 衍生规则：grep 编译日志要看完整日志，不能只看 `head -30`——本项目就有两个
  bug（`dictType` 回调签名不匹配、`createIntConfig`/`createSizeTConfig` 字段
  宽度不匹配）是被截断的日志漏掉、重新完整 grep 才抓到的。
- 衍生规则：选定合并基点（如 7.2.6）不代表选到了完全打齐的安全基线，必须单独
  核查已知 CVE——本项目就在合并后单独 cherry-pick 了 CVE-2025-32023
  的 HyperLogLog 越界写修复。

## ADR-07：为什么 AE time-event 调度是每槎位可选模式，而不是整体替换 serverCron

**问题**：`serverCron` 每秒统一驱动所有策略槎位，如果某个策略执行较慢，会拖长
整个 `serverCron` 周期、影响所有其他定时任务。要不要把全部槎位调度整体迁移到
Redis 自身的 AE 事件循环（`aeCreateTimeEvent`）？

**考虑过的选项**：
- A. 整体从 `serverCron` 迁移到 AE time event 调度；
- B. 保留 `serverCron` 作为默认调度方式，新增 AE 调度作为**逐槎位可选**的另一种
  模式，两者共存，通过 `NUMA STRATEGY SLOT SCHEDULE <id> ae|servercron` 切换。

**决策**：选 B。已验证为真实实现（不是纸面设计）：`src/numa_strategy_slots.h`
定义了 `NUMA_STRATEGY_SCHED_SERVERCRON`/`NUMA_STRATEGY_SCHED_AE` 和
`execute_step(strategy, deadline_us, budget)` 接口，`src/numa_strategy_slots.c`
里 `aeCreateTimeEvent()` 的调用和调度模式判断均已落地。

**后果**：
- 优点：不强迫所有已有策略立刻迁移到新的 `execute_step()` 接口（迁移成本和
  验证成本都不小），同时给需要更细粒度调度（deadline、budget、可续跑）的新策略
  一条渐进路径。
- 代价：两条调度路径长期共存意味着两套语义都要维护、两套语义的边界要在文档里
  写清楚——完整的调度模型、执行语义和分阶段落地路线见
  [`modules/ae_strategy_scheduler.md`](modules/ae_strategy_scheduler.md)。
- 风险：AE time event 本身仍然运行在 Redis 的单线程事件循环里，"挂到 AE"并不
  等于"变成异步/多线程"，只是换了一种更细粒度可控的调度节奏——这一点在
  [`11-risks-and-technical-debt.md`](11-risks-and-technical-debt.md) 中单独标注，
  避免被误读为并发能力的提升。

## ADR-08：为什么撤销 ADR-03/04/05 的分层，把三个迁移策略统一收敛到 NUMAflow

**问题**：ADR-03/04/05 把"内核原生 vtable 槎位框架（Composite LRU 默认开、
TinyLFU 默认关）"和"NUMAflow DAG 引擎（CAAT 默认）"设计成两套并存的分层实现——
内核保持简单可预测，NUMAflow 追求最优、可独立快速迭代。这个分层现在还站得住吗？

**发现**（三路并行代码调研的结论，细节见对应模块文档）：
- 槎位2（TinyLFU）在实践中完全不可达：没有任何 config/redis.conf 入口打开它，
  唯一路径是运维手敲 `NUMA STRATEGY ENABLE 2`；ADR-03 承诺的"两者不能同时启用"
  互斥保护也从未落地成代码。
- `nf_strategy.c` 的 `build_composite_lru`/`build_tinylfu`/`build_caat` 早就把
  内核原生的 Composite LRU 和 TinyLFU 按原算法拆成了和 CAAT 同样的原子操作链
  （`score_hotness`/`filter_hot`/`cms_estimate`/`filter_freq`/`budget_limit`/
  `select_dest_node`/`emit_migrate`，定义于 `numaflow/src/nf_ops.c`）——三个策略
  事实上已经有两份实现，一份手写 C，一份原子操作 DAG，两份要同步维护。
- ADR-04 自己的评测已经证明 CAAT 净代价比 Composite LRU 低 37%、比 TinyLFU 低
  20%，但更好的实现默认不跑：`numa_flow_cron()` 只跑
  `enabled && interval_sec > 0` 的工作流，而 NUMAflow 桥接初始化时不会自动
  LOAD 任何工作流——"内核默认保持简单"实际上变成了"内核默认使用较差的实现"。
- `numa_key_migrate_touch()`（原 `composite_lru_record_access` 内联在
  `db.c` 里的一部分）曾经是 NUMAflow `enumerate()` 读取真实热度数据的唯一入口，
  且被锁在槎位1/2 是否 enabled 的判断之后——删除内核原生实现前必须先把这个钩子
  解耦成中立、无条件调用，否则 NUMAflow 会瞬间读到全 0 的热度。

**决策**：撤销 ADR-03/04/05 的分层结论，把三个策略（`caat`/`composite_lru`/
`tinylfu`，外加 `noop`）统一收敛到 NUMAflow 的原子操作框架，不重新实现任何算法：
- 删除 `src/numa_strategy_slots.{c,h}`（连带 ADR-07 的逐槎位 AE/servercron 调度
  切换，调度对象本身没了）和 `src/numa_composite_lru.{c,h}`、
  `src/numa_tinylfu.{c,h}`。
- 新增 `numa-flow-default-strategy`（默认 `caat`）+ `numa-flow-interval-sec`
  配置项，`numa_flow_init()` 之后自动用 `nf_strategy_build()` 把默认策略登记为
  NUMAflow 的 `default` 工作流条目，随 `numa_flow_cron()` 一起跑——不再需要手动
  `NUMA FLOW LOAD` 才能得到迁移行为。`NUMA FLOW DEFAULT <name>` 命令支持运行时
  在三个预设间切换，不需要写 JSON 文件。
- `numa_key_migrate_touch()`（`src/numa_key_migrate.c`）把热度追踪钩子从
  "槎位1/2 是否 enabled" 的判断中解耦出来，改为 `db.c` 的访问路径无条件调用，
  写入 zmalloc 分配前缀这一个中立的 ground truth；composite_lru 自己的
  `key_heat_map`/热点环形缓冲和 `numa_key_migrate` 自己的 `hotness_level` 惰性
  衰减两套额外的影子状态一并退役。

**后果**：
- 优点：三个策略只剩一份实现（NUMAflow 原子操作 DAG），改一处就同步生效于
  "内核默认路径"和"实验对比路径"，不再有两份代码要保持语义一致的负担；默认
  行为从"较差但简单"的 Composite LRU 变成 ADR-04 已验证更优的 CAAT。
- 代价：`NUMA STRATEGY` 命令、`numa-migrate-config`/`composite_lru.json` 配置面
  被整体移除，是一次破坏性变更；依赖这些接口的历史脚本/测试需要迁移到
  `NUMA FLOW` 等价命令（见 [`CHANGELOG.md`](../../CHANGELOG.md)）。
- ADR-07（AE/servercron 逐槎位调度）随槎位框架删除一起失效——NUMAflow 的调度
  模型（`numa_flow_cron()` 按 `interval_sec` 判断是否该跑）是唯一剩下的调度路径，
  单线程 serverCron 驱动，没有 AE time event 变体。
- ADR-05 的核心判断（复杂自适应逻辑不进 zmalloc 热路径）继续有效，未被撤销：
  `numa_configurable_strategy.c` 的 `ADAPTIVE`/`LATENCY_AWARE` 仍是内核内的占位，
  完整实现仍然只存在于 NUMAflow 侧；唯一变化是内核现在会在设置时打日志、在
  `NUMA CONFIG GET` 的 `strategy_note` 字段里明说"这是占位，完整实现见
  NUMAflow"——过去这个分层只写在 arc42 文档里，现在代码自己也会说。

## ADR-09：为什么 TinyLFU/CAAT 通过 Redis 桥接实际上从未真正迁移过数据（发现于
## ADR-08 落地后的全策略回归测试，两个独立 bug，均在合并前修复）

**问题**：ADR-08 把三个迁移策略统一收敛到 NUMAflow 之后，"这三个策略现在唯一
且真实的实现"这个前提本身是否站得住？逐策略跑一遍真实回归测试（而不只是命令
层的 smoke test）后发现：不站得住——TinyLFU 和 CAAT 在通过 Redis 桥接
（`src/numa_flow.c`）运行时实际上什么都不迁移，这两个 bug 在 ADR-08 之前就已经
存在于 `numaflow/src/nf_strategy.c`，只是从未被真实的桥接路径触发过（此前
Composite LRU 是内核默认、TinyLFU 从未启用、CAAT 只能靠手动 `NUMA FLOW LOAD`
加载），ADR-08 把 CAAT 提升为默认之后才第一次暴露。

**发现 1：CMS 频率估计从未被写入过**。`nf_ops.c` 的 `cms_estimate` 读取
`nf_tracker_freq(ctx->tracker, key)`，但整条 Redis 桥接路径里没有任何地方调用
`nf_tracker_observe()`（唯一的写入入口）——只有 numaflow 自己的独立公平评测
（`numaflow/src/nf_bench.c`）在每次模拟访问时手动调用它。也就是说 TinyLFU 这部分
的评测数字是真实的（评测 harness 自己喂了 tracker，TinyLFU 的图没有"发现 2"
描述的那种丢结果问题），但同一份图跑在真实 Redis 桥接下，`freq_est` 永远是
0——TinyLFU 的 `filter_freq threshold=2` 会把所有候选过滤到空，CAAT 的
`demote_cold`/`filter_freq` 也读同一个永远为 0 的信号。

**决策 1**：给 `numa_flow.h`/`.c` 新增 `numa_flow_observe_access(key)`，从
`src/db.c` 里和 `numa_key_migrate_touch()` 完全同一个真实访问路径调用——这是
Redis 桥接里唯一调用 `nf_tracker_observe()` 的地方，语义上对齐 `nf_bench.c`
"每次真实访问时喂一次"的方式，而不是"每个 cron 周期把整个 keyspace 喂一遍"
（那样会把"存在"和"被访问"混为一谈，语义不对）。

**发现 2（更深）：CAAT 单链设计会把已经执行过的降级操作从结果里丢失**。
`nf_exec_run()` 的最终结果是所有*没有出边的终止节点*输出的并集；旧版 `build_caat`
是一条线性链：`demote_cold→emit_migrate(降级)→filter_freq→filter_benefit→...
→emit_migrate(晋升)`。凡是被降级但没通过晋升阶段过滤条件的 item，会在到达任何
终止节点之前被 `filter_freq`/`filter_benefit` 丢弃——它的降级明明已经真实发生
（`current_node` 已变、`ctx.stats.migrations_done` 已计数），但因为再也没有
"活" 到图的终点，桥接层基于"入队时的原始节点 vs 结果里的最终节点"做 diff 的
逻辑（`nf_bridge_run`）完全看不到它，宿主的 `apply()`（真正执行迁移的回调）
永远不会被调用。用一个跨两节点的独立 harness 直接驱动真实引擎代码验证：旧版
CAAT 在"全冷"场景下报告迁移数为 0，尽管内部 `migrations_done` 已经是 50。

**决策 2**：在两个阶段各自发生任何变更**之前**先按原始驻留位置分叉——DRAM 上
的 item 只会走降级子链（决策后跟一个终止 `emit_migrate`，之后不再有过滤），
非 DRAM 的 item 只会走晋升子链（先过滤候选，最后才变更，变更后不再有任何节点）。
`numa_flow.c`/`nf_adapt.c` 里 `NF_ADAPT_AGGRESSIVE` 模板用同一手法修复，因为它
是同一个 bug 的另一份拷贝。

**验证**：`/tmp/nf_tinylfu_probe.c`（临时验证程序，未纳入仓库）直接调用
`nf_bridge_run` 驱动真实的 `nf_strategy_build("tinylfu"/"caat"/"composite_lru")`
图，用 10 个"热但在远端节点"的 key + 40 个"冷但在本地节点"的 key 构造合成场景：
修复前 TinyLFU 报告 0 次迁移、CAAT 报告的迁移数只覆盖降级方向且数值随场景变化
（说明晋升方向从未生效）；修复后 TinyLFU 正确晋升全部 10 个热 key，CAAT 正确
同时完成 40 次降级 + 10 次晋升，`composite_lru`（对照组，从不依赖 tracker）在
两组场景下行为完全一致，证明两个修复精确命中问题、没有牵连无关策略。
`numaflow/tests/test_smoke.c`、`test_adapt.c` 里硬编码的节点/边数量和节点 id
断言已同步更新为新拓扑的真实值。

**遗留事项（已用仓库自带的四个基准工作负载全部复测，结论比预想的更细：bug 的
影响确实真实存在，而且 CAAT 的优势本身是有条件的）**："发现 2"描述的单链丢
结果问题，`numaflow/src/nf_bench.c`（ADR-04 数字的来源）自己也会中——它和
`nf_bridge_run` 一样，只从 `nf_exec_run()` 的 `ex.result`（终止节点输出的并集）
读取最终状态来更新自己的 `st[]` 驻留位置表，同样的单链拓扑下，被降级但没通过
晋升过滤的 cold item 一样会在 `st[]` 里丢失这次状态变化。

最初只拿 ADR-04 的原始参数（zipf 工作负载）手动跑了一次修复后的二进制，命中率
91.03%（原文 91.1%），代价对比也和原文的 ~37%/~20% 几乎一致，一度以为这个 bug
对聚合结果影响可忽略。但这只是和 ADR-04 的**文字描述**对比；用 `make bench`
重新生成仓库里实际提交的四份基准文件（`results/bench_{zipf,uniform,hotspot,
temporal}.json`，同一组固定参数：3000 key / 120000 次访问 / epoch 3000 /
budget 256 / 2 节点 / seed 20240517）并与修复前**提交到仓库里的旧文件**逐项
对比后，发现旧文件本身就已经和 ADR-04 的文字描述不一致（旧 zipf 命中率其实是
84.57%，不是 91.1%——说明 ADR-04 的原始数字可能来自一次未被存档、且这两个 bug
都不存在的特定手动运行，而后来提交进仓库的基准文件是在这两个 bug（尤其是"发现
2"）已经引入之后重新生成的，从未和 ADR-04 的文字对上过）。修复后 vs. 修复前
（仓库旧文件）的真实差距很大，不是噪声：

| 工作负载 | CAAT 命中率（修复前→后） | CAAT 净代价（修复前→后） |
|---|---|---|
| zipf | 84.6% → 91.0% | 184.2M → 84.8M（↓54%） |
| uniform | 18.0% → 49.0% | 388.5M → 166.5M（↓57%） |
| hotspot | 76.1% → 89.6% | 185.4M → 79.3M（↓57%） |
| temporal | 48.9% → 57.3% | 307.8M → 153.6M（↓50%） |

修复后 CAAT 一致大幅优于修复前的自己（净代价普遍降低约一半），"发现 2"这个
bug 确实严重拖累了 CAAT，值得修。但把修复后的 CAAT 拿去跟 composite_lru/tinylfu
横向比较，会发现 ADR-04"CAAT 全面更优"的结论需要加一个条件——比较的只有 zipf
一种工作负载：

| 工作负载 | CAAT vs Composite LRU 净代价 | CAAT vs TinyLFU 净代价 |
|---|---|---|
| zipf | 低 36.9%（CAAT 更优） | 低 19.9%（CAAT 更优） |
| hotspot | 低 39.5%（CAAT 更优） | 低 16.4%（CAAT 更优） |
| uniform | **高 31.1%（CAAT 更差）** | 低 12.2%（CAAT 更优） |
| temporal | **高 3.3%（CAAT 更差）** | 低 16.4%（CAAT 更优） |

CAAT 在有明显冷热分层的负载（zipf、hotspot）上优势和 ADR-04 描述的一致；但在
uniform（访问均匀分布，没有真正的"冷"数据）上反而明显更差——`demote_cold` 主动
把"冷"数据搬走这个动作，在没有真正冷热差异的负载下变成纯粹的浪费迁移开销。
temporal 负载上两者基本打平（CAAT 略差 3.3%）。

**结论**：ADR-08 把 CAAT 设为默认这个决策本身不需要撤销（多数真实 Redis 场景
的访问分布是偏态的，接近 zipf/hotspot 而不是 uniform），但"CAAT 全面优于两者"
不是一个无条件成立的结论，应理解为"CAAT 在访问分布有明显冷热分层时更优，在
访问接近均匀分布时反而更差"。`results/report.html` 和 `results/bench_*.json`
已用修复后的二进制重新生成并提交；ADR-04 引用这组数字时应注明工作负载依赖性，
不要再引用未注明工作负载的单一百分比。

**追加发现（ADR-10 之后，用 `run_full_validation.sh` 自带的 20000 key /
budget=64 参数复测才发现）**：上面"uniform 负载下 CAAT 更差"这个结论本身还有
一层没写全的前提——它只在 budget 相对 keyspace 比较宽松时成立（本节用的是
256/3000≈8.5%）。换成 `run_full_validation.sh` 的标准参数（20000 key /
budget=64，比例≈0.32%，更接近真实 Redis 场景里"迁移预算远小于键空间规模"的
情况）复测，CAAT 在全部四个工作负载（包括 uniform）上净代价都低于 Composite
LRU/TinyLFU，见 `docs/test/benchmark_results.txt` 第 8 节的完整数字。原因：
budget 越宽松，`demote_cold` 主动搬离"冷"数据这个动作在没有真正冷热差异的
uniform 负载下就越容易变成纯粹浪费的迁移开销；budget 收紧后这个副作用被自然
限流。所以完整结论应该是"CAAT 在访问分布有明显冷热分层、或迁移预算相对键空间
偏紧时更优；只有在访问接近均匀分布**且**迁移预算相对键空间又比较宽松这两个
条件同时成立时，才会不如更简单的 Composite LRU"——不是单一维度的"workload
dependent"，是 workload 和 budget/keyspace 比例两个维度共同决定的。

## ADR-10：为什么真实规模测试（而不是命令层 smoke test）是必需的 —— 一个真正的
## 死循环，此前从未被触发过

**问题**：ADR-09 修完两个 bug 后，`NUMA FLOW` 的命令分发、`NUMA FLOW DEFAULT`
切换、小规模 smoke test 全部通过；这是否等于"Redis 集成路径已经可以放心使用"？

**发现**：不等于。用真实的 `redis-benchmark`/YCSB 往一个跑着默认 CAAT 策略的
Redis 实例灌入几十万到百万级别的 key 后，**服务器会在几十万 key 规模上完全失去
响应——连 `PING` 都拿不到回复，进程持续占满一个 CPU 核心，state 一直是
`R`（运行态，不是阻塞态）**。用 `_serverLog`/`fprintf`+`fflush` 逐节点插桩定位
后发现：卡住的不是 DAG 的任何一个算子，而是 `numaflow/src/nf_bridge.c` 里
`nf_bridge_run()` 内部一个不起眼的 `key -> 原始节点` 查找表（`kn_map_t`）——
`kn_init(&map, 1024)` 把它的容量硬编码为 `1024*2+1=2049` 槽位，且 `kn_put()`
只有线性探测（`while (m->keys[i]) i++`），**没有任何扩容逻辑**。一旦某次运行
入队的 item 数量超过这个容量（本次触发条件正是 ADR-08 里为避免"每 tick 全表
扫描"新增的 `NUMA_FLOW_SCAN_BATCH=4096`——批大小本身没错，但恰好超过了这个从未
被检验过的隐藏上限），表被填满后 `kn_put` 的线性探测循环就再也找不到空槽位，
变成一个**真正的、CPU 占满的死循环**——不是变慢，是永远不返回。这个 bug 在
`nf_bridge_run()` 里从 NUMAflow 项目一开始就存在，此前从未被触发，原因有三：
① Redis 集成此前是纯 opt-in（需要手动 `NUMA FLOW LOAD`），没有人在真实大数据集
上长期跑过它；② numaflow 自己的公平评测 harness（`nf_bench.c`）走的是完全不同
的代码路径（直接调用 `nf_exec_run`，根本不经过 `nf_bridge_run`/`kn_map_t`），
所以 ADR-04/ADR-09 的评测数字从未受这个 bug 影响；③ 本次会话早前的验证（ADR-08
的功能验证、ADR-09 的独立 harness 验证）用的都是几十个 item 的合成场景，远低于
2049 这个隐藏阈值。

**决策**：把 `kn_map_t` 改成正确的、会扩容的哈希表——`kn_put()` 在装载因子超过
70% 时先 `kn_grow()`（容量翻倍并重新哈希所有已有条目）再插入，不再依赖调用方
猜一个"足够大"的初始容量。同时保留 `NUMA_FLOW_SCAN_BATCH=4096` 的设计不变（它
本身没有问题，问题是它第一次让代码路径吃到了超过 2049 的规模）。

**验证**：修复后用 `redis-benchmark -n 1000000 -r 1000000 -t set -P 50`（约
68 万去重后的 key）实测：1.4 秒内完成插入（~70 万 req/s），插入期间及之后
`PING` 全程秒回，`NUMA FLOW STATUS default` 显示每个 tick 正确处理了 4096 个
item。Redis 全量 `make test`、`numaflow` 自身的 `make test`、
`tests/unit/test_numa_command.sh`（61/61）三者复测全部通过。

**教训（写给后来者，也写给自己）**：ADR-09 当时的结论"两个 bug 已修复、经独立
harness 验证"是真实但不完整的——**验证的规模不够**。命令层 smoke test 和几十个
item 的合成场景能证明"逻辑走对了路径"，但不能证明"这条路径在真实数据量下不会
撞上一个从未被检验过的容量假设"。这正是本仓库 ADR-06 那条教训（"没有冲突标记
不代表合并正确"）的同构版本：**没有在小规模测试里报错，不代表在真实规模下没有
错误**。这也是为什么这次刷新 `docs/test/benchmark_results.txt`/`results/*.json`
时改用了仓库 `run_full_validation.sh` 自带的 YCSB 百万级 3 阶段基准（Fill→
Hotspot→Sustain）而不是仅仅停留在 numaflow 自己的合成评测——真实 Redis 集成路径
的规模验证，必须走真实的、大数据量的端到端基准，两者缺一不可。

## ADR-11：迁移路径在真实双 NUMA 节点上的首次验证，发现两个零覆盖的 bug

**问题**：ADR-10 修完扫描/哈希表两个 bug 后，`NUMA FLOW` 在百万级 key 规模下不再
死循环、不再无响应；这是否等于"迁移这条路径本身是对的"？

**发现**：不等于——而且此前完全无法验证，因为开发主机只有 1 个 NUMA 节点
（`numa_pool_num_nodes()==1`），`current_node` 永远是 0，`select_dest_node` 永远
选不出第二个候选节点，`migrations` 永远是 0。也就是说，"决策"这一半（哪些 key
该迁移）此前每次都测过，但"执行"这一半（真的把数据挪到另一个节点）**从 ADR-08
把三策略收敛进 NUMAflow 以来，从未被真正跑过一次**。用
`tests/vm/boot_numa_vm.sh` 起一个真实的 QEMU 双 NUMA 节点 guest 后，第一次跑
`caat`/`composite_lru`/`tinylfu`，立刻暴露两个此前零覆盖的 bug：

1. **NUMAflow 驱动的迁移 100% 静默失败**（`NUMA FLOW STATUS` 显示策略正确决策了
   几十次迁移，但 `NUMA MIGRATE STATS` 的 `successful_migrations` 恒为 0）。根因：
   `numa_migrate_key_by_name()`（`src/numa_key_migrate.c`）内部直接
   `dictFind(db->dict, keyname)`，而 `db->dict` 用 SDS 键——`dictSdsHash()`/
   `dictSdsKeyCompare()` 都会对**查找键**调用 `sdslen()`。NUMAflow 桥接
   （`src/numa_flow.c` 的 `numa_flow_apply`）传的是 `nf_item_t.key`，一个普通
   `char[]` 而非 SDS，`sdslen()` 把指针前面的字节当 SDS header 读出垃圾长度，
   于是每次查找必然 miss（而且是一次越界读）。旧接口签名是
   `const char *keyname`，"必须传 SDS"这个契约只写在头文件注释里，签名本身没有
   任何强制，把这个陷阱完全隐藏了。
2. **`composite_lru` 预设永远不迁移任何数据**（`caat`/`tinylfu` 不受影响）。
   `src/numa_flow.c` 把 `br.ctx.tick` 设成完整的 24 位 `server.lruclock`
   （当时约 860 万），而 `nf_item_t.recency` 来自 zmalloc 前缀的 **uint16_t**
   `last_access`（只有低 16 位）。DAG 里所有 `idle = ctx->tick - it.recency` 的
   计算（`op_score_hotness`/`op_decay_hotness`）因此都得到数百万秒的"空闲时长"，
   `nf_staircase_decay()` 恒定返回最大衰减值，hotness 被永久压到最低档，被
   `filter_hot threshold=5` 全部滤除。`caat`/`tinylfu` 用 CMS 频率而非 hotness
   做门控，所以只有 `composite_lru` 表现异常——这也是本次调查最初的线索。

**决策**：
- 新增 `numa_key_migrate_dict_find()`，在函数内部用 `sdsnew(keyname)` 归一化后再
  查找、再释放，让 `numa_migrate_key_by_name()` 同时接受真实 SDS 和普通 C
  字符串两种调用方，而不是要求调用方自己记住这个约定。
- 把 `br.ctx.tick` 截断到 16 位（`server.lruclock & 0xFFFF`）以匹配
  `nf_item_t.recency` 的实际精度。

**验证**：新增 `tests/vm/placement_quality.sh`，在 QEMU 双节点 guest 内部对每个
策略做同一套确定性实验——写 400 个 key，只对前 40 个（"热" key）打 8000 次 GET，
静置后统计"热 key 是否驻留在本地节点"、"冷 key 是否被挪离本地节点"、以及实际迁移
次数。之所以测这两个比例而不是吞吐/延迟：QEMU 的两个 `-numa node` 背后是同一块
宿主机 DRAM，没有真实的跨节点延迟差，测不出迁移的性能收益；但"策略把数据放在哪"
是可以直接观测、不依赖任何延迟建模、且跨策略可比的信号——"迁移收益 = 放置质量
× 节点延迟差"，这里只测第一个因子。修复前后对比：

```
                        修复前              修复后
successful_migrations   0                   50
composite_lru 迁移次数   0（全部策略都是 0）  17

修复后四策略对比：
noop           hot_local= 21/40  ( 52.5%)  cold_off=131/360 ( 36.4%)  migrations=0
composite_lru  hot_local= 40/40  (100.0%)  cold_off=157/360 ( 43.6%)  migrations=17
tinylfu        hot_local= 40/40  (100.0%)  cold_off=146/360 ( 40.6%)  migrations=11
caat           hot_local= 40/40  (100.0%)  cold_off=360/360 (100.0%)  migrations=40
```

三个真实迁移策略都能把 100% 的热 key 留在本地节点；但只有 CAAT 同时把 100% 的冷
key 挪离本地节点——`composite_lru`/`tinylfu` 在这套引擎里只做"促升"（promote）、
不做"降级"（demote），这与 ADR-04 当初的评测结论（CAAT 是三者中唯一同时执行
promote 和 demote 的策略）在真实硬件上首次得到独立验证，而不只是 numaflow 自己
合成评测里的数字。修复后复测 Redis 全量 `make test`、
`tests/unit/test_numa_command.sh`（61/61）、`numaflow` 自身 `make test`，三者
全部通过。

**教训**：这是 ADR-10 那条教训的又一次同构重现——**这次缺的不是"规模"，是
"节点数"**。开发主机长期只有 1 个 NUMA 节点，意味着"决策"和"执行"两个阶段里，
执行阶段（真实跨节点搬移数据）从这套统一框架存在以来从未被跑过一次，无论此前
的测试规模多大、覆盖率看起来多完整。`numa_pool_num_nodes()==1` 时
`migrations` 恒为 0 不是"通过"，是"这条路径没有被执行"——这两者在测试报告里
长得一样，但含义完全不同。这也解释了为什么这两个 bug 能在一次消费了 ADR-08/09/10
全部修复的干净代码上，仍然是首次出现：它们只在"迁移真的会发生"时才会被触发，而
"迁移真的会发生"本身需要 ≥2 个 NUMA 节点这个此前从未满足过的前提条件。
