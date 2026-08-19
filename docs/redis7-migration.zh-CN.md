# Redis 6.2.21 → 7.2.6 迁移记录

本文档记录 NUMA/CXL 分叉的 Redis 内核从 6.2.21 迁移到 7.2.6（分支
`feat/redis7-port`）时**实际发生**的过程。它取代了早先的
`docs/redis8-migration.md`——那是一份在真正尝试合并**之前**写的纸面设计，其中好几
个假设在真正合并之后被证明是错的，配套的 `src/redis8_compat.h` 兼容层也从未被接入
构建（没有任何文件 `#include` 它），已被删除。以下是这次真实合并的完整记录：用了
什么工具、哪里出了问题、每个问题是怎么被发现和修复的。

## 为什么选 7.2.6，而不是 8

7.2.6 是 Redis 许可证变更（7.4 版本起改为 RSALv2/SSPL 双许可）之前、也是更大规模
的 8.x 重构之前的最后一个稳定版本，同时已经包含了会真正影响本分叉 NUMA 模块的全
部 API 变化（不透明的 `dictEntry`、listpack、quicklist 容器类型）。选择它既能让
diff 保持在可审阅的规模，又能继续留在本分叉一直采用的 BSD-3-Clause 许可证下。

## 为什么需要手动移植（graft）

本仓库当初是作为一份全新的历史导入的（`git log` 只显示一个根提交 "first
commit"）——它**不**与 `redis/redis` 共享提交历史。直接对着它执行
`git merge upstream/7.2.6` 会被当成两棵完全不相关的树来对比，产生的是漫天的整文件
冲突，而不是一次真正的三方合并。

解决办法：先拉取上游的 `6.2.21` 和 `7.2.6` 两个 tag，然后执行
`git replace --graft <本仓库根提交> <上游 6.2.21 提交>`，给本仓库的根提交移植一个
假的父提交，让它等于上游自己的 6.2.21 树。这样 Git 就能计算出正确的合并基点（上游
6.2.21），在"我们的 6.2.21 + NUMA 模块"和"上游 7.2.6"之间做一次真正的三方合并，
而不是一次从零开始的对拍。

## 核心教训：合并干净 ≠ 合并正确

`git merge` 的 recursive 策略，只要能算出**一个**结果、不需要留下 `<<<<<<<` 标记，
就会自动解决冲突——但"没有标记"只代表"没有*无法自动判断*的冲突"，不代表"判断的
结果是对的"。这次合并里有 6 个真实 bug 在零冲突标记的情况下混进了代码，对
`grep` diff3 标记这种检查方式完全不可见：

| # | 文件 | 混入了什么 | 症状 |
|---|------|----------------|---------|
| 1 | `src/zmalloc.c` | 一个包住 `PREFIX_SIZE` 的 `#ifdef HAVE_NUMA` 块，闭合的 `#endif` 丢了 | 编译错误，后面所有符号全炸 |
| 2 | `src/dict.c` | 一段多余的重复溢出检查，引用了一个在 `dictht`→`ht_table[]` 结构调整后已经不存在的变量 `realsize` | 编译错误：未声明的标识符 |
| 3 | `src/server.c` | `afterCommand()` 的 6.2.21 版本和 7.2.6 版本两份函数体前后紧挨着都被保留了 | 编译错误：重复定义 |
| 4 | `src/networking.c` | `addReplyBigNum()` 和 `deferredAfterErrorReply()` 同样两份函数体都被保留 | 编译错误：重复定义 |
| 5 | `src/server.h` | `objectComputeSize()` 的旧 2 参数原型，在 `.c` 里的定义已经改成 4 参数之后，仍然留在头文件里没更新 | 隐式声明（编译不报错），导致 `evict.c`/`evict_numa.c` 在 `integration/replication-buffer.tcl` 测试中 SIGSEGV |
| 6 | `src/server.c` | `call()` 函数里，6.2.21 那个无条件调用 `replicationFeedMonitors()` 的语句，和 7.2.6 那个受 flag 保护的调用，两句都被保留了 | 不崩溃，但 MONITOR 客户端会看到每条命令被喂两次（`unit/introspection.tcl` 测试失败） |

