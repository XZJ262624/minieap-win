param([string]$VisualStudioPath)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputDir = Join-Path $repoRoot 'port-validation/capture-tests'
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
if (-not $VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $VisualStudioPath) { throw 'Visual Studio C++ build tools were not found.' }
& (Join-Path $VisualStudioPath 'Common7/Tools/Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

$sources = @(
    (Join-Path $PSScriptRoot 'capture_smoke.c'),
    (Join-Path $repoRoot 'src/minieap/if_impl/libpcap/if_impl_libpcap.c'),
    (Join-Path $repoRoot 'src/minieap/util/sched_alarm.c'),
    (Join-Path $repoRoot 'src/minieap/util/logging.c')
)
$exe = Join-Path $outputDir 'capture-smoke.exe'
$compileArguments = @('/nologo', '/TC', '/std:c11', '/utf-8', '/W3', '/Od', '/RTC1', '/MDd',
    '/D_CRT_SECURE_NO_WARNINGS', '/DWIN32_LEAN_AND_MEAN', '/D_WIN32_WINNT=0x0601',
    "/I$(Join-Path $repoRoot 'compat')", "/I$(Join-Path $repoRoot 'src/minieap/include')",
    "/I$(Join-Path $repoRoot 'third_party/npcap-sdk/Include')", "/Fe$exe")
Push-Location $outputDir
try {
    # No wpcap.lib/Packet.lib: every pcap call is a local fake in capture_smoke.c.
    $buildOutput = & cl.exe @compileArguments @sources 2>&1
    $buildOutput | Set-Content -LiteralPath (Join-Path $outputDir 'build.log') -Encoding utf8
    if ($LASTEXITCODE -ne 0) {
        $buildOutput | Write-Host
        throw 'Offline capture test build failed.'
    }
    $info = [Diagnostics.ProcessStartInfo]::new()
    $info.FileName = $exe
    $info.WorkingDirectory = $outputDir
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.StandardOutputEncoding = [Text.Encoding]::UTF8
    $info.StandardErrorEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::new()
    $process.StartInfo = $info
    try {
        if (-not $process.Start()) { throw 'Could not start offline capture tests.' }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(15000)) {
            $process.Kill()
            $process.WaitForExit()
            throw 'Offline capture loop failed to terminate within 15 seconds.'
        }
        $output = $stdout.Result + $stderr.Result
        [IO.File]::WriteAllText((Join-Path $outputDir 'results.log'), $output, [Text.Encoding]::UTF8)
        $output | Write-Host
        if ($process.ExitCode -ne 0) { throw "Offline capture test exited with $($process.ExitCode)." }
    } finally {
        $process.Dispose()
    }
} finally {
    Pop-Location
}
