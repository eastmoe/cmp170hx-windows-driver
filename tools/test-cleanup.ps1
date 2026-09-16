#Requires -RunAsAdministrator
param([ValidateSet('Switch','ResetOnly','Compute','Memory')][string]$Mode='Switch')
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=Split-Path $PSScriptRoot
. "$PSScriptRoot\diagnostics.ps1"
$out=Join-Path "$root\logs" ('cleanup-0.9-'+$Mode+'-'+(Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Path $out | Out-Null
$ctl="$root\dist\cmpctl.exe"
$bound=$false;$runStarted=$false;$canRestore=$true;$failure=$null;$originalInf=$null
function Target {
    $items=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Match '^PCI\\VEN_10DE&DEV_(20C2|2082)&')
    if($items.Count -ne 1){throw 'Expected exactly one present CMP GPU.'}
    $items[0]
}
function Property($device,[string]$key) {
    (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName $key -ErrorAction Stop).Data
}
function Ctl([string[]]$Arguments,[string]$Name) {
    $lines=@(& $ctl @Arguments 2>&1);$code=$LASTEXITCODE
    $body=$lines | Out-String -Width 300
    $body | Set-Content "$out\$Name.txt" -Encoding UTF8
    Write-Host $body
    [pscustomobject]@{Code=$code;Text=$body}
}
function Check-Package {
    foreach($line in Get-Content "$root\dist\SHA256SUMS.txt") {
        if($line -notmatch '^([0-9a-fA-F]{64})  ([\w.-]+)$'){throw 'Invalid package hash manifest.'}
        $wanted=$Matches[1];$name=$Matches[2]
        if((Get-FileHash "$root\dist\$name").Hash -ne $wanted){throw "Package hash mismatch: $name"}
    }
    foreach($name in 'cmp170.sys','cmp170.inf','cmp170.cat','cmpctl.exe') {
        if(!(Test-Path "$root\dist\$name")){throw "Missing signed package file: $name"}
    }
    Copy-Item "$root\dist\SHA256SUMS.txt" "$out\package-hashes.txt"
}
Start-Transcript -LiteralPath "$out\transcript.txt" | Out-Null
try {
    Check-Package
    $task=Get-ScheduledTask -TaskName CMP170HX-AutoUnlock -ErrorAction SilentlyContinue
    if($task -and $task.State -eq 'Running'){throw 'Auto unlock is running; wait for it to finish.'}
    if($task){$task | Disable-ScheduledTask | Out-Null}
    $device=Target;$instance=$device.InstanceId
    if((Property $device DEVPKEY_Device_Service) -ne 'nvlddmkm' -or
       (Property $device DEVPKEY_Device_ProblemCode) -ne 0){throw 'Start from healthy NVIDIA after a full power cycle, without EFI unlock.'}
    & nvidia-smi.exe -q | Set-Content "$out\baseline-nvidia.txt"
    if($LASTEXITCODE){throw 'NVIDIA-SMI baseline failed.'}
    if($Mode -eq 'Memory') {
        $rows=@(& nvidia-smi.exe '--query-gpu=uuid,pci.device_id,pci.bus_id,memory.total' '--format=csv,noheader,nounits')
        if($LASTEXITCODE){throw 'Cannot identify the NVIDIA memory target.'}
        $rows | Set-Content "$out\memory-baseline.csv"
        $cards=@($rows | ConvertFrom-Csv -Header uuid,id,bus,mib | Where-Object {$_.id.Trim() -eq '0x20C210DE'})
        if($cards.Count -ne 1){throw 'Memory test requires exactly one NVIDIA 20C2 target.'}
        $memoryUuid=$cards[0].uuid.Trim();$memoryBus=$cards[0].bus.Trim()
        $stockMiB=0
        if(![int]::TryParse($cards[0].mib.Trim(),[ref]$stockMiB) -or $stockMiB -lt 7000 -or $stockMiB -gt 8192){throw 'NVIDIA baseline is not stock 8GiB. Fully power cycle first.'}
    }
    $boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    $boot | Set-Content "$out\boot.txt"
    Save-CmpDiagnostics "$out\before"
    $bound=$true # Bind can fail after changing PnP; recovery must still be attempted.
    & "$root\manage.ps1" -Action Bind
    $saved=Get-Content "$root\backup\restore.json" -Raw | ConvertFrom-Json
    if($saved.InstanceId -ne $instance){throw 'Recovery instance mismatch.'}
    $originalInf=$saved.OriginalInf
    $device=Target
    if($device.InstanceId -ne $instance -or (Property $device DEVPKEY_Device_Service) -ne 'cmp170' -or
       (Property $device DEVPKEY_Device_ProblemCode) -ne 0){throw 'Experimental driver did not start normally.'}
    $pre=Ctl @('--diag') 'bound-diagnostic'
    if($pre.Code -ne 0 -or $pre.Text -notmatch 'protocol=6 build=00090001 .*state=0 .*mode=0'){throw 'Expected a fresh 0.9 protocol-6 device. No RUN performed.'}
    if($Mode -in 'Compute','ResetOnly') {
        if($pre.Text -notmatch 'RESET query=0x00000000 supported=0x([0-9A-Fa-f]+)' -or
           ([Convert]::ToUInt32($Matches[1],16) -band 1) -eq 0){throw 'Function-level reset unavailable. No payload executed.'}
    }
    if($Mode -in 'Compute','Memory') {
        if($pre.Text -notmatch '(?m)^00823804\s+[0-9A-F]{8}\s+FFFFFF8F\s*$' -or
           $pre.Text -notmatch '(?m)^001FA824\s+[0-9A-F]{8}\s+1FFFFE00\s*$' -or
           $pre.Text -notmatch '(?m)^001FA828\s+[0-9A-F]{8}\s+00000000\s*$') {
            throw 'FEAT/WPR baseline is unsuitable. Power cycle without EFI unlock; no RUN performed.'
        }
        if($Mode -eq 'Memory' -and (
           $pre.Text -notmatch 'device=20C2 ' -or
           $pre.Text -notmatch '(?m)^009A0204\s+[0-9A-F]{8}\s+02449000\s*$' -or
           $pre.Text -notmatch '(?m)^00100CE0\s+[0-9A-F]{8}\s+00000208\s*$')) {
            throw 'Memory requires a cold 20C2 8GiB baseline: CFG1=02449000 LMR=00000208. No RUN performed.'
        }
        $runStarted=$true;$canRestore=$false
        $operation='--'+$Mode.ToLowerInvariant()
        $result=Ctl @($operation,'--ack-experimental') ($Mode.ToLowerInvariant())
        if($Mode -ne 'Memory' -and $result.Text -match 'RUN checks=0x([0-9A-Fa-f]+)') {
            $checks=[Convert]::ToUInt32($Matches[1],16)
            $canRestore=(($checks -band 6) -eq 6) -and (($checks -band 16) -ne 0) -and (($checks -band 32) -eq 0)
        }
        # Even a failed RUN needs an independent, current readback.
        $post=Ctl @('--diag') 'post-cleanup-diagnostic'
        if($Mode -eq 'Memory') {
            # Never hand partial geometry to NVIDIA, even with quiescent DMA.
            foreach($report in $result,$post) {
                if($report.Code -ne 0 -or
                   $report.Text -notmatch 'state=1 status=0x00000000 stage=30 mode=2' -or
                   $report.Text -notmatch 'RUN checks=0x1F ' -or
                   $report.Text -notmatch 'latest DIAG status=0x00000000' -or
                   $report.Text -notmatch '(?m)^009A0204\s+[0-9A-F]{8}\s+02779000\s*$' -or
                   $report.Text -notmatch '(?m)^00100CE0\s+[0-9A-F]{8}\s+0000020B\s*$') {
                    throw 'Memory geometry/cleanup verification failed. No hot restore; save logs and fully power off.'
                }
            }
        }
        if($result.Code -ne 0 -or $post.Code -ne 0){throw 'Compute/cleanup verification failed; see immediate and independent readbacks.'}
    }
    if($Mode -eq 'Memory') {
        $handover=Ctl @('--memory-handover','--ack-experimental') 'memory-handover'
        if($handover.Code -ne 0 -or $handover.Text -notmatch 'MEMORY handover=1 no_flr=1 cached=2') {
            throw 'Memory handover verification failed; see memory-handover.txt.'
        }
        $canRestore=$true
    }
    if($Mode -in 'Compute','ResetOnly') {
        $runStarted=$true;$canRestore=$false
        $reset=Ctl @('--final-reset','--ack-experimental') 'final-reset'
        if($reset.Code -ne 0 -or $reset.Text -notmatch 'attempted=1 status=0x00000000 complete=1 cached=1') {
            throw 'Final function-level reset did not complete successfully. Fully power off; no hot restore.'
        }
        $canRestore=$true
        # Final reset ends all MMIO. No post-reset DIAG; verify with NVIDIA after rebind.
    }
} catch {
    $failure=$_.Exception.Message
    $failure | Set-Content "$out\failure.txt"
} finally {
    try {
        if($bound -and $canRestore) {
            if(!$originalInf) {
                $saved=Get-Content "$root\backup\restore.json" -Raw | ConvertFrom-Json
                if($saved.InstanceId -ne $instance){throw 'Recovery instance mismatch.'}
                $originalInf=$saved.OriginalInf
            }
            $restore=Ctl @('--bind',$originalInf,'--ack-device-rebind') 'restore'
            if($restore.Code -eq 3010){throw 'Restore requires a restart; no further hardware work this boot.'}
            if($restore.Code -ne 0){throw "Restore API failed: $($restore.Code)"}
            $healthy=$false
            for($i=0;$i -lt 30;$i++) {
                $device=Target
                if($device.InstanceId -ne $instance){throw 'Target instance changed.'}
                $service=Property $device DEVPKEY_Device_Service
                $problem=Property $device DEVPKEY_Device_ProblemCode
                $inf=Property $device DEVPKEY_Device_DriverInfPath
                "$(Get-Date -Format o) service=$service problem=$problem inf=$inf" | Add-Content "$out\restore-poll.txt"
                if($service -eq 'nvlddmkm' -and $problem -eq 0){$healthy=$true;break}
                if($problem -eq 43){break}
                Start-Sleep -Seconds 1
            }
            & nvidia-smi.exe -q | Set-Content "$out\restored-nvidia.txt"
            if(!$healthy -or $LASTEXITCODE){throw 'NVIDIA did not recover normally. Save evidence and fully power off.'}
            if($Mode -eq 'Memory' -and !$failure) {
                $rows=@(& nvidia-smi.exe -i $memoryUuid '--query-gpu=uuid,pci.bus_id,memory.total' '--format=csv,noheader,nounits')
                if($LASTEXITCODE){throw 'Post-reset memory capacity query failed.'}
                $rows | Set-Content "$out\memory-restored.csv"
                $cards=@($rows | ConvertFrom-Csv -Header uuid,bus,mib)
                $expandedMiB=0
                if($cards.Count -ne 1 -or $cards[0].uuid.Trim() -ne $memoryUuid -or
                   $cards[0].bus.Trim() -ne $memoryBus -or
                   ![int]::TryParse($cards[0].mib.Trim(),[ref]$expandedMiB)) {throw 'Post-reset target/capacity is ambiguous.'}
                if($expandedMiB -lt 64512 -or $expandedMiB -gt 65536) {
                    throw "NVIDIA is healthy but reports $expandedMiB MiB (baseline $stockMiB). 64GiB geometry was NOT accepted. No hot retry."
                }
                Write-Host "CAPACITY: NVIDIA reports $stockMiB -> $expandedMiB MiB on $memoryBus. CUDA address verification is still required."
            }
        } elseif($bound -and $runStarted) {
            throw 'Engine/DMA safety or complete memory geometry was not confirmed. Hot Restore skipped. Fully power off; after boot use manage.ps1 -Action Restore.'
        }
    } catch {
        $failure=(@($failure,$_.Exception.Message) | Where-Object {$_}) -join ' | '
        $failure | Set-Content "$out\failure.txt"
    }
    try {Save-CmpDiagnostics "$out\after"} catch {$_ | Out-String | Set-Content "$out\collection-error.txt"}
    if($failure){Write-Host "FAILED: $failure"}else{Write-Host 'PASS: diagnostic/cleanup and NVIDIA communication checks. CUDA correctness has not been tested.'}
    Write-Host "EVIDENCE: $out"
    Stop-Transcript | Out-Null
}
if($failure){throw $failure}