这 6 个问题没有一个留下冲突标记，全部只能靠**真正编译整棵树 + 跑真实测试套件**才
能发现——只 grep 冲突标记，或者"读一遍 diff 觉得看起来合理"，都不足以发现它们。
**今后的规则：本仓库任何一次非小规模的跨版本合并之后，都必须跑一次完整的
`make -j$(nproc)` 编译到底，把每一个编译错误都当作"可能是合并静默损坏了代码"去
排查，而不是想当然地当成"这是正常的 API 迁移工作量"。** 区分"这段代码需要迁移到
新 API"和"合并把原本能工作的代码搞坏了"这两种情况的方法，是把这段代码分别和"合
并前的本项目提交"以及"真实的上游 tag 提交"各 diff 一遍，而不是只凭感觉猜。

## 把 NUMA 模块迁移到 Redis 7 的 API 上

这部分是预期之中的真实工作量，不是合并事故：

- **`dictCreate(dictType*)` 去掉了 `privdata` 参数**——`numa_key_migrate.c` 和
  `numa_composite_lru.c` 里每一处 `dictCreate(&sometype, NULL)` 都改成
  `dictCreate(&sometype)`。
- **`dictEntry` 变成完全不透明的类型。** 原来手动遍历 `d->ht[t].table[i]` 的代码
  （出现在 `numa_object_sample_alloc_ptr`/`numa_object_sample_alloc_size`
  里）被改写成使用
  `dictGetIterator`/`dictNext`/`dictGetKey`/`dictGetVal`/`dictReleaseIterator`，
  统计条目大小改用 `dictEntryMemUsage()` 和 `dictSlots(d)`，而不是
  `sizeof(dictEntry)` 加手算槎位数。
- **`dictType` 的回调函数**（`keyCompare`/`keyDup`/`keyDestructor`/
  `valDestructor`）第一个参数现在是 `dict *d`，而不是 `void *privdata`——两个模块
  的回调表都相应更新。
- **`dictGenHashFunction`** 现在接受 `size_t len`，而不是 `int len`——顺手删掉了
  `numa_tinylfu.c` 里一处多余、且已经用旧签名"遮蔽"了 `dict.h` 正确原型的
  `extern` 重复声明。
- **`quicklistNode->zl` 改名为 `->entry`**——`numa_key_migrate.c` 里 LIST 类型的迁
  移适配器整段随之改名。
- **hash/zset 编码**：`OBJ_ENCODING_ZIPLIST` 为了 RDB 向后兼容仍然保留，但新对象
  使用 `OBJ_ENCODING_LISTPACK`。在 `migrate_hash_type()` 和 `migrate_zset_type()`
  里，给已有的 ziplist 分支旁边加上了 listpack 分支（用 `lpBytes()`）。迁移本身两
  种编码都只是对整块打包数据做一次 `memcpy`，所以这里的风险比最初预想的更低——不
  需要逐条遍历。
- **`src/commands/numa.json`**：用 Redis 7 的声明式命令自省系统（`COMMAND INFO`、
  `COMMAND DOCS`）注册了 `NUMA` 命令，并重新生成了 `src/commands.def`。

## 一个被合并"暴露"、但并非由合并造成的历史 bug

`numa_init()`——负责搭建 `zmalloc.c` 里 slab/direct-cache 分配器状态——只在
`server.c` 的 `main()` 里被调用过。但每一个链接了 `zmalloc.o` 的其它二进制
（`redis-cli`、`redis-benchmark`、`redis-check-rdb`/`aof`、`redis-sentinel`）从来
没有调用过它，所以这些进程整个生命周期里 `numa_ctx.numa_available` 都是 0。
`zmalloc()`/`zcalloc()`/`zrealloc()` 已经对这个标志位做了判断，退化成普通
`malloc()`——但 `dict.c` 的 `dictCreate()`/`dictGetIterator()` 用到的
`zmalloc_local()`/`zcalloc_local()`/`ztrycalloc_local()` 会**无条件**调用
`numa_alloc_dram()`，在一段从未初始化过的全局状态上运行 slab 分配器逻辑。这会以一
种"当下不炸、之后在一次完全不相关的 `free()` 里才炸"的方式损坏堆。

