param([string]$VisualStudioPath, [string]$OutputDirectory)

$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$validationRoot = Join-Path $repoRoot 'port-validation'
$buildRoot = if ($OutputDirectory) { [IO.Path]::GetFullPath($OutputDirectory) } else { Join-Path $validationRoot 'algorithm-tests' }
$baselineRoot = Join-Path $buildRoot 'algorithm-baseline'
$archivePath = Join-Path $validationRoot 'before-windows-port.zip'
if (-not (Test-Path -LiteralPath $archivePath)) {
    throw 'The pre-port snapshot port-validation/before-windows-port.zip is required.'
}
if (-not $VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $VisualStudioPath) { throw 'Visual Studio C++ build tools were not found.' }
& (Join-Path $VisualStudioPath 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null

New-Item -ItemType Directory -Path $baselineRoot, $buildRoot -Force | Out-Null
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
try {
    foreach ($entry in $archive.Entries) {
        # Extract only the original, offline crypto sources and their headers.
        if ($entry.FullName -notmatch '^src/minieap/(md5\.c|include/(md5|types|ustd)\.h|packet_plugin/rjv3/rjv3_hashes/[^/]+\.[ch])$') {
            continue
        }
        $destination = Join-Path $baselineRoot $entry.FullName
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $destination, $true)
    }
} finally {
    $archive.Dispose()
}
# Baseline adaptations are limited to these two compatibility headers.
# Baseline md5.c, checkV4.c and all other algorithm sources remain from the ZIP.
foreach ($header in @('types.h', 'ustd.h')) {
    Copy-Item -LiteralPath (Join-Path $repoRoot "src/minieap/include/$header") -Destination (Join-Path $baselineRoot "src/minieap/include/$header")
}

function Build-CryptoRegression([string]$SourceRoot, [string]$Name) {
    $outputDir = Join-Path $buildRoot $Name
    New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
    $includeDir = Join-Path $SourceRoot 'src/minieap/include'
    $hashDir = Join-Path $SourceRoot 'src/minieap/packet_plugin/rjv3/rjv3_hashes'
    $sources = @((Join-Path $PSScriptRoot 'crypto_regression.c'), (Join-Path $SourceRoot 'src/minieap/md5.c'))
    $sources += @(Get-ChildItem -LiteralPath $hashDir -Filter '*.c' | Sort-Object Name | ForEach-Object FullName)
    $exe = Join-Path $outputDir 'crypto-regression.exe'
    $arguments = @('/nologo', '/TC', '/std:c11', '/utf-8', '/W3', '/Od', '/RTC1', '/MDd', '/D_CRT_SECURE_NO_WARNINGS', "/I$includeDir", "/I$hashDir", "/Fe$exe")
    Push-Location $outputDir
    try {
        $buildOutput = & cl.exe @arguments @sources 2>&1
        $buildOutput | Set-Content -LiteralPath (Join-Path $outputDir 'build.log') -Encoding utf8
        if ($LASTEXITCODE -ne 0) {
            $buildOutput | Write-Host
            throw "$Name crypto build failed."
        }
    } finally {
        Pop-Location
    }
    return $exe
}

$baselineExe = Build-CryptoRegression $baselineRoot 'baseline'
$currentExe = Build-CryptoRegression $repoRoot 'current'
$baselineOutput = @(& $baselineExe)
if ($LASTEXITCODE -ne 0) { throw 'Baseline crypto regression failed.' }
$currentOutput = @(& $currentExe)
if ($LASTEXITCODE -ne 0) { throw 'Current crypto regression failed.' }
$baselineOutput | Set-Content -LiteralPath (Join-Path $buildRoot 'baseline-results.txt') -Encoding utf8
$currentOutput | Set-Content -LiteralPath (Join-Path $buildRoot 'current-results.txt') -Encoding utf8
if (($baselineOutput -join "`n") -cne ($currentOutput -join "`n")) {
    Compare-Object $baselineOutput $currentOutput | Out-String | Write-Host
    throw 'Crypto outputs differ from the adapted pre-port baseline.'
}
$longOutput = @(& $currentExe --long-credentials)
if ($LASTEXITCODE -ne 0) { throw '64-byte credential regression failed.' }
$longOutput | Set-Content -LiteralPath (Join-Path $buildRoot 'long-credentials-results.txt') -Encoding utf8
Write-Host 'PASS: MD5 RFC vectors; all five RJv3 branches; high-byte challenge; computePwd; RJ encode/decode.'
Write-Host 'PASS: current outputs match pre-port algorithm sources (only types.h and ustd.h adapted).'
Write-Host 'PASS: 64-byte username/password, compiled with MSVC x64 /RTC1.'
