#Requires -RunAsAdministrator
# Copies protected evidence into the user workspace. No GPU writes or driver changes.
param()
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$out=Join-Path "$root\logs" ('export-'+(Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Force $out | Out-Null
Start-Transcript -LiteralPath "$out\export.txt" | Out-Null
try {
    foreach($entry in @(
        @{Source="$env:ProgramData\CMP170HX\logs";Name='autostart-logs'},
        @{Source="$env:ProgramData\CMP170HX\blocked.json";Name='blocked.json'},
        @{Source="$env:ProgramData\CMP170HX\last-boot.txt";Name='last-boot.txt'},
        @{Source="$env:SystemRoot\Temp\cmp170-kernel.log";Name='cmp170-kernel.log'},
        @{Source="$env:ProgramFiles\CMP170HX\tools\autostart.ps1";Name='installed-autostart.ps1'},
        @{Source="$env:ProgramFiles\CMP170HX\backup\restore.json";Name='restore.json'}
    )) {
        if(Test-Path -LiteralPath $entry.Source) {
            Copy-Item -LiteralPath $entry.Source -Destination (Join-Path $out $entry.Name) -Recurse -ErrorAction Stop
            Write-Host "Copied $($entry.Source)"
        } else {Write-Host "Missing: $($entry.Source)"}
    }
    $task=Get-ScheduledTask -TaskName 'CMP170HX-AutoUnlock' -ErrorAction SilentlyContinue
    if($task) {
        Export-ScheduledTask -TaskName 'CMP170HX-AutoUnlock' | Set-Content "$out\task.xml" -Encoding UTF8
        Get-ScheduledTaskInfo -TaskName 'CMP170HX-AutoUnlock' | Format-List * | Out-String | Set-Content "$out\task-status.txt"
    } else {Set-Content "$out\task-status.txt" 'Task absent (queried elevated).'}
    . "$PSScriptRoot\diagnostics.ps1"
    Save-CmpDiagnostics "$out\current"
    Get-ChildItem "$env:ProgramFiles\CMP170HX\dist" -File | Get-FileHash -Algorithm SHA256 |
        Format-Table -AutoSize | Out-String -Width 300 | Set-Content "$out\installed-hashes.txt"
    Write-Host "EXPORTED: $out"
    Write-Host 'Return to Codex after this completes. No need to upload or paste the log contents.'
} finally {Stop-Transcript | Out-Null}
