# 测试指南

## 快速参考

```bash
./run_full_validation.sh --quick     # 编译 + make test + NUMAflow 基准测试
./run_full_validation.sh             # 以上内容 + YCSB（若已安装 java）、
                                      #   QEMU 虚拟机冒烟测试、CXLMemSim 校验
```

每次运行都会生成 `results/full_report_<timestamp>/index.html`，以及每一步的原始
日志/JSON 文件。任何在当前环境跑不了的步骤都会被记录为 `skipped` 并写明原因——
**绝不伪造结果**。

可用开关：`--skip-build`、`--skip-test`、`--skip-ycsb`、`--skip-vm`、
`--skip-cxlmemsim`、`--vm-timeout SECONDS`。`--quick` 等价于
`--skip-ycsb --skip-vm --skip-cxlmemsim`。

## 测试层级

### 1. 标准 Redis Tcl 测试套件

```bash
cd src && make clean && make -j$(nproc)
cd .. && make test
```

运行完整的 `tests/unit/*.tcl` 套件。[`docs/redis7-migration.md`](docs/redis7-migration.md)
里提到的全部 6 个静默合并 bug 和那 1 个既有的分配器保护 bug，都是在这一层被真正
抓到的——这一层出现任何回归都应当被当作严重信号，而不是噪音。

### 2. NUMA 环境脚本

```bash
./utils/numa/check_numa_config.sh
./utils/numa/diagnose_numa.sh
```

对照主机真实 NUMA 拓扑做的快速健全性检查（即使主机只有 1 个物理节点，检查也能
优雅降级、正常运行）。

### 3. NUMAflow 子系统（纯 C11，不依赖 Redis）

```bash
cd numaflow && make && make test && make report
```

`make test` 会跑单元测试、冒烟测试、桥接/自适应测试和分配器测试。`make report`
会在 `zipf`/`uniform`/`hotspot`/`temporal` 四种工作负载上运行公平评测框架，重新
生成 `results/bench_*.json` 和 `results/report.html`（比较
noop/composite_lru/tinylfu/CAAT 净代价和本地命中率的条形图，纯 stdlib SVG 实现，
不依赖 matplotlib）。

`numaflow eval` 还支持 `--cxl-latency-ns <n>` 和 `--cxl-bandwidth-mbps <n>`，用一
次真实 CXLMemSim 设备级链路运行捕获到的数值，替换非 DRAM 层代价模型的参数——而
不是使用 `numa_shim.c` 里合成的 tier-1 默认值（300ns / 8000 MB/s）。
`run_full_validation.sh` 会用两种方式各跑一遍全部工作负载，把校准后的结果写入
`results/bench_<workload>_cxlcal.json`。

### 4. YCSB 带宽基准测试

```bash
cd tests/ycsb && ./run_bw_benchmark.sh    # 三阶段：Fill -> Hotspot -> Sustain
cd tests/ycsb && ./run_ycsb.sh            # baseline/stress 两种模式
```

需要 JDK 和 YCSB 发行包（如果 `PATH` 里没有 `java`，`run_full_validation.sh` 会
自动跳过这一步——它不会替你安装 JDK）。

如果想和未经修改的原版 Redis 做同版本的 A/B 对比（无 NUMA 模块，用 jemalloc 而
不是本项目的 NUMA 分配器），可以从本仓库自带的上游 tag 生成一份原版 7.2.6 检出，
再用 `run_bw_benchmark_vanilla.sh` 跑它：

```bash
git worktree add ../redis-7.2.6-vanilla 7.2.6
cd ../redis-7.2.6-vanilla/src && make -j$(nproc)
cd - && ./run_bw_benchmark_vanilla.sh   # 默认绑定 6380 端口
```

### 5. QEMU 多 NUMA 节点冒烟测试

```bash
./tests/vm/boot_numa_vm.sh [--timeout SECONDS] [--keep] [--skip]
```

