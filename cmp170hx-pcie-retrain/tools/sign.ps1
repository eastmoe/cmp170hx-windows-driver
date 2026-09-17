$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
# 复用 cmp170hx-windows-driver 的本地开发证书(同一测试签名信任链)
$cert=Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq 'CN=CMP170HX Local Development Test' -and $_.HasPrivateKey } | Sort-Object NotAfter -Descending | Select-Object -First 1
if(!$cert){throw 'CMP170HX local dev certificate with private key not found in Cert:\CurrentUser\My'}
if($cert.NotAfter -le (Get-Date)){throw 'certificate expired'}
$signtool="${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
# WDM 服务模式：无 INF/目录文件，.sys 内嵌签名即满足服务加载校验
foreach($file in 'cmpretrain.sys','retrainctl.exe') {
    & $signtool sign /s My /sha1 $cert.Thumbprint /fd SHA256 "$root\dist\$file"
    if($LASTEXITCODE){throw "Signing failed: $file"}
}
Get-ChildItem "$root\dist" -File | Where-Object Name -NE SHA256SUMS.txt | Get-FileHash -Algorithm SHA256 | ForEach-Object {"$($_.Hash.ToLower())  $(Split-Path $_.Path -Leaf)"} | Set-Content "$root\dist\SHA256SUMS.txt"
Write-Host "signed with $($cert.Thumbprint); no trust/boot/driver state changed."
