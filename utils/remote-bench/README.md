# Redis-NUMA 一键远程压测流水线

在 **64GB 云服务器**上一步完成: 推送最新代码 -> 启动 QEMU (模拟 CXL / 双 NUMA)
-> guest 内编译 + 完整压力测试 -> 回拉结果到本地。

## 链路

```
[本地 Windows]  one_shot.ps1
   1. tar 打包工作区(含未提交修改)
   2. scp 上传 tar.gz + 两个脚本
   3. ssh 执行 remote_pipeline.sh ──┐
                                    ▼
[远端服务器]    remote_pipeline.sh (root)
   1. 安装 qemu-system / cloud-image-utils
   2. 下载 Ubuntu 24.04 cloud image + cloud-init seed(注入 ssh key)
   3. 启动 QEMU: Node0=DRAM / Node1=模拟CXL, hostfwd 2222->22
   4. 等待 guest ssh 就绪
   5. 执行 guest_benchmark.sh ──┐
                               ▼
[QEMU guest]    guest_benchmark.sh (ubuntu@localhost:2222)
   A. 挂载 9p 共享目录(宿主机 /root/redis-numa)
   B. rsync 同步代码 -> C. 安装依赖 -> D. make 编译(自动 HAVE_NUMA)
   E. 安装 YCSB -> F. run_bw_benchmark.sh --process-nodes 0,1
   G. 结果 rsync 写回宿主机
                               │
   结果经 9p 落回宿主机 /root/redis-numa/tests/ycsb/results/
                               │
[本地 Windows]  one_shot.ps1 最后一步: scp -r 回拉 results 到本地
```

## 前置条件

| 位置 | 要求 |
|------|------|
| 本地 | Windows 10/11 + PowerShell 7, 自带 `ssh`/`scp`/`tar` |
| 远端 | Ubuntu 20.04+ / Debian 12, root 登录, 可访问外网, 64GB 内存 |
| 远端 CPU | **有 /dev/kvm** (物理机或开启嵌套虚拟化的云服务器), 否则 TCG 模拟压测会慢 10-50 倍 |

> 云服务器默认关闭嵌套虚拟化, 请在控制台开启 (如阿里云"嵌套虚拟化"开关、
> 华为云/KVM 宿主机的 `/sys/module/kvm_intel/parameters/nested`)。

## 一键运行

```powershell
# 最简 (默认: bw 三阶段压测, QEMU 双 NUMA 节点 4G+8G, 完成后保留 VM)
.\tools\one_shot.ps1 -RemoteHost 1.2.3.4 -RemoteUser root

# 指定压测/模拟方式
.\tools\one_shot.ps1 -RemoteHost 1.2.3.4 -BenchMode stress -SimMode cxl -Node1Mem 16g -MaxMem 10000mb

# 完成后关闭 VM
.\tools\one_shot.ps1 -RemoteHost 1.2.3.4 -KeepVm:$false
```

## 参数

| 参数 | 默认 | 说明 |
|------|------|------|
| `-RemoteHost` | 必填 | 远端服务器 IP/域名 |
| `-RemoteUser` | root | 远端登录用户 |
| `-SSHPort` | 22 | 远端 ssh 端口 |
| `-BenchMode` | bw | `bw`=带宽饱和三阶段(填充→热点→持续) / `stress`=100万条8KB / `baseline`=轻量 |
| `-SimMode` | numa | `numa`=双NUMA节点(最稳) / `cxl`=真CXL设备仿真(需 QEMU 7.2+) |
| `-Node0Mem` | 4g | Node 0 DRAM |
| `-Node1Mem` | 8g | Node 1 模拟 CXL |
| `-GuestCpus` | 4 | guest vCPU (≥2) |
| `-MaxMem` | 6000mb | redis maxmemory |
| `-ResultDir` | 自动 | 本地结果目录 |
| `-KeepVm` | on | 测试后保留 VM (可再连进去调试) |

## 产物

- 本地: `results_remote_<时间戳>/` 下为 `load.txt` / `run.txt` / `sysinfo.txt` / `redis.log` 等
- 远端: `/root/redis-numa/tests/ycsb/results/`, guest 磁盘 `/root/qemu-vm/`
- 手动进 guest: `ssh -i /root/qemu-vm/id_ed25519 -p 2222 ubuntu@<服务器IP>`

## 注意事项

- **CXL 模式 (`--sim-mode cxl`)** 用 `cxl-type3` 设备 + `memory-backend-file` 仿真,
  guest 内核(CXL 驱动)需把 CXL 内存在线后才出现在 `numactl --hardware` 中;
  若仍显示 1 节点, 请回退 `numa` 模式。Redis-NUMA 只依赖 OS 层 NUMA 节点, 两种模式验证效果一致。
- 远端流水线可幂等重跑: VM 已在运行会复用, guest 内测试结果每次写入新时间戳目录。
- `bw` 模式默认 `--process-nodes 0,1`(对应 guest 双节点), 勿在 guest 里用宿主机习惯的 `0,2`。
- 首次运行需下载约 600MB cloud image + YCSB, 之后增量很快。
