# Read-only collection works even when the driver exposes no device interface.
function Save-CmpDiagnostics([string]$Directory) {
    New-Item -ItemType Directory -Force $Directory | Out-Null
    function Capture([string]$Name,[scriptblock]$Read) {
        try { & $Read 2>&1 | Out-String -Width 300 | Set-Content -LiteralPath "$Directory\$Name.txt" -Encoding UTF8 }
        catch { $_ | Out-String | Set-Content -LiteralPath "$Directory\$Name.error.txt" -Encoding UTF8 }
    }
    Capture device {
        Get-PnpDevice -PresentOnly | Where-Object InstanceId -Match '^PCI\\VEN_10DE&DEV_(20C2|2082)&' | ForEach-Object {
            $_ | Format-List *
            Get-PnpDeviceProperty -InstanceId $_.InstanceId | Format-List KeyName,Type,Data
            $problem=(Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_ProblemStatus).Data
            'ProblemStatus hex: 0x{0:X8}' -f $problem
        }
    }
    Capture platform { Get-CimInstance Win32_OperatingSystem | Format-List Caption,Version,BuildNumber,LastBootUpTime; bcdedit.exe /enum '{current}'; Get-CimInstance -Namespace root\Microsoft\Windows\DeviceGuard -ClassName Win32_DeviceGuard | Format-List * }
    Capture service { sc.exe query cmp170; sc.exe qc cmp170; reg.exe query HKLM\SYSTEM\CurrentControlSet\Services\cmp170 /s }
    Capture tasks { Get-ScheduledTask -TaskName CMP170HX-AutoUnlock -ErrorAction SilentlyContinue | Format-List *; Get-ScheduledTaskInfo -TaskName CMP170HX-AutoUnlock -ErrorAction SilentlyContinue | Format-List * }
    foreach($channel in 'System','Microsoft-Windows-CodeIntegrity/Operational','Microsoft-Windows-Kernel-PnP/Configuration') {
        $name=$channel -replace '[/\\]','-'
        Capture $name { Get-WinEvent -FilterHashtable @{LogName=$channel;StartTime=(Get-Date).AddDays(-2)} -MaxEvents 500 -ErrorAction Stop | Format-List TimeCreated,ProviderName,Id,LevelDisplayName,Message }
        Capture "$name-export" { wevtutil.exe epl $channel "$Directory\$name.evtx" /ow:true '/q:*[System[TimeCreated[timediff(@SystemTime) <= 172800000]]]' }
    }
    Capture kernel { Get-Content "$env:SystemRoot\Temp\cmp170-kernel.log" -Tail 10000 -ErrorAction Stop }
    Capture setupapi { Get-Content "$env:SystemRoot\INF\setupapi.dev.log" -Tail 12000 -ErrorAction Stop }
    Capture nvidia { nvidia-smi.exe -q }
}
