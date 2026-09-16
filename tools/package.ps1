$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$stamp=Get-Date -Format yyyyMMdd-HHmmss
$stage="$root\build\package-$stamp\cmp170hx-windows-driver"
New-Item -ItemType Directory -Force $stage,"$stage\vendor" | Out-Null
foreach($name in 'src','tools','tests','dist','analysis','research','cert','README.md','ANALYSIS.md','TEST-RESULTS.md','TEST-RESULTS-0.1.md','TEST-RESULTS-0.4.md','TEST-RESULTS-0.9.md','WPR-UNLOAD-0.4.md','WPR-PLM-0.5.md','HANDOVER-RESET-0.6.md','MEMORY-0.7.md','MEMORY-0.8.md','MEMORY-0.9.md','LICENSE','build.ps1','test.ps1','manage.ps1') {
    Copy-Item -LiteralPath "$root\$name" -Destination $stage -Recurse
}
Get-ChildItem "$root\vendor" -File | Where-Object Extension -In '.bin','.json' | Copy-Item -Destination "$stage\vendor"
Copy-Item "$root\vendor\python" "$stage\vendor\python" -Recurse
$zip="$root\cmp170hx-windows-driver-0.9-$stamp.zip"
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal
Get-FileHash -LiteralPath $zip -Algorithm SHA256



