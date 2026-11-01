param([string]$VisualStudioPath, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputDir = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $repoRoot 'port-validation/packet-tests' }
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
if (-not $VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $VisualStudioPath) { throw 'Visual Studio C++ build tools were not found.' }
& (Join-Path $VisualStudioPath 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$hashDir = Join-Path $repoRoot 'src/minieap/packet_plugin/rjv3/rjv3_hashes'
$sources = @('tests/packet_smoke.c', 'compat/getopt.c', 'compat/win32_compat.c',
    'src/minieap/config.c', 'src/minieap/eap_state_machine.c', 'src/minieap/md5.c',
    'src/minieap/packet_builder.c', 'src/minieap/if_impl/if_impl.c',
    'src/minieap/packet_plugin/packet_plugin.c', 'src/minieap/packet_plugin/printer/packet_plugin_printer.c',
    'src/minieap/packet_plugin/rjv3/packet_plugin_rjv3.c',
    'src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_priv.c',
    'src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_prop.c',
    'src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_keepalive.c',
    'src/minieap/util/conf_parser.c', 'src/minieap/util/linkedlist.c',
    'src/minieap/util/logging.c', 'src/minieap/util/misc.c',
    'src/minieap/util/packet_util.c', 'src/minieap/util/sched_alarm.c') |
    ForEach-Object { Join-Path $repoRoot $_ }
$sources += @(Get-ChildItem -LiteralPath $hashDir -Filter '*.c' | Sort-Object Name | ForEach-Object FullName)
$exe = Join-Path $outputDir 'packet-smoke.exe'
$arguments = @('/nologo', '/TC', '/std:c11', '/utf-8', '/W3', '/Od', '/RTC1', '/MDd',
    '/D_CRT_SECURE_NO_WARNINGS', '/DSTATIC_GETOPT', '/DWIN32_LEAN_AND_MEAN', '/DNOMINMAX',
    "/I$repoRoot/compat", "/I$repoRoot/src/minieap/include", "/I$repoRoot/src/minieap/packet_plugin/rjv3",
    "/I$hashDir", "/FI$repoRoot/compat/win32_compat.h", "/Fe$exe")
Push-Location $outputDir
try {
    $buildOutput = & cl.exe @arguments @sources '/link' 'Ws2_32.lib' 2>&1
    $buildOutput | Set-Content -LiteralPath (Join-Path $outputDir 'build.log') -Encoding utf8
    if ($LASTEXITCODE -ne 0) {
        $buildOutput | Write-Host
        throw 'Offline packet test build failed.'
    }
    $output = @(& $exe 2>&1)
    $result = $LASTEXITCODE
    $output | Set-Content -LiteralPath (Join-Path $outputDir 'results.txt') -Encoding utf8
    $output | Write-Host
    if ($result -ne 0) { throw "Offline packet tests failed ($result)." }
} finally { Pop-Location }
