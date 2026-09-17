$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$id=[Security.Principal.WindowsIdentity]::GetCurrent()
$prin=New-Object Security.Principal.WindowsPrincipal($id)
if(!$prin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw '需要管理员 PowerShell'}
& "$root\dist\retrainctl.exe" --uninstall
exit $LASTEXITCODE