启动一个 Debian 12 通用云镜像（只下载一次，缓存在 `tests/vm/.cache/`，已加入
gitignore），通过 `-object memory-backend-ram` + `-numa node,memdev=...` 模拟出
2 个 NUMA 节点，带一个诚实的 SSH 等待超时（默认 480 秒——本机没有 `/dev/kvm`，
启动走的是纯软件 TCG 模拟，本来就慢），把本地编译好的
`redis-server`/`redis-cli`/`redis-benchmark` 拷进去，在客户机内跑
`PING`/`SET`/`GET`、`NUMA CONFIG GET`、`NUMA FLOW LIST` 以及一次
`redis-benchmark`。NUMA 节点的可见性是直接读取 `/sys/devices/system/node/` 来
检查的，而不是靠 `numactl`——云镜像默认没装这个包，装它需要一次在 TCG 慢速
slirp NAT 下可能耗时数分钟的 `apt-get`，相比直接读 sysfs 没有任何好处。

如果虚拟机在超时时间内没能通过 SSH 连通，脚本会记录串口最后的输出，把一条
`"timeout"` 状态写入 `tests/vm/results/qemu_smoke_<ts>.json`，然后正常退出（一
个跑得慢/跑不起来的 QEMU 环境不算作 NUMA 代码本身的 bug）。

### 6. CXLMemSim 设备级链路校验

```bash
./tests/cxl/run_cxlmemsim.sh [--timeout SECONDS] [--skip]
```

需要提前 clone `external/CXLMemSim`（来自 `SlugLab/CXLMemSim`），并构建好它自带
的改版 QEMU 和 `cxlmemsim_server`（`script/build_qemu.sh` + `cmake --build
build`）。下面的原生工作负载 bench 还需要 C++20 编译器和 `libfmt-dev`；缺少任
一依赖都会被跳过并记录原因。三项检查各自独立，各自可以优雅降级：

1. CXLMemSim 自己的 CTest 测试套件。
2. `tests/cxl/cxlmemsim_workload_bench.cpp`——链接 `libcxlmemsim.a`，直接通过
   CXLMemSim 自己的 `CXLMemExpander` C++ 模型重放 NUMAflow 评测框架所用的同一批
   zipf/uniform/hotspot/temporal 轨迹，写出
   `tests/cxl/results/cxlmemsim_native_bench_<ts>.json`。只需要静态库和头文件，
   不需要改版 QEMU。
3. 启动改版 QEMU，把一个 CXL Type2 端点连接到 `cxlmemsim_server`（TCP），确认设
   备真的完成了连接（在 QEMU 日志里检查 `"Device realized"` 和
   `"Connected to CXLMemSim"`）——不需要启动完整的客户机操作系统，QEMU 会在设备
   连接完成后立刻用 `-S` 暂停。

关于这一步为什么验证的是设备仿真链路本身、而不是一次完整的
"redis-server 跑在 CXLMemSim 客户机里"，以及真实的 Redis 层 DRAM-vs-远端内存对
比该去哪里找（`tests/ycsb/scripts/eval_cxl_memory.sh`，需要跑在一个真实的、至少
2 个 NUMA 节点的环境里，比如第 5 步里的那台虚拟机），见
[`ARCHITECTURE.md`](ARCHITECTURE.md#external-validation-layers)。

## 手动功能烟雾测试

日常改一个具体模块、不想跑完整套件时很有用：

```bash
./src/redis-server ./redis.conf --daemonize yes --logfile /tmp/r.log
./src/redis-cli numa config get
./src/redis-cli numa strategy list
./src/redis-cli numa migrate stats
./src/redis-benchmark -q -n 20000 -c 20 -t set,get
./src/redis-cli --cluster create 127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003 \
    --cluster-replicas 0   # 顺带练到 redis-cli 自身的 dict/迭代器代码路径，
                            # 不只是 redis-server
```
</content>
