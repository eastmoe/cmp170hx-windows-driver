# Shared, side-effect-free helpers. manage.ps1, tools\autostart.ps1 and
# tools\test-cleanup.ps1 dot-source this file; the offline runners in tests\ dot-source it
# too, so their mocks only have to replace the real hardware/OS readers.
# Nothing here touches a device, the registry or the network.

# cmpctl runs every present card and prefixes one block per device with its interface path,
# so a caller can attribute each result to one board.
function Get-CmpCtlBlock([string]$Text) {
    $blocks=New-Object System.Collections.Generic.List[object]
    $instance=$null;$index=-1;$lines=New-Object System.Collections.Generic.List[string]
    foreach($line in ($Text -split "`r?`n")) {
        if($line -match '^DEVICE index=(\d+) count=(\d+) path=(.+)$') {
            if($instance){$blocks.Add([pscustomobject]@{Index=$index;Instance=$instance;Text=($lines -join "`n")})}
            $index=[int]$Matches[1]
            $instance=((($Matches[3].Trim() -replace '^\\\\\?\\','') -replace '#\{[^}]*\}$','') -replace '#','\')
            $lines=New-Object System.Collections.Generic.List[string]
        } elseif($instance){$lines.Add($line)}
    }
    if($instance){$blocks.Add([pscustomobject]@{Index=$index;Instance=$instance;Text=($lines -join "`n")})}
    return $blocks
}

# One block per present card and no card twice: a partial or duplicated report is never trusted.
function Assert-CmpCtlBlock([string]$Text,[int]$Expected,[string]$Stage) {
    $blocks=@(Get-CmpCtlBlock $Text)
    if($blocks.Count -ne $Expected){throw "$Stage reported $($blocks.Count) device block(s); expected $Expected. No further hardware work."}
    $seen=@{}
    foreach($block in $blocks) {
        $key=$block.Instance.ToUpperInvariant()
        if($seen.ContainsKey($key)){throw "$Stage reported device $($block.Instance) twice."}
        $seen[$key]=$true
    }
    return $blocks
}

# Pre-RUN gate, evaluated per card: an unknown or partially configured board is never touched.
function Assert-CmpPreRun([object]$Block,[string]$Mode) {
    $who=$Block.Instance
    if($Block.Text -notmatch 'protocol=7 build=000A0003 .*state=0 .*mode=0'){throw "Expected a fresh protocol-7 (build 000A0003) device on $who. No RUN performed."}
    if($Mode -in 'Compute','ResetOnly') {
        if($Block.Text -notmatch 'RESET query=0x00000000 supported=0x([0-9A-Fa-f]+)' -or
           ([Convert]::ToUInt32($Matches[1],16) -band 1) -eq 0){throw "Function-level reset unavailable on $who. No payload executed."}
    }
    if($Mode -in 'Compute','Memory') {
        if($Block.Text -notmatch '(?m)^00823804\s+[0-9A-F]{8}\s+FFFFFF8F\s*$' -or
           $Block.Text -notmatch '(?m)^001FA824\s+[0-9A-F]{8}\s+1FFFFE00\s*$' -or
           $Block.Text -notmatch '(?m)^001FA828\s+[0-9A-F]{8}\s+00000000\s*$') {
            throw "FEAT/WPR baseline is unsuitable on $who. Power cycle without EFI unlock; no RUN performed."
        }
        if($Mode -eq 'Memory' -and (
           $Block.Text -notmatch 'device=20C2 ' -or
           $Block.Text -notmatch '(?m)^009A0204\s+[0-9A-F]{8}\s+02449000\s*$' -or
           $Block.Text -notmatch '(?m)^00100CE0\s+[0-9A-F]{8}\s+00000208\s*$')) {
            throw "Memory requires a cold 20C2 8GiB baseline on ${who}: CFG1=02449000 LMR=00000208. No RUN performed."
        }
    }
}

# Recovery records are written as InstanceIds[]; older single-device records used InstanceId.
function Get-CmpRecoveryInstances($saved) {
    $ids=@($saved.InstanceIds | Where-Object {$_})
    if($ids.Count -eq 0){$ids=@($saved.Instances | Where-Object {$_})}
    if($ids.Count -eq 0){$ids=@($saved.InstanceId | Where-Object {$_})}
    return $ids
}

function Test-CmpSameSet($left,$right) {
    return (@(Compare-Object -ReferenceObject @($left) -DifferenceObject @($right)).Count -eq 0)
}
