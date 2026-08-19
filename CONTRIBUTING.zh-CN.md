# 贡献指南

本分叉遵循标准的 GitHub PR 工作流。本文档只讲**本项目特有的约定**——如何给 NUMA
模块和 NUMAflow 子系统写代码；至于通用的 Redis 贡献礼仪（issue 分类、邮件列表、
安全问题上报流程），请参考 [`SECURITY.md`](SECURITY.md) 中链接的上游约定。

## 新增一个 NUMA 模块

1. 在 `src/` 下先创建 `.h`（接口/结构体声明），再创建 `.c`（实现）。
2. 把 `numa_xxx.o` 加进 `src/Makefile` 的 `REDIS_SERVER_OBJ` 里。**NUMA 的 `.o`
   文件必须排在 `server.o` 之后**，这是链接顺序的硬性要求。
3. 在 `server.h` 里用 `#ifdef HAVE_NUMA` 包住头文件的 `#include`。
4. 在 `server.c` 里调用该模块的初始化函数，且必须在 `initServer()` **执行完之
   后**——现有的每一个 NUMA 模块都依赖 `initServer()` 建好的状态，而 `numa_init()`
   本身则必须在 `initServer()` **之前**运行。这个先后顺序一旦搞反，是本分叉里最
   常见、也最难排查的启动期崩溃原因。
5. 用 `extern void _serverLog(int level, const char *fmt, ...)`，不要直接调用
   `serverLog()`——这是 Redis 内部既有的约定。参考现有模块（`numa_composite_lru.c`、
   `numa_bw_monitor.c` 等）里的写法。
6. 遵守 [`ARCHITECTURE.md`](ARCHITECTURE.md#module-dependency-order-bottom-to-top)
   里记录的模块依赖顺序：
   `libnuma -> numa_pool -> numa_migrate -> numa_key_migrate ->
   {numa_composite_lru, numa_tinylfu, numa_strategy_slots} -> numa_command
   -> evict_numa -> server.c`。

## 提交 PR 之前

- `cd src && make clean && make -j$(nproc)` 必须零错误、且不能引入新的编译警告。
- 在仓库根目录跑 `make test`，必须通过完整的 Tcl 测试套件。
- 如果你改动了 NUMAflow（`numaflow/`），`cd numaflow && make test` 也必须通过——
  它跟 Redis/libnuma 完全无依赖，是一个很有用的对照组：如果它坏了，说明问题出在
  NUMAflow 自身，不是这次 Redis 侧集成引入的。
- 跑一次 `./run_full_validation.sh --quick` 做快速综合检查；如果改动涉及迁移策
  略、内存分配或淘汰逻辑，跑完整版 `./run_full_validation.sh`（它还会跑一遍 QEMU
  多 NUMA 节点路径）。细节见 [`TESTING.md`](TESTING.md)。

## 这些规则从何而来

下面每一条都不是拍脑袋定的——都能追溯到本分叉真实踩过的一个坑：

- **合并干净 ≠ 合并正确。** `git merge` 的 recursive 策略在两边都改动过同一段代码
  时，可能悄无声息地重复、丢弃或损坏代码，且完全不留下冲突标记。Redis
  6.2.21→7.2.6 的核心合并里就有 6 个这样的 bug，靠 grep 冲突标记根本发现不了，只
  有真正跑一次 `make -j$(nproc)` 加完整测试套件才抓得到。完整名单见
  [`docs/redis7-migration.md`](docs/redis7-migration.md)。**永远不要只凭"没有冲
  突标记"就认定一次非小合并是安全的。**
- **要 grep 完整的编译日志，不要只看开头一截。** 一次 `head -30` 漏掉了一处
  `dictType` 回调签名不匹配、和一处 `createIntConfig`/`createSizeTConfig` 字段宽
  度不匹配的问题——这两个问题都只在编译日志**更靠后**的部分以警告（而不是报错）
  的形式出现。永远用 `implicit declaration|incompatible pointer|too few
  arguments|too many arguments` 这类关键词去 grep**完整**的日志。
- **审查每一个二进制入口点的分配器初始化顺序，不能只看 `redis-server` 的
  `main()`。** `numa_init()` 只在那里被调用；但 `redis-cli`/`redis-benchmark`/
  `redis-check-rdb`/`redis-check-aof`/`redis-sentinel` 全都链接了 `zmalloc.o`，
  却从来没有调用过它。任何假设 `numa_ctx.numa_available` 已经被设置好的新 NUMA
  代码路径，都需要给这些从未初始化过它的二进制准备一条明确的退化路径。
- **合并基点的 tag 不等于一个打齐补丁的安全基线。** 针对合并拉进来的具体文件，要
  单独核查已知 CVE——这和"diff 出 NUMA 相关改动"是两件独立的事。选中 `7.2.6` 并
  不自动包含 CVE-2025-32023 这个在该 tag 之后才修复的 HyperLogLog 漏洞补丁。