这个 bug 在合并之前的 6.2.21 版本 `dict.c`/`zmalloc.c` 里其实**一直存在**——只是
7.x 之前的测试套件从没恰好触发过 `redis-cli` 的 dict/迭代器代码路径。Redis 7 新增
的 `tests/unit/cluster/cli.tcl`（跑 `redis-cli --cluster create`，内部会建一个反
亲和性打分用的 dict）第一次踩中了它，表现为 `redis-cli` 在 `zfree()` 内部
SIGSEGV。修复方式是给 `numa_alloc_dram()` 补上和 `zmalloc()` 一样的
`numa_ctx.numa_available` 判断（配一份同样的 plain-`malloc` 退化路径）。

**教训**：一个"NUMA 感知"的分叉项目，需要审查每一个二进制入口点的分配器初始化时
序，不能只看 `redis-server` 的 `main()`。这一类 bug 在其它进程恰好触发相关代码路
径之前完全隐形。

## 一个必须单独 cherry-pick 的安全修复

选定 `7.2.6` 作为合并基点，并不等于选到了一个完全打齐安全补丁的基线。
`tests/unit/hyperloglog.tcl` 里 "Corrupted sparse HyperLogLogs ... XZERO
opcode" 这条测试，在完全没有改过、和上游逐字节相同的 `src/hyperloglog.c` 里让
`hllMerge()` 崩溃——这正是
[CVE-2025-32023](https://github.com/redis/redis/security/advisories)（一个
HyperLogLog 越界写漏洞），上游修复提交是 `f35b72dd1`，时间上*晚于* `7.2.6` 这个
tag。这个提交被干净地 cherry-pick 到了 `hyperloglog.c` 上；唯一的冲突出在对应的
`.tcl` 测试文件本身——7.2.6 自带的测试套件里已经有一条措辞不同、但逻辑上等价的回
归测试，解决方式是保留现有措辞。

**教训**：任何合并基点选定之后，都要针对合并拉进来的具体文件单独核查已知 CVE——
这和"diff 出 NUMA 相关改动"是两件独立的事。

## 两个靠编译器警告（不是报错）抓到的配置注册 bug

- `src/config.c`：`numa-demote-min-size` 用 `createIntConfig()` 注册，但它在
  `server.h` 里对应的字段实际类型是 `size_t`——当 `CONFIG SET` 设置接近或超过
  `INT_MAX` 的值时会静默截断。修复方式是换成 `createSizeTConfig()`。
- 上面这个问题，加上前面提到的 `dictType` 回调签名不匹配问题，都是靠重新 grep
  *完整*的编译日志才被发现的——用的关键词是
  `implicit declaration|incompatible pointer|too few arguments|too many
  arguments`，此前一次被截断的 `head -30` grep漏掉了它们。**一定要 grep 完整的编
  译日志，不要只看开头几十行**——一次编译在报出第一个 error 之前，完全可能已经打
  印了远超过 30 行值得关注的 warning。

## 最终验证

- `cd src && make clean && make -j$(nproc)`：零错误，只剩下和这次迁移无关的既有
  警告。
- `make test`（完整 Tcl 套件）：最终一次运行 91/91 个文件全部通过、零错误（在此之
  前的四次运行分别抓出并修复了上面 6 个 bug 里的一个）。
- `numaflow` 自己的测试套件（`cd numaflow && make test`）：不受这次核心合并影响，
  因为 NUMAflow 跟 Redis/libnuma 完全无关；全程保持绿色，可以当一个对照组。
- 手工功能烟雾测试：`NUMA` 命令注册及其三个子命令（`CONFIG`、`STRATEGY`、
  `MIGRATE`）通过 `redis-cli` 逐一验证，跨全部 5 种数据类型的驱逐压力测试，以及一
  次真实的 3 节点 `redis-cli --cluster create`。
- `tests/vm/boot_numa_vm.sh`：在一台 2-NUMA-节点的 QEMU 客户机（本机没有
  `/dev/kvm`，纯软件 TCG 模拟）里跑通 `redis-server` + `NUMA` 命令族 +
  `redis-benchmark`。详见 [TESTING.md](../TESTING.md)。

## 被删除的内容

- `src/redis8_compat.h`——死代码。整个仓库没有任何文件 `#include` 它；真正的迁移
  路径直接走的是 Redis 7 的真实 API（如上所述），而不是通过一个兼容层。留着它会
  歪曲这次迁移实际发生的方式。
- `docs/redis8-migration.md`——被本文档取代。
</content>
