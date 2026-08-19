# 7. 部署视图（Deployment View）

> arc42 §7。本章回答：**这套系统实际运行在什么样的机器/环境拓扑上，各部分部署
> 在哪里。** 分四种环境说明：生产环境、开发/单机验证环境、QEMU 多节点测试环境、
> CXLMemSim 设备仿真测试环境——后三者都是本仓库用来验证 NUMA/CXL 代码路径的测试
> 基础设施，不是生产部署形态。

## 7.1 生产部署形态：单机 redis-server + 真实 NUMA/CXL 硬件

```text
┌──────────────────────────────────────────────────────────────┐
│  物理主机                                                     │
│                                                                │
│   NUMA Node 0            NUMA Node 1           CXL 扩展节点   │
│  ┌────────────┐        ┌────────────┐        ┌────────────┐  │
│  │ CPU + DRAM │◄──────►│ CPU + DRAM │◄──────►│  CXL 内存   │  │
│  └────────────┘  UPI/  └────────────┘  CXL.mem└────────────┘  │
│        ▲          Infinity Fabric           链路      ▲       │
│        │                                              │       │
│        └──────────────────┬───────────────────────────┘       │
│                            │                                  │
│                   ┌────────▼─────────┐                        │
│                   │  redis-server     │  单进程、单实例          │
│                   │  （本项目二进制）  │  （NUMA 感知，无需       │
│                   │                   │   额外 sidecar 进程）   │
│                   └───────────────────┘                        │
└──────────────────────────────────────────────────────────────┘
```

这是最简单的部署形态：**一个 `redis-server` 进程，直接跑在裸机（或独占该拓扑的
虚拟机）上**，没有额外的守护进程、没有 sidecar、没有网络依赖的组件。NUMA 拓扑发
现、CXL 节点识别、迁移调度全部在 `redis-server` 进程内部完成，通过 `libnuma` 读
取内核暴露的拓扑信息。

前置条件（见 [`README.md`](../../README.md)「Building」一节）：
- Linux + `libnuma-dev`（或 `numactl-devel`）
- 编译时强制 `MALLOC=libc`（不能是 jemalloc，见 [09-architecture-decisions.md](09-architecture-decisions.md)）
- 该物理主机确实存在 ≥2 个 NUMA 节点，其中一个可以是 CXL 扩展内存节点（对
  `libnuma`/内核而言，CXL 节点表现为一个普通的、访问延迟更高的 NUMA 节点）

## 7.2 开发/单机验证环境（本仓库实际开发所用）

本仓库自身的开发主机**只有 1 个物理 NUMA 节点**——这不是缺陷，而是这个项目大多
数验证工作必须依赖下面两种测试环境的直接原因：

- `./utils/numa/check_numa_config.sh`、`diagnose_numa.sh` 在单节点主机上会优雅
  降级，用于基本的健全性检查；
- NUMAflow 子系统（`numaflow/`）提供了一套**纯 C11、无需真实多节点硬件**的拓扑
  仿真（`numa_shim.c`），可以在任意一台机器（包括单节点开发机，甚至一台 16GB
  的 Windows 笔记本）上确定性地评测迁移策略——这也是为什么 NUMAflow 被设计成
  完全独立于 `libnuma` 的子系统。详见 [`modules/`](modules/) 下 NUMAflow 相关
  文档与 `docs/numaflow/README.md`。

## 7.3 QEMU 多 NUMA 节点测试环境

```text
┌───────────────────────────────────────────────────┐
│  开发主机（1 个物理 NUMA 节点，无 /dev/kvm）         │
│                                                     │
│   QEMU（纯 TCG 软件模拟，无硬件虚拟化加速）           │
│  ┌───────────────────────────────────────────┐     │
│  │  客户机：Debian 12 云镜像                   │     │
│  │  -object memory-backend-ram (x2)            │     │
│  │  -numa node,memdev=... (x2)  → 2 个模拟节点 │     │
│  │                                             │     │
│  │  redis-server / redis-cli / redis-benchmark │     │
│  │  （本机编译好后拷入客户机运行）                │     │
│  └───────────────────────────────────────────┘     │
└───────────────────────────────────────────────────┘
```

由脚本 `tests/vm/boot_numa_vm.sh` 驱动：启动一个带 2 个模拟 NUMA 节点的 Debian
12 云镜像客户机（本机没有 `/dev/kvm`，纯 TCG 软件模拟，启动慢但可用），等待 SSH
就绪（默认超时 480 秒，超时视为环境不可用而非代码 bug），把本机编译好的
`redis-server`/`redis-cli`/`redis-benchmark` 拷进去，在客户机**内部**跑
`PING`/`SET`/`GET`、`NUMA CONFIG GET`、`NUMA FLOW LIST` 和一次
`redis-benchmark`。这是本仓库唯一一个能验证「`redis-server` 真的在一个多 NUMA
节点拓扑里跑」的环境。

