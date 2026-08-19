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
- CAAT 被选为 NUMAflow 的默认策略，但内核里的 Composite LRU 仍然是内核槎位 1
  的默认——两者不是替换关系，而是"内核默认保持简单可预测，NUMAflow 默认追求更
  优"的分层策略，与 ADR-05 是同一个设计哲学。
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
