$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$version='10.0.26100.6584'
New-Item -ItemType Directory -Force "$root\vendor" | Out-Null
Invoke-WebRequest "https://api.nuget.org/v3-flatcontainer/microsoft.windows.wdk.x64/$version/microsoft.windows.wdk.x64.$version.nupkg" -OutFile "$root\vendor\wdk.zip"
if((Get-FileHash "$root\vendor\wdk.zip" -Algorithm SHA256).Hash -ne 'C393D03DFB640B5C92F546B32F6770EF68CD3AAF691956E7D66D8E2C28A1B55E'){throw 'Pinned WDK package hash mismatch'}
Expand-Archive "$root\vendor\wdk.zip" "$root\vendor\wdk" -Force
Get-FileHash "$root\vendor\wdk.zip" -Algorithm SHA256
