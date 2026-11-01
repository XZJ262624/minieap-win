param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root ('.local-validation\launcher ' + [guid]::NewGuid().ToString('N')) }
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$launcher = Join-Path $root 'windows\Start-SCNUNET.ps1'
$probe = Join-Path $OutputDirectory 'scnunet-launcher-probe.exe'
$driver = Join-Path $OutputDirectory 'launcher-console-driver.exe'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC tools not found.' }
& (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Push-Location $OutputDirectory
try {
    & cl.exe /nologo /TC /std:c11 /utf-8 /O2 /GS /MD /D_CRT_SECURE_NO_WARNINGS "/I$root\compat" "/I$root\src\minieap\include" "/Fe$probe" "$PSScriptRoot\launcher_probe.c" "$root\compat\win32_compat.c" "$root\src\minieap\util\pid_lock.c" "$root\src\minieap\util\logging.c" /link Ws2_32.lib *> probe-build.log
    if ($LASTEXITCODE -ne 0) { throw 'Probe build failed; see probe-build.log.' }
    & cl.exe /nologo /TC /utf-8 /W4 /O2 /GS /MD "/Fe$driver" "$PSScriptRoot\launcher_console_driver.c" /link User32.lib *> driver-build.log
    if ($LASTEXITCODE -ne 0) { throw 'Console driver build failed; see driver-build.log.' }
} finally { Pop-Location }

function Assert($Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}
function New-Fixture([string]$Mode) {
    $conf = Join-Path $OutputDirectory (([guid]::NewGuid().ToString('N')) + ' test.conf')
    [IO.File]::WriteAllText($conf, "SCNUNET-LAUNCHER-TEST:$Mode")
    return $conf
}
function Start-Child([string]$Exe, [string]$Arguments) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $Exe
    $info.Arguments = $Arguments
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardInput = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $child = New-Object Diagnostics.Process
    $child.StartInfo = $info
    [void]$child.Start()
    return $child
}
function Invoke-Launcher([string]$Conf, [string]$Answer = "Y`n`n", [string]$Exe = $probe, [switch]$Validate, [string]$PidPath = "$Conf.pid") {
    $arguments = "-NoLogo -NoProfile -ExecutionPolicy Bypass -File `"$launcher`" -MiniEapPath `"$Exe`" -ConfigPath `"$Conf`" -PidFilePath `"$PidPath`""
    if ($Validate) { $arguments += ' -ValidateOnly' }
    $child = Start-Child $shell $arguments
    try {
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        $child.StandardInput.Write($Answer)
        $child.StandardInput.Close()
        if (-not $child.WaitForExit(12000)) { $child.Kill(); throw 'Launcher timeout.' }
        $text = $stdout.Result + $stderr.Result
        [IO.File]::WriteAllText("$Conf.output.txt", $text)
        return [pscustomobject]@{ Code = $child.ExitCode; Text = $text }
    } finally { $child.Dispose() }
}

$shells = @("$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe")
$pwsh = Get-Command pwsh.exe -ErrorAction SilentlyContinue
if ($pwsh) { $shells += $pwsh.Source }
foreach ($shell in $shells) {
    # Parse in each installed engine, including Windows PowerShell 5.1.
    $parse = '& { $errors=$null; Get-ChildItem ''' + $root + '\windows'' -Filter *.ps1 | ForEach-Object { [void][Management.Automation.Language.Parser]::ParseFile($_.FullName,[ref]$null,[ref]$errors); if($errors){throw ($errors|Out-String)} } }'
    & $shell -NoProfile -Command $parse
    Assert ($LASTEXITCODE -eq 0) 'PowerShell syntax check failed.'
    $conf = New-Fixture normal
    $result = Invoke-Launcher $conf -Validate
    Assert ($result.Code -eq 0 -and -not (Test-Path "$conf.ready")) 'ValidateOnly launched a child or failed.'
    foreach ($answer in @("N`n", "n`n")) {
        $conf = New-Fixture normal
        $result = Invoke-Launcher $conf $answer
        Assert ($result.Code -eq 0 -and -not (Test-Path "$conf.ready")) 'N launched a child or failed.'
    }
    foreach ($answer in @("`n`n", "Y`n`n", "y`n`n", "invalid`nY`n`n")) {
        $conf = New-Fixture normal
        $result = Invoke-Launcher $conf $answer
        Assert ($result.Code -eq 0 -and $result.Text.Contains('PROBE Keep-Alive 2') -and $result.Text.Contains('PROBE stderr')) 'Enter/Y/output/normal exit failed.'
        Assert ($result.Text.Contains("PROBE cwd=$OutputDirectory")) 'Working directory or paths containing spaces were changed.'
        Assert (-not (Test-Path "$conf.pid")) 'PID lock was not removed on normal exit.'
    }
    $conf = New-Fixture normal
    $result = Invoke-Launcher $conf -Exe "$OutputDirectory\missing.exe" -Validate
    Assert ($result.Code -eq 1 -and -not (Test-Path "$conf.ready")) 'Missing EXE validation failed.'
    $result = Invoke-Launcher "$conf.missing" -Validate
    Assert ($result.Code -eq 1) 'Missing config validation failed.'
    foreach ($mode in @('nonzero', 'abnormal')) {
        $conf = New-Fixture $mode
        $result = Invoke-Launcher $conf
        $expected = if ($mode -eq 'nonzero') { 7 } else { -1073740791 }
        Assert ($result.Code -eq $expected) "$mode exit code was lost."
        if ($mode -eq 'abnormal') { Assert ($result.Text.Contains('0xC0000409')) 'Hex exception code was lost.' }
        Assert (-not (Test-Path "$conf.pid")) 'PID file survived process termination.'
    }
    # A stale PID file is harmless: the held file handle is the actual lock.
    $conf = New-Fixture normal
    [IO.File]::WriteAllText("$conf.pid", '12345')
    $result = Invoke-Launcher $conf
    Assert ($result.Code -eq 0 -and -not (Test-Path "$conf.pid")) 'Stale PID file recovery failed.'

    foreach ($differentName in @($false, $true)) {
        $holdingConf = New-Fixture hold
        $holderExe = $probe
        if ($differentName) {
            $holderExe = Join-Path $OutputDirectory 'scnunet-lock-holder.exe'
            Copy-Item -LiteralPath $probe -Destination $holderExe -Force
        }
        $holder = Start-Child $holderExe "--conf-file `"$holdingConf`" --pid-file `"$holdingConf.pid`""
        try {
            $deadline = [DateTime]::UtcNow.AddSeconds(5)
            while (-not (Test-Path "$holdingConf.ready") -and [DateTime]::UtcNow -lt $deadline) { Start-Sleep -Milliseconds 50 }
            Assert (Test-Path "$holdingConf.ready") 'Lock holder did not start.'
            $conf = New-Fixture normal
            $result = Invoke-Launcher $conf -PidPath "$holdingConf.pid"
            $expected = if ($differentName) { 96 } else { 0 }
            Assert ($result.Code -eq $expected -and -not (Test-Path "$conf.ready")) 'Duplicate process / exclusive PID lock protection failed.'
            Assert (-not $holder.HasExited) 'Existing instance was disturbed.'
        } finally {
            [IO.File]::WriteAllText("$holdingConf.stop", 'stop test fixture only')
            if (-not $holder.WaitForExit(5000)) { $holder.Kill() }
            $holder.Dispose()
        }
    }
    foreach ($answer in @('Enter', 'Y')) {
        $conf = New-Fixture hold
        $command = "`"$shell`" -NoLogo -NoProfile -ExecutionPolicy Bypass -File `"$launcher`" -MiniEapPath `"$probe`" -ConfigPath `"$conf`" -PidFilePath `"$conf.pid`""
        & $driver $command $conf $answer
        Assert ($LASTEXITCODE -eq 0) 'Isolated interactive console test failed.'
        Assert (([IO.File]::ReadAllText("$conf.ready")).Contains('stdin=1 stdout=1 stderr=1 cp=65001')) 'Native handles did not inherit the console.'
        Assert (([IO.File]::ReadAllText("$conf.stopped")).Contains('stop=1 cp_restored=1')) 'Real Ctrl+C or console cleanup failed.'
        Assert (-not (Test-Path "$conf.pid")) 'Ctrl+C did not release the PID lock.'
    }
    Write-Host "PASS: $shell; syntax, ValidateOnly, N/n, Enter/Y/y, invalid input, missing paths, exit codes, duplicates, PID locking, stale PID, console/Ctrl+C."
}
