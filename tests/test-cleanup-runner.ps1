$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot
. "$project\tools\common.ps1"
$path="$project\tools\test-cleanup.ps1"
$tokens=$null;$errors=$null
[void][Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
if($errors){throw ($errors | Out-String)}
$source=Get-Content $path -Raw
$run=[scriptblock]::Create($source.Substring($source.IndexOf('Start-Transcript -LiteralPath')).Replace('$env:ProgramData','$testData'))
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('cmp-cleanup-tests-'+[guid]::NewGuid())
# Two fixed device identities, so cmpctl output, PnP reads and nvidia-smi rows all line up.
$instanceA='PCI\VEN_10DE&DEV_20C2&SUBSYS_158510DE&REV_A1\4&1DEB9AE&0&0000'
$instanceB='PCI\VEN_10DE&DEV_20C2&SUBSYS_158510DE&REV_A1\4&1DEB9AE&0&0001'
function Path($instance) { '\\?\'+($instance -replace '\\','#')+'#{df6f62f8-6382-4b1d-93c0-6ac1cc170801}' }
function Bus($instance) { if($instance -eq $instanceB){'B4:00.0'}else{'B3:00.0'} }
function Uuid($instance) { if($instance -eq $instanceB){'GPU-test-b'}else{'GPU-test-a'} }
# One cmpctl block per present device; $second overrides the body of every device after the first.
function Body([string]$text,[string]$second=$null) {
    $out=''
    for($i=0;$i -lt $script:instances.Count;$i++) {
        $out+="DEVICE index=$i count=$($script:instances.Count) path=$(Path $script:instances[$i])`n"
        $out+=if($second -and $i -gt 0){$second}else{$text}
        $out+="`n"
    }
    return $out
}
function Check-Package {}
function Save-CmpDiagnostics($Directory) {New-Item -ItemType Directory -Path $Directory -Force | Out-Null}
function Get-ScheduledTask {}
function Get-CimInstance {[pscustomobject]@{LastBootUpTime=[datetime]'2026-09-15T00:00:00Z'}}
function Targets {$script:instances | ForEach-Object {[pscustomobject]@{InstanceId=$_}}}
function BusToken($device) {Bus $device.InstanceId}
function Property($device,$key) {
    switch($key) {
        'DEVPKEY_Device_Service' {if(!$bound -or $script:restored){'nvlddmkm'}else{'cmp170'}}
        'DEVPKEY_Device_ProblemCode' {if($script:scenario -eq 'code43' -and $script:restored){43}else{0}}
        'DEVPKEY_Device_DriverInfPath' {'oem47.inf'}
    }
}
function Start-Sleep {}
function nvidia-smi.exe {
    $global:LASTEXITCODE=0
    $rows=@()
    foreach($instance in $script:instances) {
        $token=Bus $instance;$uuid=Uuid $instance
        if($args -contains '--query-gpu=uuid,pci.device_id,pci.bus_id,memory.total'){
            $rows+="$uuid, 0x20C210DE, 00000000:$token, 8191"
        }elseif($args -contains '--query-gpu=uuid,pci.bus_id,memory.total'){
            if($script:scenario -eq 'memory-noexpand'){$rows+="$uuid, 00000000:$token, 8191"}
            elseif($script:scenario -eq 'memory-wrong-gpu'){$rows+="GPU-other-$uuid, 00000000:$token, 65535"}
            else{$rows+="$uuid, 00000000:$token, 65535"}
        }elseif($args -contains '--query-gpu=uuid,pcie.link.gen.current,pcie.link.gen.max,pcie.link.width.current'){
            if($script:scenario -eq 'memory-gen2-slow'){$rows+="$uuid, 1, 2, 4"}else{$rows+="$uuid, 2, 2, 4"}
        }
    }
    if(!$rows){'mock NVIDIA'}else{$rows}
}
function Ctl([string[]]$Arguments,[string]$Name) {
    $script:calls.Add($Arguments[0])
    if($Arguments[0] -eq '--bind'){$script:restored=$true;return [pscustomobject]@{Code=0;Text=''} }
    if($Arguments[0] -eq '--memory-handover') {
        if($script:scenario -eq 'memory-handover-failed'){return [pscustomobject]@{Code=7;Text=(Body "MEMORY handover=0 no_flr=1 cached=0`n")}}
        return [pscustomobject]@{Code=0;Text=(Body "MEMORY handover=1 no_flr=1 cached=2`n")}
    }
    if($Arguments[0] -eq '--final-reset') {
        if($script:scenario -eq 'reset-failed'){return [pscustomobject]@{Code=7;Text=(Body "attempted=1 status=0xC0000001 complete=0 cached=1`n")}}
        return [pscustomobject]@{Code=0;Text=(Body "attempted=1 status=0x00000000 complete=1 cached=1`n")}
    }
    if($Name -eq 'bound-diagnostic') {
        $cfg=if($script:scenario -eq 'memory-baseline'){'02779000'}else{'02449000'}
        $support=if($script:scenario -in 'unsupported','memory-no-reset-interface'){'00000002'}else{'00000001'}
        return [pscustomobject]@{Code=0;Text=(Body "protocol=7 build=000A0003 device=20C2 state=0 status=0x00000000 stage=0 mode=0`nRESET query=0x00000000 supported=0x$support`n00823804 00000000 FFFFFF8F`n001FA824 00000000 1FFFFE00`n001FA828 00000000 00000000`n009A0204 00000000 $cfg`n00100CE0 00000000 00000208`n")}
    }
    $pcie=if($script:scenario -eq 'memory-gen2-config-fail'){'PCIE requested=1 configured=0 '}else{'PCIE requested=1 configured=1 '}
    $code=0;$flags='1F'
    if($script:scenario -eq 'cleanup-failed'){$code=7;$flags='17'}
    if($script:scenario -eq 'dma-unsafe'){$code=7;$flags='23'}
    # multi-unsafe: card 2 keeps DMA retained (0x27) while card 1 looks clean; one unclean card blocks the whole set.
    if($script:scenario -eq 'multi-unsafe'){$code=7}
    if($Mode -eq 'Memory') {
        if($script:scenario -eq 'memory-partial'){$code=7}
        $lmr=if($script:scenario -eq 'memory-readback'){'00000208'}else{'0000020B'}
        return [pscustomobject]@{Code=$code;Text=(Body "$pcie`nstate=1 status=0x00000000 stage=30 mode=2`nRUN checks=0x$flags target=1`nlatest DIAG status=0x00000000`n009A0204 02449000 02779000`n00100CE0 00000208 $lmr`n")}
    }
    # multi-unsafe: the second card keeps DMA retained (0x27), so the whole set must stay with this driver.
    $second=if($script:scenario -eq 'multi-unsafe'){"RUN checks=0x27 target=1"}else{$null}
    [pscustomobject]@{Code=$code;Text=(Body "RUN checks=0x$flags target=1" $second)}
}
foreach($script:scenario in 'memory-gen2-success','memory-gen2-slow','memory-gen2-config-fail','switch','success','cleanup-failed','dma-unsafe','code43','memory-stale-marker','reset-only','reset-failed','unsupported','memory-success','memory-baseline','memory-partial','memory-readback','memory-noexpand','memory-wrong-gpu','memory-handover-failed','memory-no-reset-interface','multi-success','multi-unsafe') {
    $root=Join-Path $testRoot $script:scenario;$out="$root\logs";$testData="$root\data"
    New-Item -ItemType Directory -Path "$root\backup",$out,"$testData\CMP170HX" -Force | Out-Null
    # Substitute Bind with an inert script that only supplies a recovery record.
    Set-Content "$root\manage.ps1" 'param($Action)'
    $script:instances=@(if($script:scenario -like 'multi-*'){$instanceA;$instanceB}else{$instanceA})
    if($script:scenario -eq 'success') {
        # Older single-device recovery record: the reader must still accept it.
        @{InstanceId=$instanceA;OriginalInf="$root\original.inf"} | ConvertTo-Json | Set-Content "$root\backup\restore.json"
    } else {
        @{InstanceIds=$script:instances;OriginalInf="$root\original.inf"} | ConvertTo-Json | Set-Content "$root\backup\restore.json"
    }
    $Mode=if($script:scenario -eq 'switch'){'Switch'}elseif($script:scenario -eq 'reset-only'){'ResetOnly'}elseif($script:scenario -like 'memory-*'){'Memory'}else{'Compute'}
    $gen2=$script:scenario -like 'memory-gen2-*'
    $bound=$false;$runStarted=$false;$canRestore=$true;$failure=$null;$originalInf=$null
    $script:restored=$false;$script:calls=[Collections.Generic.List[string]]::new()
    if($script:scenario -eq 'memory-stale-marker') {
        Set-Content "$testData\CMP170HX\research-last-compute-boot.txt" ((Get-CimInstance).LastBootUpTime.ToUniversalTime().ToString('o'))
    }
    $threw=$false
    try {& $run} catch {$threw=$true}
    $expected=switch($script:scenario){'memory-gen2-success' {'--diag,--memory-gen2,--diag,--memory-handover,--bind'} 'memory-gen2-slow' {'--diag,--memory-gen2,--diag,--memory-handover,--bind'} 'memory-gen2-config-fail' {'--diag,--memory-gen2,--diag'} 'memory-handover-failed' {'--diag,--memory,--diag,--memory-handover'} 'memory-no-reset-interface' {'--diag,--memory,--diag,--memory-handover,--bind'} 'memory-noexpand' {'--diag,--memory,--diag,--memory-handover,--bind'} 'memory-wrong-gpu' {'--diag,--memory,--diag,--memory-handover,--bind'} 'memory-success' {'--diag,--memory,--diag,--memory-handover,--bind'} 'memory-baseline' {'--diag,--bind'} 'memory-partial' {'--diag,--memory,--diag'} 'memory-readback' {'--diag,--memory,--diag'} 'switch' {'--diag,--bind'} 'memory-stale-marker' {'--diag,--memory,--diag,--memory-handover,--bind'} 'dma-unsafe' {'--diag,--compute,--diag'} 'multi-unsafe' {'--diag,--compute,--diag'} 'reset-failed' {'--diag,--compute,--diag,--final-reset'} 'unsupported' {'--diag,--bind'} 'cleanup-failed' {'--diag,--compute,--diag,--bind'} 'reset-only' {'--diag,--final-reset,--bind'} default {'--diag,--compute,--diag,--final-reset,--bind'}}
    if(($script:calls -join ',') -ne $expected){throw "$script:scenario unexpected calls: $script:calls"}
    if($threw -ne ($script:scenario -notin 'memory-gen2-success','switch','success','reset-only','memory-success','memory-stale-marker','memory-no-reset-interface','multi-success')){throw "$script:scenario incorrect final result"}
    Write-Host "PASS cleanup runner $script:scenario"
}
Write-Host "Runner mock evidence: $testRoot"
