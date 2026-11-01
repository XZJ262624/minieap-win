param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root '.local-validation\v4-bounds' }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC tools not found.' }
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$hashDir = Join-Path $root 'src\minieap\packet_plugin\rjv3\rjv3_hashes'
# Preserve every byte of the stable algorithm except temporary storage capacity.
# An oversized reference avoids depending on the repaired capacity calculation.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $root 'port-validation\before-windows-port.zip'))
try {
    $entry = $archive.GetEntry('src/minieap/packet_plugin/rjv3/rjv3_hashes/checkV4.c')
    if (-not $entry) { throw 'Reference checkV4.c is missing from the source-only baseline.' }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { $old = $reader.ReadToEnd() } finally { $reader.Dispose() }
} finally { $archive.Dispose() }
if (([regex]::Matches($old, 'unsigned char wtmp\[160\];')).Count -ne 1) {
    throw 'Expected stable baseline buffer declaration not found.'
}
$reference = Join-Path $OutputDirectory 'reference-checkV4.c'
[IO.File]::WriteAllText($reference, $old.Replace('unsigned char wtmp[160];', 'unsigned char wtmp[8192];'))
$shared = @((Join-Path $PSScriptRoot 'crypto_regression.c'), (Join-Path $root 'src\minieap\md5.c'))
$shared += @(Get-ChildItem $hashDir -Filter '*.c' | Where-Object Name -ne 'checkV4.c' | ForEach-Object FullName)
$expected = $null
foreach ($mode in @('reference', 'Debug', 'Release', 'ASan')) {
    $dir = Join-Path $OutputDirectory $mode
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    $v4 = if ($mode -eq 'reference') { $reference } else { Join-Path $hashDir 'checkV4.c' }
    $flags = if ($mode -eq 'Debug') { @('/Od', '/RTC1', '/MDd') }
        elseif ($mode -eq 'ASan') { @('/O2', '/MD', '/fsanitize=address') }
        else { @('/O2', '/GS', '/MD') }
    Push-Location $dir
    try {
        & cl.exe /nologo /TC /std:c11 /utf-8 /Zi /D_CRT_SECURE_NO_WARNINGS "/I$hashDir" "/I$root\src\minieap\include" /Fev4-bounds.exe @flags @shared $v4 *> build.log
        if ($LASTEXITCODE -ne 0) { throw "$mode build failed; see $dir\build.log" }
        & .\v4-bounds.exe --v4-bounds > results.txt 2> errors.txt
        if ($LASTEXITCODE -ne 0) { throw "$mode regression failed; see $dir\errors.txt" }
        $actual = (Get-FileHash results.txt -Algorithm SHA256).Hash
        if ($expected -and $actual -ne $expected) { throw "$mode changed the RJv3 output bytes." }
        $expected = $actual
        Write-Host "PASS: $mode, 4096 challenges, identical output ($actual)"
    } finally { Pop-Location }
}
