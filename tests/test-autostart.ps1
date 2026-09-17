$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot
. "$project\tools\common.ps1"
foreach($path in @("$project\manage.ps1","$project\tools\autostart.ps1","$project\tools\diagnostics.ps1","$project\tools\common.ps1")) {
    $tokens=$null;$errors=$null
    [void][Management.Automation.Language.Parser]::ParseFile($path,[ref]$tokens,[ref]$errors)
    if($errors){throw ($errors | Out-String)}
}
# Execute the actual Run branch with all hardware/OS readers substituted.
# Files are confined to a fresh test directory; no scheduled task or GPU is touched.
$source=Get-Content "$project\tools\autostart.ps1" -Raw
$start=$source.IndexOf('New-Item -ItemType Directory -Force "$data\logs"')
if($start -lt 0){throw 'Run branch not found'}
$run=[scriptblock]::Create($source.Substring($start).Replace('if($failed){exit 1}','if($failed){return}'))
$testRoot=Join-Path ([IO.Path]::GetTempPath()) ('cmp170-tests-'+[guid]::NewGuid())
function Save-CmpDiagnostics([string]$Directory) {}
function Get-CimInstance { [pscustomobject]@{LastBootUpTime=[datetime]'2026-09-15T00:00:00Z'} }
function Targets { $script:deviceIds | ForEach-Object {[pscustomobject]@{InstanceId=$_;Status=$(if($script:scenario -eq 'device-failure'){'Error'}else{'OK'})}} }
function Service($Device) { if($script:calls.Count -eq 0 -or $script:calls.Count -eq 6){'nvlddmkm'}else{'cmp170'} }
function Invoke-Cmp([string]$File,[string[]]$Arguments) {
    $script:calls.Add(($Arguments -join ' '))
    if(($script:scenario -eq 'bind-failure' -and $script:calls.Count -eq 1) -or
       ($script:scenario -eq 'memory-failure' -and $Arguments[0] -eq '--memory') -or
       ($script:scenario -eq 'diagnostic-failure' -and $script:calls.Count -eq 4) -or
       ($script:scenario -eq 'handover-failure' -and $Arguments[0] -eq '--memory-handover') -or
       ($script:scenario -eq 'restore-failure' -and $script:calls.Count -eq 6)){throw 'Injected hardware failure'}
}
foreach($script:scenario in 'success','blocked','same-boot','bind-failure','device-failure','memory-failure','diagnostic-failure','handover-failure','restore-failure','legacy-recovery','multi') {
    $data=Join-Path $testRoot $script:scenario;$runtime=Join-Path $data 'runtime'
    New-Item -ItemType Directory -Force "$runtime\backup" | Out-Null
    Set-Content "$runtime\backup\original.inf" 'test'
    $script:deviceIds=@(if($script:scenario -eq 'multi'){'test-gpu';'test-gpu-2'}else{'test-gpu'})
    if($script:scenario -eq 'legacy-recovery') {
        # Older single-device recovery record: the reader must still accept it.
        @{InstanceId=$script:deviceIds[0];OriginalInf="$runtime\backup\original.inf"} | ConvertTo-Json | Set-Content "$runtime\backup\restore.json"
    } else {
        @{InstanceIds=$script:deviceIds;OriginalInf="$runtime\backup\original.inf"} | ConvertTo-Json | Set-Content "$runtime\backup\restore.json"
    }
    $script:calls=[Collections.Generic.List[string]]::new()
    if($script:scenario -eq 'blocked'){Set-Content "$data\blocked.json" '{"State":"in-progress"}'}
    if($script:scenario -eq 'same-boot'){Set-Content "$data\last-boot.txt" ((Get-CimInstance).LastBootUpTime.ToUniversalTime().ToString('o'))}
    & $run
    $expected=@{'success'=6;'blocked'=0;'same-boot'=0;'bind-failure'=1;'device-failure'=1;'memory-failure'=3;'diagnostic-failure'=4;'handover-failure'=5;'restore-failure'=6;'legacy-recovery'=6;'multi'=6}[$script:scenario]
    if($script:calls.Count -ne $expected){throw "$script:scenario expected $expected calls, got $($script:calls.Count)"}
    $blocked=Test-Path "$data\blocked.json"
    if($script:scenario -in 'success','same-boot','legacy-recovery','multi'){if($blocked){throw 'Unexpected failure latch'}}
    elseif(!$blocked){throw 'Missing durable failure latch'}
    if($script:scenario -eq 'success' -and $script:calls[2] -ne '--memory --ack-experimental'){throw 'Missing memory operation'}
    if($script:scenario -eq 'success' -and ($script:calls[3] -ne '--diag' -or $script:calls[4] -ne '--memory-handover --ack-experimental')){throw 'Missing independent diagnostic or sealed handover'}
    Write-Host "PASS autostart $script:scenario"
}
Write-Host "Autostart test logs: $testRoot"
