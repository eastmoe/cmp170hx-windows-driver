$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot
$source=Get-Content "$project\tools\resume-pcie.ps1" -Raw
$tokens=$null;$errors=$null
[void][Management.Automation.Language.Parser]::ParseInput($source,[ref]$tokens,[ref]$errors)
if($errors){throw ($errors | Out-String)}
$body=[scriptblock]::Create($source.Substring($source.IndexOf('$success=$false')).Replace('exit 1',"throw 'test failure'"))
function Start-Sleep {}
function Get-ScheduledTask {}
function Get-CimInstance {[pscustomobject]@{LastBootUpTime=[datetime]'2026-09-16T00:00:00Z'}}
function Device {[pscustomobject]@{InstanceId='test-gpu'}}
function Prop($d,$key) {if($key -like '*Service'){if($script:restored){'nvlddmkm'}else{'cmp170'}}else{0}}
function Ctl($Arguments,$Name) {
 $script:calls.Add($Name)
 $code=0
 $text=switch($Name){
 'old-driver-live' {$code=7;if($scenario -eq 'bad-old'){'wrong'}else{'protocol=7 build=000A0001 device=20C2 state=2 status=0xC0000483 stage=21 mode=2 RUN checks=0x16 DMA_released=1 DMA_retained=0 RUN cleanup_attempted=1 mismatch_mask=0x0 '}}
 'fixed-driver-live' {'protocol=7 build=000A0003 device=20C2 state=0 '}
 'handover' {'MEMORY handover=1 no_flr=1 cached=2'}
 'restore' {$script:restored=$true;''}
 'bind-fixed' {''}
 default {if($scenario -eq 'bad-resume'){$code=7;'failed'}else{'state=1 status=0x00000000 stage=30 mode=2 PCIE requested=2 configured=1 RUN checks=0x1F '}}
 }
 [pscustomobject]@{Code=$code;Text=$text}
}
function nvidia-smi.exe {
 $global:LASTEXITCODE=0
 if($args -contains '-q'){'test'}else{if($scenario -eq 'gen1'){'GPU-test, B3, 65535, 1, 2, 4'}else{'GPU-test, B3, 65535, 2, 2, 4'}}
}
foreach($scenario in 'success','bad-old','bad-resume','gen1') {
 $root=Join-Path ([IO.Path]::GetTempPath()) ('resume-test-'+[guid]::NewGuid())
 $out="$root\logs";$Evidence="$root\evidence"
 New-Item -ItemType Directory "$root\dist","$root\backup",$out,$Evidence | Out-Null
 Set-Content "$root\dist\dummy" 'test'
 "$((Get-FileHash "$root\dist\dummy").Hash)  dummy" | Set-Content "$root\dist\SHA256SUMS.txt"
 Set-Content "$Evidence\boot.txt" ((Get-CimInstance).LastBootUpTime.ToUniversalTime().ToString('o'))
 Set-Content "$Evidence\memory-baseline.csv" 'GPU-test, 0x20C210DE, B3, 8191'
 Set-Content "$root\original.inf" 'test'
 @{InstanceId='test-gpu';OriginalInf="$root\original.inf"} | ConvertTo-Json | Set-Content "$root\backup\restore.json"
 $script:calls=[Collections.Generic.List[string]]::new();$script:restored=$false;$threw=$false
 try{& $body}catch{$threw=$true}
 if($threw -ne ($scenario -ne 'success')){throw "Wrong result: $scenario"}
 $expected=if($scenario -eq 'bad-old'){'old-driver-live'}elseif($scenario -eq 'bad-resume'){'old-driver-live,bind-fixed,fixed-driver-live,resume,post-resume'}else{'old-driver-live,bind-fixed,fixed-driver-live,resume,post-resume,handover,restore'}
 if(($script:calls -join ',') -ne $expected){throw "Wrong calls: $scenario $script:calls"}
 Write-Host "PASS resume runner $scenario"
}
