#Requires -RunAsAdministrator
param([ValidateSet('Switch','ResetOnly','Compute','Memory','MemoryGen2')][string]$Mode='Switch')
$gen2=$Mode -eq 'MemoryGen2'
if($gen2){$Mode='Memory'}
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=Split-Path $PSScriptRoot
. "$PSScriptRoot\diagnostics.ps1"
. "$PSScriptRoot\common.ps1"
$out=Join-Path "$root\logs" ('cleanup-0.11-'+$(if($gen2){'MemoryGen2'}else{$Mode})+'-'+(Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Path $out | Out-Null
$ctl="$root\dist\cmpctl.exe"
$bound=$false;$runStarted=$false;$canRestore=$true;$failure=$null;$originalInf=$null
$devices=@();$instances=@();$targets=@()
function Targets {
    $items=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Match '^PCI\\VEN_10DE&DEV_(20C2|2082)&')
    if($items.Count -lt 1){throw 'No present CMP GPU.'}
    return $items
}
function Property($device,[string]$key) {
    (Get-PnpDeviceProperty -InstanceId $device.InstanceId -KeyName $key -ErrorAction Stop).Data
}
# nvidia-smi prints a full eight-digit PCI domain; PnP exposes bus/device/function separately.
function BusToken($device) {
    $bus=[int](Property $device DEVPKEY_Device_BusNumber)
    $address=[int](Property $device DEVPKEY_Device_Address)
    return ('{0:X2}:{1:X2}.{2}' -f $bus,(($address -shr 16) -band 0xFF),($address -band 0x07))
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
    $devices=@(Targets)
    $instances=@($devices | ForEach-Object {$_.InstanceId})
    foreach($device in $devices) {
        if((Property $device DEVPKEY_Device_Service) -ne 'nvlddmkm' -or
           (Property $device DEVPKEY_Device_ProblemCode) -ne 0){throw "Start from healthy NVIDIA after a full power cycle, without EFI unlock. Not healthy: $($device.InstanceId)"}
    }
    Write-Host "TARGETS: $($devices.Count) present CMP device(s)."
    & nvidia-smi.exe -q | Set-Content "$out\baseline-nvidia.txt"
    if($LASTEXITCODE){throw 'NVIDIA-SMI baseline failed.'}
    if($Mode -eq 'Memory') {
        $rows=@(& nvidia-smi.exe '--query-gpu=uuid,pci.device_id,pci.bus_id,memory.total' '--format=csv,noheader,nounits')
        if($LASTEXITCODE){throw 'Cannot identify the NVIDIA memory target.'}
        $rows | Set-Content "$out\memory-baseline.csv"
        $cards=@($rows | ConvertFrom-Csv -Header uuid,id,bus,mib)
        foreach($device in $devices) {
            $token=BusToken $device
            $match=@($cards | Where-Object {$_.bus.Trim() -match ('(?i)'+[regex]::Escape($token)+'$')})
            if($match.Count -ne 1){throw "Cannot match $($device.InstanceId) to exactly one NVIDIA row (bus $token)."}
            if($match[0].id.Trim() -ne '0x20C210DE'){throw "Device $($device.InstanceId) is not an NVIDIA 20C2 target."}
            $stockMiB=0
            if(![int]::TryParse($match[0].mib.Trim(),[ref]$stockMiB) -or $stockMiB -lt 7000 -or $stockMiB -gt 8192){throw "NVIDIA baseline on bus $token is not stock 8GiB. Fully power cycle first."}
            $targets+=[pscustomobject]@{Instance=$device.InstanceId;Bus=$match[0].bus.Trim();Uuid=$match[0].uuid.Trim();StockMiB=$stockMiB}
        }
    }
    $boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    $boot | Set-Content "$out\boot.txt"
    Save-CmpDiagnostics "$out\before"
    $bound=$true # Bind can fail after changing PnP; recovery must still be attempted.
    & "$root\manage.ps1" -Action Bind
    $saved=Get-Content "$root\backup\restore.json" -Raw | ConvertFrom-Json
    if(!(Test-CmpSameSet @(Get-CmpRecoveryInstances $saved) $instances)){throw 'Recovery instance set mismatch.'}
    $originalInf=$saved.OriginalInf
    $devices=@(Targets)
    foreach($device in $devices) {
        if((Property $device DEVPKEY_Device_Service) -ne 'cmp170' -or
           (Property $device DEVPKEY_Device_ProblemCode) -ne 0){throw "Experimental driver did not start normally on $($device.InstanceId)."}
    }
    $pre=Ctl @('--diag') 'bound-diagnostic'
    if($pre.Code -ne 0){throw 'Bound diagnostic failed.'}
    $preBlocks=@(Assert-CmpCtlBlock $pre.Text $devices.Count 'bound diagnostic')
    $preSet=@($preBlocks | ForEach-Object {$_.Instance})
    if(!(Test-CmpSameSet $preSet $instances)){throw "Bound diagnostic device set does not match the PnP target set (reported: $($preSet -join '; '); PnP: $($instances -join '; ')). No RUN performed."}
    foreach($block in $preBlocks) { Assert-CmpPreRun $block $Mode }
    if($Mode -in 'Compute','Memory','MemoryGen2') {
        $runStarted=$true;$canRestore=$false
        $operation=if($gen2){'--memory-gen2'}else{'--'+$Mode.ToLowerInvariant()}
        $result=Ctl @($operation,'--ack-experimental') ($Mode.ToLowerInvariant())
        $resultBlocks=@(Assert-CmpCtlBlock $result.Text $devices.Count $Mode.ToLowerInvariant())
        if($Mode -ne 'Memory') {
            # Hot hand-back needs quiescent engines and released, not retained, DMA on every card.
            $canRestore=$true
            foreach($block in $resultBlocks) {
                if($block.Text -notmatch 'RUN checks=0x([0-9A-Fa-f]+) '){$canRestore=$false;break}
                $checks=[Convert]::ToUInt32($Matches[1],16)
                if(!((($checks -band 6) -eq 6) -and (($checks -band 16) -ne 0) -and (($checks -band 32) -eq 0))){$canRestore=$false;break}
            }
        }
        # Even a failed RUN needs an independent, current readback.
        $post=Ctl @('--diag') 'post-cleanup-diagnostic'
        $postBlocks=@(Assert-CmpCtlBlock $post.Text $devices.Count 'post-cleanup diagnostic')
        if($Mode -eq 'Memory') {
            # Never hand partial geometry to NVIDIA, even with quiescent DMA.
            foreach($stage in @(@{Name='run';Report=$result;Blocks=$resultBlocks},@{Name='post-diag';Report=$post;Blocks=$postBlocks})) {
                foreach($block in $stage.Blocks) {
                    if($gen2 -and $block.Text -notmatch 'PCIE requested=1 configured=1 '){throw "Gen2 configuration was not verified on $($block.Instance). No hot restore."}
                    if($stage.Report.Code -ne 0 -or
                       $block.Text -notmatch 'state=1 status=0x00000000 stage=30 mode=2' -or
                       $block.Text -notmatch 'RUN checks=0x1F ' -or
                       $block.Text -notmatch 'latest DIAG status=0x00000000' -or
                       $block.Text -notmatch '(?m)^009A0204\s+[0-9A-F]{8}\s+02779000\s*$' -or
                       $block.Text -notmatch '(?m)^00100CE0\s+[0-9A-F]{8}\s+0000020B\s*$') {
                        throw "Memory geometry/cleanup verification failed on $($block.Instance). No hot restore; save logs and fully power off."
                    }
                }
            }
        }
        if($result.Code -ne 0 -or $post.Code -ne 0){throw 'Compute/cleanup verification failed; see immediate and independent readbacks.'}
    }
    if($Mode -eq 'Memory') {
        $handover=Ctl @('--memory-handover','--ack-experimental') 'memory-handover'
        if($handover.Code -ne 0){throw 'Memory handover failed; see memory-handover.txt.'}
        foreach($block in @(Assert-CmpCtlBlock $handover.Text $devices.Count 'memory-handover')) {
            if($block.Text -notmatch 'MEMORY handover=1 no_flr=1 cached=2'){throw "Memory handover verification failed on $($block.Instance)."}
        }
        $canRestore=$true
    }
    if($Mode -in 'Compute','ResetOnly') {
        $runStarted=$true;$canRestore=$false
        $reset=Ctl @('--final-reset','--ack-experimental') 'final-reset'
        if($reset.Code -ne 0){throw 'Final function-level reset did not complete successfully. Fully power off; no hot restore.'}
        foreach($block in @(Assert-CmpCtlBlock $reset.Text $devices.Count 'final-reset')) {
            if($block.Text -notmatch 'attempted=1 status=0x00000000 complete=1 cached=1'){throw "Final reset did not complete on $($block.Instance). Fully power off; no hot restore."}
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
                if(!(Test-CmpSameSet @(Get-CmpRecoveryInstances $saved) $instances)){throw 'Recovery instance mismatch.'}
                $originalInf=$saved.OriginalInf
            }
            $restore=Ctl @('--bind',$originalInf,'--ack-device-rebind') 'restore'
            if($restore.Code -eq 3010){throw 'Restore requires a restart; no further hardware work this boot.'}
            if($restore.Code -ne 0){throw "Restore API failed: $($restore.Code)"}
            $healthy=$false
            for($i=0;$i -lt 30;$i++) {
                try {$current=@(Targets)} catch {$current=@()} # a rebind restarts devices; tolerate a transiently incomplete enumeration
                if(!(Test-CmpSameSet @($current | ForEach-Object {$_.InstanceId}) $instances)){Start-Sleep -Seconds 1;continue}
                $pending=$false;$degraded=$false
                foreach($device in $current) {
                    $service=Property $device DEVPKEY_Device_Service
                    $problem=Property $device DEVPKEY_Device_ProblemCode
                    $inf=Property $device DEVPKEY_Device_DriverInfPath
                    "$(Get-Date -Format o) $($device.InstanceId) service=$service problem=$problem inf=$inf" | Add-Content "$out\restore-poll.txt"
                    if($service -ne 'nvlddmkm' -or $problem -ne 0){$pending=$true}
                    if($problem -eq 43){$degraded=$true}
                }
                if(!$pending){$healthy=$true;break}
                if($degraded){break}
                Start-Sleep -Seconds 1
            }
            & nvidia-smi.exe -q | Set-Content "$out\restored-nvidia.txt"
            if(!$healthy -or $LASTEXITCODE){throw 'NVIDIA did not recover normally. Save evidence and fully power off.'}
            if($Mode -eq 'Memory' -and !$failure) {
                $rows=@(& nvidia-smi.exe '--query-gpu=uuid,pci.bus_id,memory.total' '--format=csv,noheader,nounits')
                if($LASTEXITCODE){throw 'Post-reset memory capacity query failed.'}
                $rows | Set-Content "$out\memory-restored.csv"
                $cards=@($rows | ConvertFrom-Csv -Header uuid,bus,mib)
                foreach($target in $targets) {
                    $match=@($cards | Where-Object {$_.uuid.Trim() -eq $target.Uuid})
                    $expandedMiB=0
                    if($match.Count -ne 1 -or $match[0].bus.Trim() -ne $target.Bus -or
                       ![int]::TryParse($match[0].mib.Trim(),[ref]$expandedMiB)){throw "Post-reset target/capacity is ambiguous for $($target.Bus)."}
                    if($expandedMiB -lt 64512 -or $expandedMiB -gt 65536) {
                        throw "NVIDIA is healthy on $($target.Bus) but reports $expandedMiB MiB (baseline $($target.StockMiB)). 64GiB geometry was NOT accepted. No hot retry."
                    }
                }
                if($gen2) {
                    foreach($target in $targets) {
                        $gen2Seen=$false
                        for($sample=0;$sample -lt 10;$sample++) {
                            $speedRows=@(& nvidia-smi.exe -i $target.Uuid '--query-gpu=uuid,pcie.link.gen.current,pcie.link.gen.max,pcie.link.width.current' '--format=csv,noheader,nounits')
                            if($LASTEXITCODE){throw 'Gen2 link query failed.'}
                            $speedRows | Add-Content "$out\pcie-restored.csv"
                            $speed=@($speedRows | ConvertFrom-Csv -Header uuid,current,max,width | Where-Object {$_.uuid.Trim() -eq $target.Uuid})
                            if($speed.Count -ne 1){throw 'Gen2 target mismatch.'}
                            if($speed[0].current.Trim() -eq '2' -and [int]$speed[0].width.Trim() -gt 0){$gen2Seen=$true;break}
                            Start-Sleep -Seconds 1
                        }
                        if(!$gen2Seen){throw "GPU configuration applied on $($target.Bus), but negotiated Gen2 NOT observed after NVIDIA handover. NVIDIA restored; inspect pcie-restored.csv and test under load. No hot retry."}
                    }
                    Write-Host 'PCIE: negotiated Gen2 observed on every restored target; verify transfer bandwidth under load separately.'
                }
                Write-Host "CAPACITY: $($targets.Count) card(s) reported 64GiB geometry. CUDA address verification is still required."
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
