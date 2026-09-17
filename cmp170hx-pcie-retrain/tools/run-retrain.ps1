# CMP 170HX Gen2 重训练引导脚本：前置核验 -> apply -> NVIDIA 侧复核 -> 可选带宽
# 用法：管理员 PowerShell，先完成 0.11 MemoryGen2 且 NVIDIA 健康接管后再运行
param(
    [switch]$SkipPrecheck,       # 跳过 64GiB 前置核验(仅调试)
    [string]$BandwidthExe = ''   # 例: C:\Users\xkw19\Documents\170hx\cmp170hx-gen2-efi\dist\pcie-bandwidth.exe
)
$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[System.Text.Encoding]::UTF8  # retrainctl 输出为 UTF-8
$root=Split-Path $PSScriptRoot
$id=[Security.Principal.WindowsIdentity]::GetCurrent()
$prin=New-Object Security.Principal.WindowsPrincipal($id)
if(!$prin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw '需要管理员 PowerShell'}
New-Item -ItemType Directory -Force "$root\logs" | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmss'
$log="$root\logs\run-retrain-$stamp.log"
Start-Transcript -Path $log | Out-Null
try {
    Write-Host "== 0. 前置状态 =="
    $smi='nvidia-smi'
    $before=& $smi --query-gpu=name,pci.bus_id,memory.total,pcie.link.gen.current,pcie.link.width.current --format=csv
    $before | Write-Host
    if(!$SkipPrecheck){
        $bad=$false
        foreach($line in ($before | Select-Object -Skip 1)){
            $cols=$line -split ',\s*'
            if($cols[2] -notmatch '65536'){Write-Host "未在 64GiB 解锁态: $line";$bad=$true}
            if($cols[3] -ne '1'){Write-Host "当前链路不是 Gen1(已是 Gen2?): $line"}
        }
        if($bad){throw '未检测到 64GiB 解锁态；请先运行 cmp170hx-windows-driver 0.11 的 MemoryGen2 流程并确认 NVIDIA 健康接管（或用 -SkipPrecheck 强制）'}
    }

    Write-Host "`n== 1. 只读巡检 =="
    & "$root\dist\retrainctl.exe" --query
    if($LASTEXITCODE -gt 4){throw "query failed rc=$LASTEXITCODE（先运行 tools\install.ps1）"}

    Write-Host "`n== 2. 执行 Gen2 重训练 =="
    & "$root\dist\retrainctl.exe" --apply
    $rc=$LASTEXITCODE
    Write-Host "apply rc=$rc"

    Write-Host "`n== 3. 复核（物理 LNKSTA 为准；nvidia-smi 仅供参考）=="
    $after=& $smi --query-gpu=pci.bus_id,memory.total,pcie.link.gen.current,pcie.link.width.current --format=csv
    $after | Write-Host
    if($rc -eq 0){
        Write-Host "PASS: 驱动侧确认双端物理 Gen2 x4（LNKSTA 硬件真值）"
        Write-Host "注: nvidia-smi 的 pcie.link.gen.current 是 NVIDIA 驱动 probe 时缓存的速率；"
        Write-Host "    物理重训练发生在其初始化之后，该字段不会刷新（Linux cmpunlocker 正是因此"
        Write-Host "    才必须在 probe 阶段做重训练）。最终判据是下面的实际带宽，不是这个字段。"
    } else {
        Write-Host "重训练未全部成功 (rc=$rc)。保留本日志与 dist\logs\retrain-*.log 供分析"
    }

    if($BandwidthExe -and (Test-Path $BandwidthExe) -and $rc -eq 0){
        Write-Host "`n== 4. 256MiB pinned 带宽验证（逐卡 CUDA_VISIBLE_DEVICES，每向 12s）=="
        foreach($dev in 0,1){
            Write-Host "-- CUDA device $dev --"
            $env:CUDA_VISIBLE_DEVICES="$dev"
            & $BandwidthExe
            Write-Host "bandwidth dev$dev rc=$LASTEXITCODE"
        }
        Remove-Item Env:\CUDA_VISIBLE_DEVICES -ErrorAction SilentlyContinue
        Write-Host "判据：H2D/D2H 约 1.5-1.7 GB/s = Gen2 x4 实锤；约 0.8 GB/s = 仍是 Gen1"
    }
    Write-Host "`nlog: $log"
} finally {
    Stop-Transcript | Out-Null
}
