$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot
$certDir="$root\cert";New-Item -ItemType Directory -Force $certDir | Out-Null
$thumbFile="$certDir\thumbprint.txt"
if(Test-Path $thumbFile) {
    $thumb=(Get-Content $thumbFile -Raw).Trim()
    $cert=Get-Item "Cert:\CurrentUser\My\$thumb"
    if(!$cert.HasPrivateKey -or $cert.NotAfter -le (Get-Date)){throw 'Existing signing certificate unavailable or expired'}
} else {
    $cert=New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=CMP170HX Local Development Test' -FriendlyName 'CMP170HX Local Test Signing' -CertStoreLocation Cert:\CurrentUser\My -KeyAlgorithm RSA -KeyLength 3072 -HashAlgorithm SHA256 -KeyExportPolicy NonExportable -NotAfter (Get-Date).AddYears(2)
    $thumb=$cert.Thumbprint;Set-Content $thumbFile $thumb
}
Export-Certificate -Cert $cert -FilePath "$root\dist\cmp170-local.cer" -Force | Out-Null
$signtool="${env:ProgramFiles(x86)}\Windows Kits\10\bin\10.0.26100.0\x64\signtool.exe"
foreach($file in 'cmp170.sys','cmpctl.exe') {
    & $signtool sign /s My /sha1 $thumb /fd SHA256 "$root\dist\$file"
    if($LASTEXITCODE){throw "Signing failed: $file"}
}
& "$root\vendor\wdk\c\bin\10.0.26100.0\x86\Inf2Cat.exe" "/driver:$root\dist" /os:10_X64 /uselocaltime
if($LASTEXITCODE){throw 'Catalog generation failed'}
& $signtool sign /s My /sha1 $thumb /fd SHA256 "$root\dist\cmp170.cat"
if($LASTEXITCODE){throw 'Catalog signing failed'}
$cert | Select-Object Subject,Thumbprint,NotBefore,NotAfter,HasPrivateKey | ConvertTo-Json | Set-Content "$certDir\certificate.json"
Get-ChildItem "$root\dist" -File | Where-Object Name -NE SHA256SUMS.txt | Get-FileHash -Algorithm SHA256 | ForEach-Object {"$($_.Hash.ToLower())  $(Split-Path $_.Path -Leaf)"} | Set-Content "$root\dist\SHA256SUMS.txt"
Write-Host 'Signed with local non-exportable private key. No root trust, boot setting or driver binding was changed.'
