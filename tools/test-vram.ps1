param([switch]$Full,[ValidateRange(1,100)][int]$Passes=3)
$ErrorActionPreference='Stop'
$PSNativeCommandUseErrorActionPreference=$false
$root=Split-Path $PSScriptRoot
$probe=Join-Path (Split-Path $root) 'cmp170hx-validation\build\vram_verify.exe'
if(!(Test-Path $probe)){throw 'Build cmp170hx-validation first.'}
$out=Join-Path "$root\logs" ('vram-0.7-'+(Get-Date -Format yyyyMMdd-HHmmss-fff))
New-Item -ItemType Directory -Path $out | Out-Null
Start-Transcript -LiteralPath "$out\transcript.txt" | Out-Null
try {
    $rows=@(& nvidia-smi.exe '--query-gpu=uuid,pci.device_id,pci.bus_id,memory.total' '--format=csv,noheader,nounits')
    if($LASTEXITCODE){throw 'NVIDIA capacity query failed.'}
    $rows | Set-Content "$out\nvidia-before.csv"
    $cards=@($rows | ConvertFrom-Csv -Header uuid,id,bus,mib | Where-Object {$_.id.Trim() -eq '0x20C210DE'})
    if($cards.Count -ne 1){throw 'Expected exactly one 20C2 GPU.'}
    $total=0
    if(![int]::TryParse($cards[0].mib.Trim(),[ref]$total) -or $total -lt 64512 -or $total -gt 65536){throw 'NVIDIA has not exposed the expected 64GiB geometry. No CUDA allocation attempted.'}
    $bus=$cards[0].bus.Trim()
    # NVML prints an eight-digit PCI domain; CUDA accepts the four-digit form.
    if($bus -match '^0000([0-9A-Fa-f]{4}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-7])$'){$bus=$Matches[1]}
    $sizes=@(12288)
    if($Full){$sizes+=61440}
    foreach($mib in $sizes) {
        & $probe --pci-bus-id $bus --min-total-mib 64512 --mib $mib --passes $Passes
        if($LASTEXITCODE){throw "CUDA $mib MiB address verification failed (exit $LASTEXITCODE). Stop testing and preserve logs."}
    }
    & nvidia-smi.exe -q | Set-Content "$out\nvidia-after.txt"
    if($LASTEXITCODE){throw 'NVIDIA communication failed after CUDA verification.'}
    Write-Host 'PASS: requested allocations were fully written and verified. This does not prove absence of Windows paging or long-term HBM stability.'
} finally {
    Write-Host "EVIDENCE: $out"
    Stop-Transcript | Out-Null
}
