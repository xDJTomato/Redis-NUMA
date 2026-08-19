# 11. 风险与技术债（Risks and Technical Debt）

> arc42 第 11 章。本章只记录**当前真实存在**的风险与技术债——每一项都能对应到
> 具体代码位置或一次真实验证过程，不做泛泛的"未来可能有风险"式表态。

## 11.1 内核中的占位实现：`ADAPTIVE` / `LATENCY_AWARE`

`numa_configurable_strategy` 暴露的 9 种分配策略里，`ADAPTIVE` 和
`LATENCY_AWARE` 在 Redis 内核（`src/numa_configurable_strategy.c`）中只有占位
实现，真正的自适应/延迟感知逻辑在 NUMAflow 的 `alloc_adaptive` /
`alloc_latency_aware` 原子操作里，需要通过 `NUMA FLOW LOAD` 显式接入才能生效。
**风险**：如果只读内核代码、不知道这条分层设计，容易误以为这两个策略"没做完"
或"是 bug"。**现状**：这是刻意的架构取舍（复杂的自适应逻辑放到独立子系统迭代，
不直接耦合进内核关键路径），已在 `ARCHITECTURE.md` 与 `docs/GUIDE.zh-CN.md`
3.5 节中明确记录，但内核代码本身没有运行时提示。

## 11.2 TinyLFU 与 Composite LRU 的互斥依赖人工配置

槎位 1（Composite LRU，默认开启）与槎位 2（TinyLFU，默认关闭）会争抢同一批 key
的迁移决策权。**风险**：如果运维人员手动 `NUMA STRATEGY SLOT ENABLE 2` 却忘记
`DISABLE 1`，两个策略会互相打架，没有代码层面的自动互斥保护。**缓解**：文档中
反复提示"启用 TinyLFU 前需手动关闭 Composite LRU"，但这是约定而非强制。

## 11.3 开发主机仅有 1 个物理 NUMA 节点

本 fork 实际开发所用的主机只有 1 个物理 NUMA 节点（`numactl --hardware` 显示
`available: 1 nodes (0)`），意味着任何"跨节点迁移是否真的生效"的验证，在这台
机器上都无法直接用真实硬件完成。**缓解路径**（三选一，各有取舍）：
NUMAflow 的纯软件仿真拓扑（`numa_shim.c`，确定性但终究是模拟）、QEMU 多节点客户
机（`tests/vm/boot_numa_vm.sh`，真实 `redis-server` 但纯 TCG 软件模拟、较慢）、
CXLMemSim 设备级仿真（`tests/cxl/`，仿真具体的 CXL 器件时序，但见下一条的限制）。

## 11.4 CXLMemSim 的 CXL Type2 设备需要定制内核才能暴露为客户机内存

本 session 中做了一次真实实验：用 CXLMemSim 自带的改版 QEMU 启动一个挂了
`cxl-type2` 设备的 Debian 12 云镜像客户机。客户机内核（发行版自带，未经修改）
能在 PCI 总线上正确识别设备（`0d:00.0 CXL [0502]: Intel Corporation Device
[8086:0d92]`），但标准 `cxl_pci` 驱动无法绑定它——手动
`echo 0000:0d:00.0 > .../drivers/cxl_pci/bind` 直接返回 I/O 错误，dmesg 里
没有任何日志。追查到 CXLMemSim 自己的
`qemu_integration/launch_qemu_vcs_dcd_gfam.sh` 脚本：它默认使用的
`KERNEL_IMAGE` 是一个 CXLMemSim 作者自己打了专用补丁的 Linux 内核
（`/root/linux-cxl-type2/arch/x86/boot/bzImage`），标准驱动没有实现这个模拟
设备所需的 DVSEC register-locator 能力。**结论**：这不是配置问题，而是
CXLMemSim 这个模拟设备本身的硬性要求；在没有那份专用内核的环境里，
`redis-server` 无法真正访问到 CXL 仿真内存。构建该内核被判断为超出本轮验证范围
（详见 `ARCHITECTURE.md`"External validation layers"一节的完整证据链）。

## 11.5 YCSB 依赖 JDK，沙箱默认未预装

本项目的 YCSB 带宽基准测试需要 Java 运行环境，但本沙箱默认没有 root/apt 权限来
安装系统级 JDK。**缓解**：改用一份便携式 JDK 17（Temurin，解压到
`~/.local/opt`，不需要 root），已验证可行并跑通完整的三阶段基准（Fill → Hotspot
→ Sustain）。**技术债**：这是一次性的手工规避，`run_full_validation.sh`
本身仍然只会检测 `java` 是否在 `PATH` 上，不会自动完成这个便携式安装。

## 11.6 历史 bug：非 `redis-server` 二进制上的分配器初始化顺序

`numa_init()` 只在 `server.c` 的 `main()` 里被调用，`redis-cli` /
`redis-benchmark` / `redis-check-rdb`/`aof` / `redis-sentinel` 都链接了
`zmalloc.o` 却从未调用它。历史上 `zmalloc_local()`/`zcalloc_local()`（`dict.c`
用）没有对 `numa_ctx.numa_available` 做判断，在这些二进制里无条件调用
`numa_alloc_dram()`，导致堆损坏（`redis-cli --cluster create` 触发过一次真实
SIGSEGV，详见 [`redis7-migration.md`](../redis7-migration.md) 5.5 节）。
**现状**：已修复（补上了与 `zmalloc()` 一致的判断 + 普通 `malloc` 退化）。
**遗留风险**：这是一类容易复发的 bug——任何新的 NUMA 路径代码，如果假设
`numa_ctx.numa_available` 恒为真，都可能在非 `redis-server` 二进制上重现同样的
问题，需要在 code review 时专门检查。

## 11.7 基准测试脚本的硬编码环境假设

- `run_bw_benchmark.sh` 默认 `--process-nodes 0,2`，假设主机至少有 3 个 NUMA
  节点；本 session 在只有 1 个节点的主机上运行时触发 `numactl` 报错
  （"node argument 2 is out of range"），需要显式传 `--process-nodes all` 才能
  跑通。脚本本身的默认值尚未改为自动探测。
- `run_bw_benchmark_vanilla.sh` 曾经硬编码对比基线路径为 `../redis-6.2.21`
  （迁移前的旧内核版本，不是公平的同版本对比）。本 session 已改为从本仓库自带的
  `7.2.6` tag 生成 `../redis-7.2.6-vanilla` worktree 并指向它，但如果未来需要对
  比更多版本，这类"外部路径硬编码"的模式仍然值得关注。

## 11.8 CXLMemSim 自身的两个已知问题（外部依赖，非本项目引入）

`tests/cxl/cxlmemsim_workload_bench.cpp` 直接驱动 CXLMemSim 的
`CXLMemExpander` C++ 模型时发现：① 不先对每次访问调用 `insert()` 就直接调用
`calculate_latency`/`calculate_bandwidth`，模型会对工作负载形状完全不敏感；
② `calculate_latency()` 内部的地址缓存失效逻辑存在缺陷，在全新 endpoint 上先
调用它会导致两个计算函数之后都静默返回 0。两者都通过调整调用顺序规避
（先 `insert()` 再 `calculate_bandwidth()`），而不是直接修改
`external/CXLMemSim`（该目录未被 vendor 进本仓库历史，属于外部依赖）。
**遗留风险**：这些是绕过而非修复，若 CXLMemSim 上游更新了这部分逻辑，需要重新
验证这两条规避是否仍然成立。
