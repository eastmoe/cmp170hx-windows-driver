param([string]$Python='C:\Users\xkw19\.cache\codex-runtimes\codex-primary-runtime\dependencies\python\python.exe')
$ErrorActionPreference='Stop'
Push-Location (Split-Path $PSScriptRoot)
try {
    $vs=& "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -property installationPath
    $ver=(Get-Content "$vs\VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt").Trim()
    $vc="$vs\VC\Tools\MSVC\$ver";$sdk="${env:ProgramFiles(x86)}\Windows Kits\10";$sv='10.0.26100.0'
    & "$vc\bin\Hostx64\x64\cl.exe" /nologo /LD /O1 /MT "/I$vc\include" "/I$sdk\Include\$sv\ucrt" /Fobuild\memory_oracle.obj /Febuild\memory_oracle.dll tests\memory_exports.c /link "/libpath:$vc\lib\x64" "/libpath:$sdk\Lib\$sv\ucrt\x64" "/libpath:$sdk\Lib\$sv\um\x64"
    if($LASTEXITCODE){throw 'Memory builder compilation failed'}
    & $Python tests\test_memory_oracle.py
    if($LASTEXITCODE){throw 'Original x64 equivalence test failed'}
} finally {Pop-Location}
