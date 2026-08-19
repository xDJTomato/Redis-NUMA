> 本文是 `TLS.md` 的中文翻译，仅供阅读方便——`TLS.md` 是从上游 Redis 原样归档保
> 留的英文原文，是这份记录的权威版本；本翻译不作为归档本身，如有出入以英文原文
> 为准。

# TLS 支持

## 快速上手

### 编译

要编译出带 TLS 支持的版本，需要先安装 OpenSSL 开发库（Debian/Ubuntu 上是
`libssl-dev`）。

把 TLS 支持编译进 Redis 本体：
运行 `make BUILD_TLS=yes`。

或者把 TLS 编译成 Redis 模块：
运行 `make BUILD_TLS=module`。

注意 Sentinel 模式不支持 TLS 模块。

### 测试

要跑带 TLS 的 Redis 测试套件，需要给 TCL 装上 TLS 支持（Debian/Ubuntu 上是
`tcl-tls` 包）。

1. 运行 `./utils/gen-test-certs.sh` 生成一个根 CA 和一张服务器证书。

2. 运行 `./runtest --tls` 或 `./runtest-cluster --tls`，以 TLS 模式跑 Redis
   和 Redis Cluster 的测试。

3. 运行 `./runtest --tls-module` 或 `./runtest-cluster --tls-module`，以
   "TLS 作为模块加载"的模式跑 Redis 和 Redis Cluster 的测试。

### 手动运行

手动以 TLS 模式启动一个 Redis 实例（假设已经运行过 `gen-test-certs.sh`，示例
证书/密钥已经就位）：

TLS 内置模式：

    ./src/redis-server --tls-port 6379 --port 0 \
        --tls-cert-file ./tests/tls/redis.crt \
        --tls-key-file ./tests/tls/redis.key \
        --tls-ca-cert-file ./tests/tls/ca.crt

TLS 模块模式：

    ./src/redis-server --tls-port 6379 --port 0 \
        --tls-cert-file ./tests/tls/redis.crt \
        --tls-key-file ./tests/tls/redis.key \
        --tls-ca-cert-file ./tests/tls/ca.crt \
        --loadmodule src/redis-tls.so

用 `redis-cli` 连接这个 Redis 实例：

    ./src/redis-cli --tls \
        --cert ./tests/tls/redis.crt \
        --key ./tests/tls/redis.key \
        --cacert ./tests/tls/ca.crt

这样配置会关闭 TCP、只在 6379 端口启用 TLS。也可以让 TCP 和 TLS 同时可用，但
需要给它们分配不同的端口。

要让副本（Replica）用 TLS 连接主节点，加上 `--tls-replication yes`；要让
Redis Cluster 节点间通信也走 TLS，加上 `--tls-cluster yes`。

## 连接层

现在所有的 socket 操作都统一走一层"连接抽象层"，把 I/O 和读写事件处理对调用方
屏蔽掉。

**TLS 目前不支持多线程 I/O**，因为一个 TLS 连接需要自己去操作 AE 事件，而这并
不是线程安全的。可能的解法是给 I/O 线程各自维护独立的 AE 事件循环，并让连接和
线程建立更长期的绑定关系——这样做也有可能顺带提升整体性能。

TLS 目前的同步 I/O 是用一种比较"糙"的方式实现的：把 socket 设成阻塞模式，再配
上 socket 级别的超时。这意味着超时的精度不一定准，而且会有不少系统调用开销。不
过我更倾向于认为，与其费力去修这个问题，不如干脆彻底去掉同步 I/O、转向纯异步实
现——对于复制（replication）这一块应该不难做到；对集群的 key 迁移可能会更麻
烦一些，但这部分本来也有别的理由值得改进。

## 待办事项

- [ ] `redis-benchmark` 的支持。目前的实现是"用 hiredis 做解析和基本联网（建立
  连接），但大部分实际操作是直接操作 socket"的混合方式，要做好 TLS 支持需要先
  把这块理清楚。比较好的做法大概是迁移到 hiredis 的异步模式。
- [ ] `redis-cli` 的 `--slave` 和 `--rdb` 支持。

## 多端口

需要考虑允许把 TLS 配置在单独端口上、让 Redis 同时监听多个端口时会带来哪些影
响：

1. 启动横幅（banner）里的端口提示
2. 进程标题（proctitle）
3. 从节点如何宣告自己的地址
4. 集群总线端口的计算方式
