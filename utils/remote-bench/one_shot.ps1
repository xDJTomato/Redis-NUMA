# ============================================================================
# one_shot.ps1 — Redis-NUMA 端到端一键测试（本地 Windows 侧）
#
# 链路: 打包工作区 -> scp 上传到远端服务器 -> ssh 执行 remote_pipeline.sh
#       (QEMU 模拟 CXL/双 NUMA 节点 -> guest 内编译 + YCSB 完整压测)
#       -> scp 回拉测试结果到本地
#
# 用法 (PowerShell 7+):
#   .\tools\one_shot.ps1 -RemoteHost 1.2.3.4 -RemoteUser root
#   .\tools\one_shot.ps1 -RemoteHost 1.2.3.4 -BenchMode stress -SimMode cxl -Node1Mem 16g
#
# 依赖: 本地需要 ssh/scp/tar (Windows 10+ 自带 OpenSSH 与 bsdtar)
#       远端需要 Ubuntu/Debian + KVM (云服务器需开启嵌套虚拟化)
# ============================================================================
param(
    [Parameter(Mandatory = $true)]
    [string]$RemoteHost,                       # 远端服务器 IP/域名
    [string]$RemoteUser = "root",              # 远端登录用户 (推荐 root)
    [int]$SSHPort = 22,                        # 远端 ssh 端口
    [ValidateSet("bw", "stress", "baseline")]
    [string]$BenchMode = "bw",                 # 测试模式
    [ValidateSet("numa", "cxl")]
    [string]$SimMode = "numa",                 # numa=双NUMA节点(稳) cxl=真CXL设备仿真
    [string]$Node0Mem = "4g",                  # Node 0 DRAM 大小
    [string]$Node1Mem = "8g",                  # Node 1 模拟 CXL 大小
    [int]$GuestCpus = 4,                       # guest vCPU 数
    [string]$MaxMem = "6000mb",                # redis maxmemory
    [string]$ResultDir = "",                   # 本地结果目录 (默认 results_remote_<时间戳>)
    [switch]$KeepVm,                           # 测试后保留远端 VM 运行
    [switch]$Help
)

$ErrorActionPreference = "Stop"

function Write-Step($msg)  { Write-Host "`n==== $msg ====" -ForegroundColor Cyan }
function Write-Ok($msg)    { Write-Host "[OK] $msg" -ForegroundColor Green }
function Write-Warn2($msg) { Write-Host "[WARN] $msg" -ForegroundColor Yellow }
function Write-Err($msg)   { Write-Host "[ERR] $msg" -ForegroundColor Red }

if ($Help) {
    Get-Content $MyInvocation.MyCommand.Path | Select-Object -First 24 | Where-Object { $_ -like '#*' } | ForEach-Object { $_.Substring(2) }
    exit 0
}

# ── 0. 前置检查 ─────────────────────────────────────────────────────────────
foreach ($cmd in @("ssh", "scp", "tar")) {
    if (-not (Get-Command $cmd -ErrorAction SilentlyContinue)) {
        Write-Err "缺少命令: $cmd (Windows 10+ 请安装 OpenSSH 客户端)"
        exit 1
    }
}
$RepoRoot = Split-Path $PSScriptRoot -Parent
$RemoteTools = "/root/redis-numa-tools"
$RemoteProject = "/root/redis-numa"

$sshOpts = @("-p", "$SSHPort", "-o", "StrictHostKeyChecking=no")
$remoteHostUri = "${RemoteUser}@${RemoteHost}"

