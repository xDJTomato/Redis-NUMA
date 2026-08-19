> **关于本文件**：这是本仓库存档的上游 Redis 官方 README
> （[`REDIS_ORIGINAL_README.md`](REDIS_ORIGINAL_README.md)）的中文翻译，仅供阅读
> 参考——目的是让不熟悉英文的读者也能方便地了解 Redis 本身（不是本 fork 新增的
> NUMA/CXL 部分）是怎么构建、运行、组织代码的。**同目录下的英文原文才是忠实保留
> 的存档记录**；如果两者在措辞上有出入，请以英文原文为准。本文档描述的是 Redis
> 这个项目本身的通用知识，与本 fork 新增的 NUMA 模块无关——那部分内容见
> [`docs/GUIDE.zh-CN.md`](../../GUIDE.zh-CN.md)。

本 README 只是一份快速的*入门*文档。更详细的文档请见
[redis.io](https://redis.io)。

Redis 是什么？
--------------

Redis 常被称为一个*数据结构*服务器。这句话的意思是，Redis 通过一组命令提供对可
变数据结构的访问，这些命令通过*客户端-服务器*模型、基于 TCP 套接字和一个简单的
协议来发送。这样，不同的进程就可以以共享的方式查询和修改同一份数据结构。

Redis 中实现的数据结构有几个特殊的属性：

* Redis 会把它们存储到磁盘上，即便它们始终是在服务器内存中被读取和修改的。这
  意味着 Redis 很快，但同时也是非易失的（重启后数据不会丢失）。
* 数据结构的实现强调内存效率，因此 Redis 内部的数据结构，相比用高级编程语言建
  模同样的数据结构，通常会占用更少的内存。
* Redis 提供了一系列数据库中常见的特性，比如复制、可调节的持久化级别、集群，
  以及高可用性。

另一个不错的类比是：把 Redis 看作一个更复杂的 memcached，只不过它的操作不仅是
SET 和 GET，还包括操作 List、Set、有序数据结构等复杂数据类型的命令。

如果你想了解更多，这里有几个不错的起点：

* Redis 数据类型介绍：https://redis.io/topics/data-types-intro
* 直接在浏览器里试用 Redis：http://try.redis.io
* Redis 命令的完整列表：https://redis.io/commands
* Redis 官方文档中还有更多内容：https://redis.io/documentation

构建 Redis
--------------

Redis 可以在 Linux、OSX、OpenBSD、NetBSD、FreeBSD 上编译和使用。我们支持大端序
和小端序架构，也支持 32 位和 64 位系统。

它也可能在 Solaris 衍生系统（比如 SmartOS）上编译通过，但我们对这个平台的支持
是*尽力而为*的，不保证 Redis 在其上能和 Linux、OSX、\*BSD 上表现得一样好。

构建非常简单：

    % make

如果要构建带 TLS 支持的版本，你需要 OpenSSL 的开发库（例如 Debian/Ubuntu 上的
`libssl-dev`），然后运行：

    % make BUILD_TLS=yes

如果要构建带 systemd 支持的版本，你需要 systemd 的开发库（比如 Debian/Ubuntu 上
的 `libsystemd-dev`，或 CentOS 上的 `systemd-devel`），然后运行：

    % make USE_SYSTEMD=yes

如果要给 Redis 程序名加一个后缀，使用：

    % make PROG_SUFFIX="-alt"

你可以用下面的命令构建一个 32 位的 Redis 二进制：

    % make 32bit

构建完 Redis 之后，最好用下面的命令测试一下：

    % make test

如果构建时启用了 TLS，要跑带 TLS 的测试（需要先安装 `tcl-tls`）：

    % ./utils/gen-test-certs.sh
    % ./runtest --tls


修复依赖或缓存构建选项引发的构建问题
---------

Redis 有一些依赖，它们都放在 `deps` 目录下。当依赖的源代码发生变化时，`make`
并不会自动重新构建这些依赖。

当你用 `git pull` 更新了源代码，或者 `deps` 目录树内的代码以任何其它方式被修改
过时，请务必用下面这条命令彻底清理并从头重新构建：

    make distclean

这会清理：jemalloc、lua、hiredis、linenoise。

另外，如果你强制指定了某些构建选项，比如 32 位目标、关闭 C 编译器优化（用于调
试）等类似的构建期选项，这些选项会被一直缓存下去，直到你执行一次 `make
distclean` 命令。

修复构建 32 位二进制时的问题
---------

如果你在用 32 位目标构建完 Redis 之后，需要改用 64 位目标重新构建（或者反过来），
你需要在 Redis 发行包的根目录下执行一次 `make distclean`。

如果在构建 32 位版本的 Redis 时遇到构建错误，可以尝试下面的步骤：

* 安装 `libc6-dev-i386` 这个包（也可以试试 `g++-multilib`）。
* 试试用下面这条命令代替 `make 32bit`：
  `make CFLAGS="-m32 -march=native" LDFLAGS="-m32"`

分配器（Allocator）
---------

要在构建 Redis 时选择一个非默认的内存分配器，可以通过设置 `MALLOC` 环境变量来
实现。Redis 默认会针对 libc 的 malloc 编译和链接，唯一的例外是：在 Linux 系统
上默认使用 jemalloc。选择 jemalloc 作为默认值，是因为实践证明它比 libc malloc
的内存碎片问题更少。

要强制针对 libc malloc 编译，使用：

    % make MALLOC=libc

要在 Mac OS X 系统上针对 jemalloc 编译，使用：

    % make MALLOC=jemalloc

单调时钟（Monotonic clock）
---------------

默认情况下，Redis 会使用 POSIX 的 `clock_gettime` 函数作为单调时钟源。在大多数
现代系统上，可以用处理器内部时钟来提升性能。这方面需要注意的事项可以参考这里：
    http://oliveryang.net/2015/09/pitfalls-of-TSC-usage/

要构建支持使用处理器内部指令时钟的版本，使用：

    % make CFLAGS="-DUSE_PROCESSOR_CLOCK"

详细构建输出
-------------

Redis 默认会以一种对用户友好的彩色输出方式构建。如果你想看到更详细的输出，可以
使用：

    % make V=1

运行 Redis
-------------

要用默认配置运行 Redis，只需输入：

    % cd src
    % ./redis-server

如果你想提供自己的 redis.conf，需要额外传一个参数（配置文件的路径）来运行它：

    % cd src
    % ./redis-server /path/to/redis.conf

你也可以直接在命令行里通过参数选项来修改 Redis 的配置。例如：

    % ./redis-server --port 9999 --replicaof 127.0.0.1 6379
    % ./redis-server /etc/redis/6379.conf --loglevel debug

redis.conf 里的所有选项，都同样支持以命令行选项的形式使用，且名字完全一致。

用 TLS 运行 Redis：
------------------

关于如何让 Redis 配合 TLS 使用，请参阅 [TLS.md](TLS.md) 文件（本目录内也提供了
中文版 [TLS.zh-CN.md](TLS.zh-CN.md)）获取更多信息。

体验 Redis
------------------

你可以用 redis-cli 来体验 Redis。先启动一个 redis-server 实例，然后在另一个终端
里试试下面的操作：

    % cd src
    % ./redis-cli
    redis> ping
    PONG
    redis> set foo bar
    OK
    redis> get foo
    "bar"
    redis> incr mycounter
    (integer) 1
    redis> incr mycounter
    (integer) 2
    redis>

你可以在 https://redis.io/commands 找到全部可用命令的列表。

安装 Redis
-----------------

要把 Redis 的二进制文件安装到 `/usr/local/bin`，只需使用：

    % make install

如果你想安装到别的目标目录，可以使用 `make PREFIX=/some/other/directory
install`。

`make install` 只会把二进制文件安装到你的系统里，不会配置初始化脚本和相应位置
的配置文件。如果你只是想稍微体验一下 Redis，这样就够了；但如果你是要在生产系统
上以正确的方式安装它，我们提供了一个脚本，可以在 Ubuntu 和 Debian 系统上完成这
件事：

    % cd utils
    % ./install_server.sh

*注意*：`install_server.sh` 在 Mac OSX 上不能用；它是专门为 Linux 构建的。

这个脚本会问你几个问题，然后帮你配置好运行 Redis 所需的一切，让它作为一个后台
守护进程运行，并在系统重启后自动再次启动。

你可以用名为 `/etc/init.d/redis_<端口号>` 的脚本来停止和启动 Redis，例如
`/etc/init.d/redis_6379`。

代码贡献
-----------------

注意：以任何形式向 Redis 项目贡献代码——包括通过 Github 发送 pull request、通
过私人邮件或公开讨论组发送代码片段或补丁——都意味着你同意在 Redis 源码发行包中
[COPYING][1] 文件所规定的 BSD 许可证条款下发布你的代码。

请查阅本源码发行包中的 [CONTRIBUTING][2] 文件以获取更多信息，包括我们处理安全
漏洞的流程细节。

[1]: https://github.com/redis/redis/blob/unstable/COPYING
[2]: https://github.com/redis/redis/blob/unstable/CONTRIBUTING

Redis 内部实现
===

如果你正在阅读这份 README，你很可能正对着一个 Github 页面，或者刚解压了 Redis
的发行 tar 包。无论哪种情况，你距离源代码都只差一步之遥，所以接下来我们会介绍
Redis 源码的目录布局，每个文件大致包含什么内容，Redis 服务器内部最重要的一些
函数和数据结构，等等。我们会把讨论保持在一个较高的层次，不深入具体细节——否则
这份文档会变得非常庞大，而且我们的代码库也在持续变化——但一个大致的概念应该是
一个很好的起点，帮助你进一步理解。此外，大部分代码都有详尽的注释，容易跟读。

源码目录布局
---

Redis 的根目录只包含这份 README、调用 `src` 目录内真正 Makefile 的顶层
Makefile，以及 Redis 和 Sentinel 的示例配置文件。你还能找到几个 shell 脚本，用
于执行 Redis、Redis Cluster 和 Redis Sentinel 的单元测试，这些测试实现在
`tests` 目录里。

根目录下有以下几个重要的目录：

* `src`：包含用 C 编写的 Redis 实现。
* `tests`：包含用 Tcl 实现的单元测试。
* `deps`：包含 Redis 使用的各种库。编译 Redis 所需的一切都在这个目录里；你的
  系统只需要提供 `libc`、一个兼容 POSIX 的接口和一个 C 编译器。值得一提的是，
  `deps` 里包含了一份 `jemalloc` 的拷贝，这是 Redis 在 Linux 下的默认分配器。
  另外要注意，`deps` 目录下也有一些最初是从 Redis 项目里发展出来的东西，但它们
  的主仓库并不是 `redis/redis`。

还有一些别的目录，但对我们这里的目标来说不那么重要。我们会主要关注 `src`，也
就是 Redis 实现所在的地方，逐一了解每个文件里都有什么。文件介绍的顺序是按照
逐步揭示不同复杂度层次的逻辑顺序来安排的。

注意：Redis 最近经历了相当多的重构。函数名和文件名都发生了变化，所以你可能会
发现这份文档更贴近 `unstable` 分支的情况。比如，在 Redis 3.0 中，`server.c` 和
`server.h` 文件曾被叫做 `redis.c` 和 `redis.h`。不过整体结构是一样的。请记住，
所有新的开发和 pull request 都应该针对 `unstable` 分支进行。

server.h
---

理解一个程序如何工作，最简单的方式是先理解它使用的数据结构。所以我们从 Redis
的主头文件 `server.h` 开始。

所有的服务器配置，以及一般意义上的全部共享状态，都定义在一个名为 `server` 的
全局结构体里，类型是 `struct redisServer`。这个结构体里几个重要的字段包括：

* `server.db` 是一个 Redis 数据库数组，数据就存储在这里。
* `server.commands` 是命令表。
* `server.clients` 是连接到服务器的客户端组成的链表。
* `server.master` 是一个特殊的客户端——如果本实例是一个副本（replica），这个
  字段就是它的主节点。

这个结构体里还有大量其它字段。大部分字段都直接在结构体定义处有注释说明。

另一个重要的 Redis 数据结构，是用来定义一个客户端的结构体。过去它叫
`redisClient`，现在就叫 `client`。这个结构体有很多字段，这里我们只展示主要
的几个：
```c
struct client {
    int fd;
    sds querybuf;
    int argc;
    robj **argv;
    redisDb *db;
    int flags;
    list *reply;
    char buf[PROTO_REPLY_CHUNK_BYTES];
    // ……还有很多其它字段……
}
```
这个 client 结构体定义了一个*已连接的客户端*：

* `fd` 字段是这个客户端的套接字文件描述符。
* `argc` 和 `argv` 会被填充上客户端正在执行的命令，这样实现某个具体 Redis
  命令的函数就可以读取到这些参数。
* `querybuf` 累积客户端发来的请求，这些请求会由 Redis 服务器按照 Redis 协议
  解析，然后通过调用客户端正在执行的命令的实现来执行。
* `reply` 和 `buf` 是动态和静态的缓冲区，用来累积服务器要发给客户端的回复。
  这些缓冲区会在文件描述符可写时，被增量地写入到套接字里。

正如你在上面的 client 结构体里看到的，命令中的参数是用 `robj` 结构体来描述的。
下面是完整的 `robj` 结构体，它定义了一个*Redis 对象*：

    typedef struct redisObject {
        unsigned type:4;
        unsigned encoding:4;
        unsigned lru:LRU_BITS; /* lru 时间（相对于 server.lruclock） */
        int refcount;
        void *ptr;
    } robj;

基本上，这个结构体可以表示 Redis 所有基础的数据类型，比如字符串、列表、集合、
有序集合等等。有意思的地方在于：它有一个 `type` 字段，因此可以知道某个给定对象
的类型；还有一个 `refcount` 字段，因此同一个对象可以在多个地方被引用，而不需要
被多次分配。最后，`ptr` 字段指向这个对象实际的表示——即便是同一种类型，这个表
示也可能因为所用的 `encoding` 不同而不同。

Redis 对象在 Redis 内部被大量使用，不过为了避免间接访问带来的开销，最近在很多
地方我们直接使用未被包在 Redis 对象里的普通动态字符串。

server.c
---

这是 Redis 服务器的入口点，`main()` 函数就定义在这里。以下是启动 Redis 服务器
过程中最重要的几个步骤。

* `initServerConfig()` 设置 `server` 结构体的默认值。
* `initServer()` 分配运行所需的数据结构，设置监听套接字，等等。
* `aeMain()` 启动事件循环，开始监听新连接。

事件循环会周期性地调用两个特殊的函数：

1. `serverCron()` 会按照 `server.hz` 指定的频率被周期性调用，执行那些需要时不
   时被执行一次的任务，比如检查超时的客户端。
2. `beforeSleep()` 会在每次事件循环触发、Redis 处理完少量请求、即将返回事件
   循环之前被调用。

在 server.c 内部，你还能找到处理 Redis 服务器其它重要事情的代码：

* `call()` 用于在给定客户端的上下文中调用某个给定的命令。
* `activeExpireCycle()` 处理通过 `EXPIRE` 命令设置了存活时间的 key 的淘汰。
* `performEvictions()` 会在需要执行一个新的写命令、但根据 `maxmemory` 指令
  Redis 已经内存不足时被调用。
* 全局变量 `redisCommandTable` 定义了全部 Redis 命令，指定了每个命令的名字、
  实现该命令的函数、所需的参数数量，以及其它属性。

networking.c
---

这个文件定义了与客户端、主节点和副本（在 Redis 里它们只是特殊的客户端）之间
全部的 I/O 函数：

* `createClient()` 分配并初始化一个新的客户端。
* `addReply*()` 这一族函数，被命令的实现用来把数据追加到客户端结构体里，这些
  数据会作为某个已执行命令的回复被传输给客户端。
* `writeToClient()` 把输出缓冲区里待发送的数据传输给客户端，它是由*可写事件
  处理函数* `sendReplyToClient()` 调用的。
* `readQueryFromClient()` 是*可读事件处理函数*，把从客户端读到的数据累积进
  查询缓冲区。
* `processInputBuffer()` 是按照 Redis 协议解析客户端查询缓冲区的入口点。一旦
  命令准备好被处理，它就会调用定义在 `server.c` 里的 `processCommand()` 来
  真正执行这个命令。
* `freeClient()` 释放、断开并移除一个客户端。

aof.c 与 rdb.c
---

从名字就能猜到，这两个文件实现了 Redis 的 RDB 和 AOF 持久化。Redis 的持久化
模型基于 `fork()` 系统调用，创建一个和主 Redis 线程拥有相同（共享）内存内容的
线程。这个次级线程把内存内容 dump 到磁盘上。`rdb.c` 用它来在磁盘上创建快照，
`aof.c` 用它来在 append only file 变得太大时执行 AOF 重写。

`aof.c` 内部的实现还有一些额外的函数，用来实现一个 API，让各个命令在客户端
执行它们时，把新的命令追加写入 AOF 文件。

定义在 `server.c` 里的 `call()` 函数，负责调用那些最终会把命令写入 AOF 的函数。

db.c
---

某些 Redis 命令操作特定的数据类型；另一些则是通用的。通用命令的例子有 `DEL`
和 `EXPIRE`——它们操作的是 key，而不是具体针对某种值。所有这些通用命令都定义
在 `db.c` 里。

此外，`db.c` 还实现了一套 API，可以在不直接访问内部数据结构的情况下，对 Redis
数据集执行某些操作。

`db.c` 内部最重要的、在很多命令实现中被用到的函数包括：

* `lookupKeyRead()` 和 `lookupKeyWrite()` 用来获取某个给定 key 关联的值的
  指针；如果该 key 不存在，则返回 `NULL`。
* `dbAdd()` 以及它更高层的对应函数 `setKey()`，在 Redis 数据库里创建一个新的
  key。
* `dbDelete()` 删除一个 key 及其关联的值。
* `emptyDb()` 清空单个数据库，或者清空全部已定义的数据库。

这个文件剩下的部分实现了暴露给客户端的通用命令。

object.c
---

前面已经描述过定义 Redis 对象的 `robj` 结构体。在 `object.c` 内部，有一整套在
基础层面操作 Redis 对象的函数，比如分配新对象、处理引用计数等等。这个文件里值
得一提的函数包括：

* `incrRefCount()` 和 `decrRefCount()` 用来增加或减少一个对象的引用计数。当
  计数降到 0 时，对象最终会被释放。
* `createObject()` 分配一个新对象。此外还有一些专门的函数用来分配具有特定内容
  的字符串对象，比如 `createStringObjectFromLongLong()` 以及类似的函数。

这个文件还实现了 `OBJECT` 命令。

replication.c
---

这是 Redis 内部最复杂的文件之一，建议在对代码库其余部分有一定熟悉之后，才去
接触它。这个文件里实现了 Redis 的主节点角色和副本角色。

这个文件里最重要的函数之一是 `replicationFeedSlaves()`，它把命令写给代表连接
到我们主节点上的副本实例的客户端，这样副本就能获取到客户端执行的写操作：这样
它们的数据集就会和主节点保持同步。

这个文件还实现了 `SYNC` 和 `PSYNC` 命令，它们用于在主节点和副本之间执行首次
同步，或者在断连之后继续复制。

其它 C 文件
---

* `t_hash.c`、`t_list.c`、`t_set.c`、`t_string.c`、`t_zset.c` 和 `t_stream.c`
  包含了各个 Redis 数据类型的实现。它们同时实现了访问某个给定数据类型的 API，
  以及这些数据类型对应命令的客户端实现。
* `ae.c` 实现了 Redis 的事件循环，它是一个自成一体的库，读起来和理解起来都很
  简单。
* `sds.c` 是 Redis 的字符串库，更多信息可以参考
  http://github.com/antirez/sds 。
* `anet.c` 是一个库，相比内核暴露的原始接口，它让使用 POSIX 网络功能变得更
  简单。
* `dict.c` 实现了一个非阻塞的哈希表，支持增量 rehash。
* `scripting.c` 实现了 Lua 脚本功能。它完全自成一体、与 Redis 实现的其它部分
  隔离，如果你熟悉 Lua API，理解起来会很简单。
* `cluster.c` 实现了 Redis Cluster。建议在对代码库其余部分非常熟悉之后再阅读
  它。如果你打算阅读 `cluster.c`，请务必先阅读
  [Redis Cluster 规范][3]。

[3]: https://redis.io/topics/cluster-spec

一个 Redis 命令的解剖
---

所有 Redis 命令都是以如下方式定义的：

    void foobarCommand(client *c) {
        printf("%s",c->argv[1]->ptr); /* 对参数做一些处理。 */
        addReply(c,shared.ok); /* 给客户端回复一些内容。 */
    }

然后，这个命令会在 `server.c` 的命令表里被引用：

    {"foobar",foobarCommand,2,"rtF",0,NULL,0,0,0,0,0},

在上面的例子里，`2` 是这个命令接受的参数数量，而 `"rtF"` 是命令的标志位，具体
含义记录在 `server.c` 里命令表顶部的注释中。

命令执行完某些操作之后，会给客户端返回一个回复，通常是通过 `addReply()` 或者
定义在 `networking.c` 里的类似函数来完成的。

Redis 源码里有大量的命令实现，可以作为实际命令实现的例子。写几个玩具命令来练手，
是熟悉这个代码库的一个不错的练习。

还有很多别的文件这里没有介绍，但把所有内容都讲一遍并没有意义。我们只是想帮你
迈出第一步。相信你最终会在 Redis 代码库里找到自己的方向 :-)

尽情享受！
