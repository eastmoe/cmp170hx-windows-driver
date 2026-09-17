#Requires -RunAsAdministrator
param([string]$Evidence='')
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=Split-Path $PSScriptRoot
if(!$Evidence){$Evidence="$root\logs\cleanup-0.10-MemoryGen2-20260916-140651-454"}
$out="$root\logs\pcie-resume-"+(Get-Date -Format yyyyMMdd-HHmmss)
New-Item -ItemType Directory -Path $out | Out-Null
$ctl="$root\dist\cmpctl.exe"
function Ctl([string[]]$Arguments,[string]$Name) {
    $lines=@(& $ctl @Arguments 2>&1);$code=$LASTEXITCODE
    $text=$lines | Out-String -Width 300
    $text | Set-Content "$out\$Name.txt"
    Write-Host $text
    [pscustomobject]@{Code=$code;Text=$text}
}
function Device {
    $list=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Match '^PCI\\VEN_10DE&DEV_20C2&')
    if($list.Count -ne 1){throw 'Expected exactly one 20C2.'}
    $list[0]
}
function Prop($d,$key){(Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName $key).Data}
$success=$false
Start-Transcript "$out\transcript.txt" | Out-Null
try {
    foreach($line in Get-Content "$root\dist\SHA256SUMS.txt") {
        if($line -notmatch '^([0-9a-fA-F]{64})  ([\w.-]+)$'){throw 'Invalid package manifest.'}
        if((Get-FileHash "$root\dist\$($Matches[2])").Hash -ne $Matches[1]){throw 'Package hash mismatch.'}
    }
    $boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    if($boot -ne (Get-Content "$Evidence\boot.txt" -Raw).Trim()){throw 'Different boot: use the cold MemoryGen2 test instead.'}
    $task=Get-ScheduledTask -TaskName CMP170HX-AutoUnlock -ErrorAction SilentlyContinue
    if($task -and $task.State -eq 'Running'){throw 'Auto unlock is running.'}
    if($task){$task | Disable-ScheduledTask | Out-Null}
    $d=Device
    if((Prop $d DEVPKEY_Device_Service) -ne 'cmp170' -or (Prop $d DEVPKEY_Device_ProblemCode) -ne 0){throw 'Resume requires the healthy, still-bound experimental driver.'}
    $recovery=Get-Content "$root\backup\restore.json" -Raw | ConvertFrom-Json
    if($recovery.InstanceId -ne $d.InstanceId -or !(Test-Path -LiteralPath $recovery.OriginalInf)){throw 'NVIDIA recovery record mismatch.'}
    $baseline=@(Get-Content "$Evidence\memory-baseline.csv" | ConvertFrom-Csv -Header uuid,id,bus,mib | Where-Object {$_.id.Trim() -eq '0x20C210DE'})
    if($baseline.Count -ne 1){throw 'Expected exactly one saved NVIDIA target.'}
    $uuid=$baseline[0].uuid.Trim();$bus=$baseline[0].bus.Trim()
    $pre=Ctl @('--diag') 'old-driver-live'
    if($pre.Code -ne 7 -or
       $pre.Text -notmatch 'protocol=7 build=000A000[12] device=20C2 state=2 status=0xC0000483 stage=2[13] mode=2' -or
       $pre.Text -notmatch 'RUN checks=0x16 ' -or
       $pre.Text -notmatch 'DMA_released=1 DMA_retained=0' -or
       $pre.Text -notmatch 'RUN cleanup_attempted=[01] mismatch_mask=0x0 '){throw 'Live state differs from the diagnosed 0.10 failure; no resume.'}
    $bind=Ctl @('--bind',"$root\dist\cmp170.inf",'--ack-device-rebind') 'bind-fixed'
    if($bind.Code -ne 0){throw 'Rebind failed or requires reboot. No continuation.'}
    $fresh=Ctl @('--diag') 'fixed-driver-live'
    if($fresh.Code -ne 0 -or $fresh.Text -notmatch 'protocol=7 build=000A0003 device=20C2 state=0 '){throw 'Fresh fixed driver not loaded. No continuation.'}
    $run=Ctl @('--pcie-resume','--ack-experimental') 'resume'
    $diag=Ctl @('--diag') 'post-resume'
    foreach($result in $run,$diag) {
        if($result.Code -ne 0 -or $result.Text -notmatch 'state=1 status=0x00000000 stage=30 mode=2' -or
           $result.Text -notmatch 'PCIE requested=2 configured=1 ' -or $result.Text -notmatch 'RUN checks=0x1F '){throw 'Resume not verified. Preserve logs; no NVIDIA handover.'}
    }
    $handover=Ctl @('--memory-handover','--ack-experimental') 'handover'
    if($handover.Code -ne 0 -or $handover.Text -notmatch 'MEMORY handover=1 no_flr=1 cached=2'){throw 'Handover not verified.'}
    $restore=Ctl @('--bind',$recovery.OriginalInf,'--ack-device-rebind') 'restore'
    if($restore.Code -ne 0){throw 'NVIDIA restore failed or requires restart.'}
    $healthy=$false
    for($i=0;$i -lt 30;$i++) {
        $d=Device
        if((Prop $d DEVPKEY_Device_Service) -eq 'nvlddmkm' -and (Prop $d DEVPKEY_Device_ProblemCode) -eq 0){$healthy=$true;break}
        Start-Sleep 1
    }
    if(!$healthy){throw 'NVIDIA did not recover normally.'}
    & nvidia-smi.exe -q *> "$out\nvidia.txt"
    if($LASTEXITCODE){throw 'NVIDIA-SMI failed.'}
    $seen=$false
    for($i=0;$i -lt 10;$i++) {
        $lines=@(& nvidia-smi.exe -i $uuid '--query-gpu=uuid,pci.bus_id,memory.total,pcie.link.gen.current,pcie.link.gen.max,pcie.link.width.current' '--format=csv,noheader,nounits')
        if($LASTEXITCODE){throw 'NVIDIA acceptance query failed.'}
        $lines | Add-Content "$out\acceptance.csv"
        $cards=@($lines | ConvertFrom-Csv -Header uuid,bus,mib,current,max,width)
        if($cards.Count -ne 1 -or $cards[0].uuid.Trim() -ne $uuid -or $cards[0].bus.Trim() -ne $bus){throw 'Restored target mismatch.'}
        $card=$cards[0]
        if([int]$card.mib.Trim() -lt 64512 -or [int]$card.mib.Trim() -gt 65536){throw '64GiB not accepted.'}
        if($card.current.Trim() -eq '2' -and [int]$card.width.Trim() -gt 0){$seen=$true;break}
        Start-Sleep 1
    }
    if(!$seen){throw 'NVIDIA restored with 64GiB, but actual Gen2 not observed; inspect acceptance.csv under load. No automatic retry.'}
    $success=$true
} catch {
    $_ | Out-String | Set-Content "$out\failure.txt"
    Write-Host $_
} finally {
    try { . "$PSScriptRoot\diagnostics.ps1"; Save-CmpDiagnostics "$out\after" } catch { $_ | Out-String | Set-Content "$out\diagnostics-error.txt" }
    @{Success=$success;Evidence=$out} | ConvertTo-Json | Set-Content "$out\result.json"
    Stop-Transcript | Out-Null
}
if(!$success){exit 1}