# ── 1. 打包工作区 (含未提交修改 = 最新代码) ────────────────────────────────
Write-Step "1/5 打包工作区 (排除 .git/编译产物/历史结果)"
$tarLocal = Join-Path $env:TEMP ("redis-numa-" + (Get-Date -Format "yyyyMMddHHmmss") + ".tar.gz")
Push-Location $RepoRoot
try {
    & tar.exe -czf $tarLocal `
        --exclude=".git" `
        --exclude="tests/ycsb/ycsb-0.17.0" `
        --exclude="tests/ycsb/results" `
        --exclude="monitor/cmm-d-ctap_v2.2.4/ctap" `
        --exclude="*.o" `
        --exclude="src/redis-server" --exclude="src/redis-cli" --exclude="src/redis-benchmark" `
        .
    if ($LASTEXITCODE -ne 0) { throw "tar 打包失败" }
}
finally { Pop-Location }
Write-Ok "打包完成: $tarLocal ($([math]::Round((Get-Item $tarLocal).Length / 1MB, 1)) MB)"

# ── 2. 上传代码与脚本 ───────────────────────────────────────────────────────
Write-Step "2/5 上传到远端 $RemoteHost"
& ssh @sshOpts $remoteHostUri "mkdir -p $RemoteTools" 2>$null
if ($LASTEXITCODE -ne 0) { throw "无法连接远端或创建目录失败" }
& scp @sshOpts $tarLocal "${remoteHostUri}:${RemoteTools}/redis-numa.tar.gz"
if ($LASTEXITCODE -ne 0) { throw "上传 tar.gz 失败" }
& scp @sshOpts (Join-Path $PSScriptRoot "remote_pipeline.sh") "${remoteHostUri}:${RemoteTools}/remote_pipeline.sh"
if ($LASTEXITCODE -ne 0) { throw "上传 remote_pipeline.sh 失败" }
& scp @sshOpts (Join-Path $PSScriptRoot "guest_benchmark.sh") "${remoteHostUri}:${RemoteTools}/guest_benchmark.sh"
if ($LASTEXITCODE -ne 0) { throw "上传 guest_benchmark.sh 失败" }
Write-Ok "上传完成"

# ── 3. 远端解压 + 编排流水线 ────────────────────────────────────────────────
Write-Step "3/5 远端执行 QEMU + 压力测试 (可能耗时 20-60 分钟)"
$keep = if ($KeepVm) { "1" } else { "0" }
$remoteCmd = "set -e; " +
    "rm -rf $RemoteProject && mkdir -p $RemoteProject && " +
    "tar -xzf $RemoteTools/redis-numa.tar.gz -C $RemoteProject && " +
    "chmod +x $RemoteTools/remote_pipeline.sh $RemoteTools/guest_benchmark.sh && " +
    "bash $RemoteTools/remote_pipeline.sh --bench-mode $BenchMode --sim-mode $SimMode " +
    "--node0-mem $Node0Mem --node1-mem $Node1Mem --cpus $GuestCpus " +
    "--maxmem $MaxMem --keep-vm $keep"
& ssh @sshOpts $remoteHostUri $remoteCmd
if ($LASTEXITCODE -ne 0) {
    Write-Err "远端流水线失败 (exit=$LASTEXITCODE)，可加 -ResultDir 保留日志排查"
    exit $LASTEXITCODE
}

# ── 4. 回拉结果 ─────────────────────────────────────────────────────────────
Write-Step "4/5 回拉测试结果到本地"
if (-not $ResultDir) {
    $ResultDir = Join-Path $RepoRoot ("results_remote_" + (Get-Date -Format "yyyyMMdd_HHmmss"))
}
New-Item -ItemType Directory -Path $ResultDir -Force | Out-Null
& scp @sshOpts -r "${remoteHostUri}:${RemoteProject}/tests/ycsb/results/*" "${ResultDir}/"
if ($LASTEXITCODE -ne 0) {
    Write-Err "回拉结果失败，远端结果仍在 ${RemoteProject}/tests/ycsb/results/"
    exit $LASTEXITCODE
}
Write-Ok "结果已保存到: $ResultDir"

# ── 5. 汇总 ─────────────────────────────────────────────────────────────────
Write-Step "5/5 完成"
Write-Ok "本地结果目录:  $ResultDir"
if ($KeepVm) {
    Write-Ok "远端 VM 仍在运行，手动连接: ssh -i /root/qemu-vm/id_ed25519 -p 2222 ubuntu@$RemoteHost"
    Write-Ok "停止 VM:        ssh $remoteHostUri 'bash /root/redis-numa-tools/remote_pipeline.sh --keep-vm 0'  (或在远端执行 poweroff)"
} else {
    Write-Ok "远端 VM 已关闭"
}
Write-Ok "报告: 可查看 $ResultDir 下的 load.txt / run.txt / sysinfo.txt"
