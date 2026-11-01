param([string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $projectRoot 'port-validation\platform-test'
}
$OutputDirectory = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null

$vswherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vsInstallation = & $vswherePath -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstallation) { throw 'Visual Studio MSVC x64 tools were not found.' }
& (Join-Path $vsInstallation 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$sources = @(
    'tests\platform_smoke.c',
    'compat\win32_compat.c',
    'compat\getopt.c',
    'src\minieap\util\misc.c',
    'src\minieap\util\logging.c',
    'src\minieap\util\linkedlist.c',
    'src\minieap\util\sched_alarm.c',
    'src\minieap\util\pid_lock.c'
) | ForEach-Object { Join-Path $projectRoot $_ }
$executable = Join-Path $OutputDirectory 'platform_smoke.exe'
$compilerArguments = @('/nologo', '/TC', '/std:c11', '/utf-8', '/W3', '/Od', '/MDd',
    '/D_CRT_SECURE_NO_WARNINGS', '/DSTATIC_GETOPT', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX',
    '/D_WIN32_WINNT=0x0A00', "/I$(Join-Path $projectRoot 'compat')",
    "/I$(Join-Path $projectRoot 'src\minieap\include')", "/Fe$executable")
Push-Location $OutputDirectory
try {
    & cl.exe @compilerArguments @sources /link Ws2_32.lib
    if ($LASTEXITCODE -ne 0) { throw 'Offline platform test compilation failed.' }
    & $executable $OutputDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Offline platform checks failed.' }
} finally {
    Pop-Location
}
