param([Parameter(Mandatory)][ValidateSet('Install','Run','Disable')][string]$Action)
$ErrorActionPreference='Stop'
$taskName='CMP170HX-AutoUnlock'
$runtime=Join-Path $env:ProgramFiles 'CMP170HX'
$data=Join-Path $env:ProgramData 'CMP170HX'
$source=Split-Path $PSScriptRoot
. "$PSScriptRoot\diagnostics.ps1"
. "$PSScriptRoot\common.ps1"
function Invoke-Cmp([string]$File,[string[]]$Arguments) {
    Write-Host "$(Get-Date -Format o) EXEC $File $($Arguments -join ' ')"
    & $File @Arguments
    $code=$LASTEXITCODE
    Write-Host "$(Get-Date -Format o) EXIT $code"
    if($code -eq 3010){throw 'Reboot required by PnP; automatic operation stopped. See diagnostics and Restore.'}
    if($code){throw "$File failed with exit code $code"}
}
function Targets {
    $found=@(Get-PnpDevice -PresentOnly | Where-Object InstanceId -Match '^PCI\\VEN_10DE&DEV_(20C2|2082)&')
    if($found.Count -lt 1){throw 'No present CMP GPU.'}
    return $found
}
function Service($Device) { (Get-PnpDeviceProperty -InstanceId $Device.InstanceId -KeyName DEVPKEY_Device_Service).Data }
if(!([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Administrator PowerShell required.'}
if($Action -eq 'Disable') {
    $task=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if($task){Disable-ScheduledTask -TaskName $taskName | Out-Null}
    Write-Host 'Automatic unlock disabled. Recovery files and logs retained.'
    return
}
if($Action -eq 'Install') {
    $old=Get-ScheduledTask -TaskName $taskName -ErrorAction SilentlyContinue
    if($old -and $old.State -eq 'Running'){throw 'Automatic unlock is running; wait for it to finish before installing.'}
    if($old){Disable-ScheduledTask -TaskName $taskName | Out-Null}
    if(Test-Path "$data\blocked.json"){throw 'Previous automatic attempt failed or was interrupted. Run Restore and inspect logs first; then remove blocked.json explicitly before reinstalling.'}
    foreach($name in 'cmp170.sys','cmpctl.exe','cmp170.cat','cmp170.inf') {
        if(!(Test-Path "$source\dist\$name")){throw "Missing $name; run tools\sign.ps1 first."}
    }
    foreach($name in 'cmp170.sys','cmpctl.exe','cmp170.cat') {
        $sig=Get-AuthenticodeSignature "$source\dist\$name"
        if($sig.Status -ne 'Valid'){throw "Signature not trusted/valid for $name ($($sig.Status)); sign and run PrepareTestSigning first."}
    }
    $devices=@(Targets)
    $serviceValues=@($devices | ForEach-Object {Service $_} | Sort-Object -Unique)
    if($serviceValues.Count -ne 1){throw "Mixed driver services across CMP targets: $($serviceValues -join ', ')"}
    $service=$serviceValues[0]
    foreach($directory in $runtime,$data) {
        New-Item -ItemType Directory -Force $directory | Out-Null
        # SYSTEM executes these files. Do not inherit writable user permissions.
        Invoke-Cmp icacls.exe @($directory,'/inheritance:r','/grant:r','*S-1-5-18:(OI)(CI)F','*S-1-5-32-544:(OI)(CI)F')
    }
    New-Item -ItemType Directory -Force "$runtime\dist","$runtime\tools","$runtime\backup" | Out-Null
    if($service -eq 'nvlddmkm') {
        $published=@($devices | ForEach-Object {(Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverInfPath).Data} | Sort-Object -Unique)
        if($published.Count -ne 1 -or $published[0] -notmatch '^oem\d+\.inf$'){throw "Expected one published NVIDIA INF across all CMP targets; found: $($published -join ', ')"}
        $backup="$runtime\backup\$(Get-Date -Format yyyyMMdd-HHmmss-fff)"
        New-Item -ItemType Directory -Force $backup | Out-Null
        Invoke-Cmp pnputil.exe @('/export-driver',$published[0],$backup)
        $original=@(Get-ChildItem $backup -Recurse -Filter '*.inf')
        if($original.Count -ne 1){throw 'Expected one exported NVIDIA INF'}
        $saved=@{InstanceIds=@($devices | ForEach-Object {$_.InstanceId});PublishedInf=$published[0];OriginalInf=$original[0].FullName}
    } elseif($service -eq 'cmp170') {
        $recovery="$source\backup\restore.json"
        if(!(Test-Path $recovery)){$recovery="$runtime\backup\restore.json"}
        $saved=Get-Content $recovery -Raw | ConvertFrom-Json
        $savedInstances=@(Get-CmpRecoveryInstances $saved)
        $current=@($devices | ForEach-Object {$_.InstanceId})
        if(!$savedInstances.Count -or !(Test-Path $saved.OriginalInf)){throw 'Missing or mismatched NVIDIA recovery backup'}
        # One exported NVIDIA package covers the whole model family, so the saved record must cover every target.
        foreach($id in $savedInstances){if($current -notcontains $id){throw "Saved recovery device no longer present: $id"}}
        $saved.InstanceIds=$current
        $backup="$runtime\backup\import-$(Get-Date -Format yyyyMMdd-HHmmss-fff)"
        Copy-Item -LiteralPath (Split-Path $saved.OriginalInf) -Destination $backup -Recurse
        $saved.OriginalInf=Join-Path $backup (Split-Path $saved.OriginalInf -Leaf)
    } else {throw "Unexpected service: $service"}
    $saved | ConvertTo-Json | Set-Content "$runtime\backup\restore.json"
    Copy-Item "$source\dist\*" "$runtime\dist" -Force
    Copy-Item "$source\tools\autostart.ps1","$source\tools\diagnostics.ps1","$source\tools\common.ps1" "$runtime\tools" -Force
    # Stage signed package now; the startup task performs the actual rebind.
    Invoke-Cmp pnputil.exe @('/add-driver',"$runtime\dist\cmp170.inf")
    $exe="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
    $command=New-ScheduledTaskAction -Execute $exe -Argument "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File `"$runtime\tools\autostart.ps1`" -Action Run" -WorkingDirectory $runtime
    $trigger=New-ScheduledTaskTrigger -AtStartup
    $trigger.Delay='PT30S'
    $principal=New-ScheduledTaskPrincipal -UserId 'SYSTEM' -LogonType ServiceAccount -RunLevel Highest
    $settings=New-ScheduledTaskSettingsSet -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 15) -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries
    Register-ScheduledTask -TaskName $taskName -Action $command -Trigger $trigger -Principal $principal -Settings $settings -Force | Out-Null
    Write-Host "Installed. On next Windows startup: memory unlock once, then NVIDIA restore on success. Logs: $data\logs"
    return
}
New-Item -ItemType Directory -Force "$data\logs" | Out-Null
$session=Join-Path "$data\logs" (Get-Date -Format yyyyMMdd-HHmmss-fff)
New-Item -ItemType Directory $session | Out-Null
Start-Transcript "$session\transcript.txt" | Out-Null
$failed=$false
try {
    Save-CmpDiagnostics "$session\before"
    if(Test-Path "$data\blocked.json"){Write-Host 'Paused after previous failure/interruption. See blocked.json.';return}
    $boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    if(Test-Path "$data\last-boot.txt") {
        if((Get-Content "$data\last-boot.txt" -Raw).Trim() -eq $boot){Write-Host 'Already attempted this Windows boot.';return}
    }
    $devices=@(Targets)
    $saved=Get-Content "$runtime\backup\restore.json" -Raw | ConvertFrom-Json
    $savedInstances=@(Get-CmpRecoveryInstances $saved)
    $current=@($devices | ForEach-Object {$_.InstanceId})
    if(!$savedInstances.Count -or !(Test-Path $saved.OriginalInf)){throw 'GPU/recovery mismatch'}
    foreach($id in $savedInstances){if($current -notcontains $id){throw "GPU/recovery mismatch: $id not present"}}
    foreach($device in $devices) {
        if((Service $device) -notin 'nvlddmkm','cmp170'){throw "Unexpected target service on $($device.InstanceId)"}
    }
    # Persist BEFORE rebind: a crash or forced termination leaves the next run paused.
    @{Boot=$boot;Log=$session;State='in-progress';Time=(Get-Date -Format o)} | ConvertTo-Json | Set-Content "$data\blocked.json"
    Set-Content "$data\last-boot.txt" $boot
    $ctl="$runtime\dist\cmpctl.exe"
    Invoke-Cmp $ctl @('--bind',"$runtime\dist\cmp170.inf",'--ack-device-rebind')
    Save-CmpDiagnostics "$session\bound"
    $devices=@(Targets)
    foreach($device in $devices) {
        if((Service $device) -ne 'cmp170' -or $device.Status -ne 'OK'){throw "Experimental driver did not start on $($device.InstanceId); see bound/device.txt and kernel/CodeIntegrity logs."}
    }
    Invoke-Cmp $ctl @('--diag')
    Invoke-Cmp $ctl @('--memory','--ack-experimental')
    Invoke-Cmp $ctl @('--diag')
    Invoke-Cmp $ctl @('--memory-handover','--ack-experimental')
    Save-CmpDiagnostics "$session\unlocked"
    Invoke-Cmp $ctl @('--bind',$saved.OriginalInf,'--ack-device-rebind')
    $devices=@(Targets)
    foreach($device in $devices) {
        if((Service $device) -ne 'nvlddmkm' -or $device.Status -ne 'OK'){throw "NVIDIA restore did not reach OK state on $($device.InstanceId)"}
    }
    Remove-Item -LiteralPath "$data\blocked.json"
    Write-Host 'Register operation succeeded and NVIDIA restored. Capacity/residency still needs validation; see nvidia log.'
} catch {
    $failed=$true
    $_ | Out-String | Set-Content "$session\failure.txt"
    @{Log=$session;State='failed';Error="$($_.Exception.Message)";Time=(Get-Date -Format o)} | ConvertTo-Json | Set-Content "$data\blocked.json"
    Write-Host "FAILED: $($_.Exception.Message). Automatic attempts paused."
} finally {
    Save-CmpDiagnostics "$session\after"
    Stop-Transcript | Out-Null
}
if($failed){exit 1}
