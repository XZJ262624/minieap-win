param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$Rebuild
)

$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$msbuild = & $vswhere -latest -products '*' -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'Visual Studio 2022 C++ build tools were not found.' }
$target = if ($Rebuild) { 'Rebuild' } else { 'Build' }
$logDir = Join-Path $PSScriptRoot 'port-validation'
New-Item -ItemType Directory -Force -Path $logDir | Out-Null
& $msbuild (Join-Path $PSScriptRoot 'minieap-win.sln') "/t:$target" "/p:Configuration=$Configuration" /p:Platform=x64 /v:minimal /nologo "/flp:logfile=$logDir\$Configuration-x64-build.log;verbosity=normal;encoding=UTF-8"
if ($LASTEXITCODE -ne 0) { throw "MiniEAP $Configuration x64 build failed ($LASTEXITCODE)." }
