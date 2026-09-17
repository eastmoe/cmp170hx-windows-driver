$ErrorActionPreference='Stop'
[Console]::OutputEncoding=[System.Text.Encoding]::UTF8  # retrainctl 输出为 UTF-8
$root=Split-Path $PSScriptRoot
$id=[Security.Principal.WindowsIdentity]::GetCurrent()
$prin=New-Object Security.Principal.WindowsPrincipal($id)
if(!$prin.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw '需要管理员 PowerShell'}
foreach($f in 'cmpretrain.sys','retrainctl.exe'){
    if(!(Test-Path "$root\dist\$f")){throw "dist\$f 缺失，请先 .\build.ps1 -Sign"}
}
$ts=(bcdedit /enum "{current}" | Select-String 'testsigning\s+Yes')
if(!$ts){Write-Host '警告: 当前启动项未启用 testsigning；未签名或不受信驱动将无法加载'}
# WDM 服务模式：SCM 注册 + 立即加载，无需 INF/PnP
& "$root\dist\retrainctl.exe" --install -Sys "$root\dist\cmpretrain.sys"
if($LASTEXITCODE){throw "install failed rc=$LASTEXITCODE"}
& "$root\dist\retrainctl.exe" --query