## 7.4 CXLMemSim 设备仿真测试环境

```text
┌────────────────────────────────────────────────────────────┐
│  开发主机                                                    │
│                                                              │
│  cxlmemsim_server（独立进程，TCP 服务端，仿真 CXL 设备时序）    │
│         ▲                                                   │
│         │ TCP（CXL Type2 端点 <-> server 时序转发协议）        │
│         │                                                   │
│  CXLMemSim 改版 QEMU（-M q35,cxl=on + pxb-cxl/cxl-rp/cxl-type2）│
│  ┌────────────────────────────────────────────────────┐    │
│  │  客户机：Debian 12 云镜像（发行版原生内核）             │    │
│  │                                                      │    │
│  │  lspci 可见 CXL 设备：                                │    │
│  │    0d:00.0 CXL [0502]: Intel Corporation Device       │    │
│  │    [8086:0d92]                                        │    │
│  │                                                      │    │
│  │  ⚠ 但设备内存从未真正暴露为客户机可见 RAM/NUMA 容量：  │    │
│  │    /sys/bus/cxl/devices/ 下只有 root0/port1/          │    │
│  │    decoder1.0，没有 mem0/endpoint0；手动 bind 返回     │    │
│  │    I/O error，dmesg 无日志——静默探测失败                │    │
│  └────────────────────────────────────────────────────┘    │
└────────────────────────────────────────────────────────────┘
```

本会话中实际验证过的现实边界（这不是本项目代码的问题，是 CXLMemSim 这个外部工
具自身的要求）：CXLMemSim 官方脚本
`qemu_integration/launch_qemu_vcs_dcd_gfam.sh` 默认使用的
`KERNEL_IMAGE` 是一份**作者自己打了专用补丁的 Linux 内核**
（`/root/linux-cxl-type2/arch/x86/boot/bzImage`）——因为这个 `cxl-type2`
仿真设备没有实现标准 `cxl_pci` 驱动依赖的 DVSEC register-locator 能力，发行版
自带的标准内核驱动无法绑定它。也就是说：**要让 `redis-server` 真正把数据写进
CXLMemSim 仿真出来的内存，前提是先编译出这份专用内核**——这份内核在本仓库的
验证环境里没有构建（工作量是另一项数小时级的独立任务），所以本仓库对
CXLMemSim 的验证目前停在「设备链路层」：

- `tests/cxl/run_cxlmemsim.sh` 验证的是 QEMU &harr; `cxlmemsim_server` 的设备级
  链路是否真的连通（`"Device realized"`、`"Connected to CXLMemSim"`），以及
  CXLMemSim 自己的 CTest 套件、`tests/cxl/cxlmemsim_workload_bench.cpp` 直接调
  用 `libcxlmemsim.a` 的时序模型——三者都不需要客户机操作系统完整启动。
- 真正的「Redis 数据读写落在 CXL 仿真内存上」的验证，需要先解决上面的专用内核
  依赖；退而求其次的「Redis 层 DRAM vs. 远端内存」对比，用的是
  `tests/ycsb/scripts/eval_cxl_memory.sh`（`numactl --membind` 跨 7.3 节所述的
  真实多节点环境，不依赖 CXLMemSim）。

完整背景与两个 CXLMemSim 自身的时序模型坑，见
[`ARCHITECTURE.md`](../../ARCHITECTURE.md#external-validation-layers)。

## 7.5 NUMAflow 子系统的独立部署形态

`numaflow/` 是纯 C11、零 Redis/libnuma 依赖的子系统，因此它有**两种完全不同的
部署方式**，且互不影响：

1. **嵌入进 `redis-server`**（生产场景）：编译进 `HAVE_NUMA` 构建，作为一个可选
   的策略引擎，通过 `NUMA FLOW LOAD/RUN/LIST/STATUS/UNLOAD/ADAPT` 命令加载/管理，
   `serverCron` 按配置间隔运行——这是 7.1 节部署图里「进程内嵌入」画的那样。
2. **完全独立运行**（研究/评测场景）：`cd numaflow && make` 产出一个独立的
   `numaflow` 命令行工具 + `nf_tui` 交互界面 + `python gui/server.py` 启动的
   Web GUI（`http://127.0.0.1:8090`），三者都不需要 Redis 进程存在，可以在任何
   有 C11 编译器的机器上跑（包括 Windows）。

两种部署方式共享同一份引擎代码（`numaflow/src/`），差异只在于「谁调用它」。
