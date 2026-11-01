param([string]$OutputDirectory, [switch]$ReuseFixtures)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $OutputDirectory){$OutputDirectory=Join-Path $root ('.local-validation\window-'+[guid]::NewGuid().ToString('N'))}
$OutputDirectory=[IO.Path]::GetFullPath($OutputDirectory)
# Build and run the pre-existing offline native invocation/Ctrl+C regression too.
if (-not $ReuseFixtures) { & (Join-Path $PSScriptRoot 'test-launcher.ps1') -OutputDirectory $OutputDirectory }
$probe=Join-Path $OutputDirectory 'scnunet-launcher-probe.exe'
$driver=Join-Path $OutputDirectory 'launcher-console-driver.exe'
$conf=Join-Path $OutputDirectory (([guid]::NewGuid().ToString('N'))+' managed test.conf')
[IO.File]::WriteAllText($conf,'SCNUNET-LAUNCHER-TEST:hold')
$launcher=Join-Path $root 'windows\Start-SCNUNET.ps1'
$manager=Join-Path $root 'windows\Open-SCNUNET.ps1'
$shell="$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe"
$paths='-MiniEapPath "{0}" -ConfigPath "{1}" -PidFilePath "{2}.pid"' -f $probe,$conf,$conf
function Assert($ok,[string]$message){if(-not $ok){throw $message}}
function Owners {
    @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object {
        $_.CommandLine -like ('* -File "'+$launcher+'" *') -and $_.CommandLine.Contains($conf)
    } | ForEach-Object {Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue})
}
function Wait-For([scriptblock]$Condition,[string]$Message){
    $deadline=[DateTime]::UtcNow.AddSeconds(10)
    while([DateTime]::UtcNow -lt $deadline){if(& $Condition){return};Start-Sleep -Milliseconds 100}
    throw $Message
}
function Manager([string]$Extra=''){
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$shell
    $info.Arguments='-NoProfile -ExecutionPolicy Bypass -File "{0}" -LauncherPath "{1}" {2} -ConnectionWindowStyle Hidden -NoErrorWindow {3}' -f $manager,$launcher,$paths,$Extra
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $info.RedirectStandardOutput=$true;$info.RedirectStandardError=$true
    $p=[Diagnostics.Process]::Start($info)
    $stdout=$p.StandardOutput.ReadToEndAsync();$stderr=$p.StandardError.ReadToEndAsync()
    try{
        if(-not $p.WaitForExit(25000)){throw 'Window manager timed out.'}
        [IO.File]::WriteAllText((Join-Path $OutputDirectory 'manager-last.txt'),$stdout.Result+$stderr.Result)
        Assert ($p.ExitCode -eq 0) ('Window manager failed: '+$stderr.Result)
    }finally{$p.Dispose()}
}
$initial=0
try {
    Manager '-ValidateOnly'
    Assert ((Owners).Count -eq 0) 'Validation opened a window.'
    $command='"{0}" -NoProfile -ExecutionPolicy Bypass -File "{1}" {2}' -f $shell,$launcher,$paths
    $initial=[int](& $driver --spawn $command)
    Wait-For { (Owners).Count -eq 1 } 'Legacy window not discoverable.'
    Start-Sleep -Milliseconds 800
    & $driver --key $initial Y
    Wait-For {Test-Path "$conf.ready"} 'Offline native child did not start.'
    $oldChild=[int](Get-CimInstance Win32_Process -Filter "ParentProcessId=$initial" | Where-Object ExecutablePath -IEQ $probe | Select-Object -ExpandProperty ProcessId)
    Manager
    Assert ((Owners).Count -eq 1 -and (Owners)[0].Id -eq $initial) 'Desktop invocation duplicated the existing window.'
    Assert (-not (Test-Path "$conf.stopped")) 'Desktop invocation stopped authentication.'
    # Wrong process identity must be rejected BEFORE attaching or sending Ctrl+C.
    $info=New-Object Diagnostics.ProcessStartInfo
    $info.FileName=$shell
    $info.Arguments='-NoProfile -ExecutionPolicy Bypass -File "{0}" -LauncherPath "{1}" {2} -ConsoleProcessId {3} -ConsoleStartTicks 1 -StopConsole' -f $manager,$launcher,$paths,$initial
    $info.UseShellExecute=$false;$info.CreateNoWindow=$true
    $bad=[Diagnostics.Process]::Start($info)
    Assert ($bad.WaitForExit(10000) -and $bad.ExitCode -eq 3) 'Stale process identity was accepted.'
    $bad.Dispose()
    Assert (-not (Test-Path "$conf.stopped")) 'Identity rejection disturbed the native child.'

    # Reproduce the user's existing two-window state: one authenticating,
    # another legacy launcher waiting at its initial confirmation.
    $extra=[int](& $driver --spawn $command)
    Wait-For {(Owners).Count -eq 2} 'Second legacy prompt did not start.'
    Start-Sleep -Milliseconds 500
    Manager '-Resume'
    Assert (-not (Get-Process -Id $initial -ErrorAction SilentlyContinue)) 'Legacy console survived resume.'
    Assert (-not (Get-Process -Id $oldChild -ErrorAction SilentlyContinue)) 'Old native child survived resume.'
    Assert (-not (Get-Process -Id $extra -ErrorAction SilentlyContinue)) 'Extra legacy prompt survived resume.'
    Assert (([IO.File]::ReadAllText("$conf.stopped")).Contains('stop=1 cp_restored=1')) 'Native process did not clean up via its actual Ctrl+C handler.'
    Assert (-not (Test-Path "$conf.pid")) 'PID lock was not released.'
    $owner=@(Owners)
    Assert ($owner.Count -eq 1) 'Resume did not leave exactly one confirmation window.'
    $waitingId=$owner[0].Id
    Manager
    Assert ((Owners).Count -eq 1 -and (Owners)[0].Id -eq $waitingId) 'Repeated desktop click duplicated the waiting prompt.'
    Manager '-Resume'
    Assert (-not (Get-Process -Id $waitingId -ErrorAction SilentlyContinue) -and (Owners).Count -eq 1) 'Repeated resume retained the old waiting prompt.'
    # The replacement must wait for confirmation, never authenticate by itself.
    Assert (-not (Test-Path "$conf.pid")) 'Resume automatically authenticated.'
    $replacement=(Owners)[0].Id
    Start-Sleep -Milliseconds 500
    & $driver --key $replacement Y
    Wait-For {Test-Path "$conf.pid"} 'Replacement console did not start the native probe after Y.'
    Manager '-Resume'
    Assert (-not (Get-Process -Id $replacement -ErrorAction SilentlyContinue) -and (Owners).Count -eq 1) 'Resume from a new managed authentication window failed.'
    Assert (-not (Test-Path "$conf.pid")) 'Replacement native child did not release the PID lock.'
    $replacement=(Owners)[0].Id
    Start-Sleep -Milliseconds 500
    & $driver --key $replacement N
    Wait-For {(Owners).Count -eq 0} 'N did not close the replacement window.'
    Write-Host 'PASS: legacy running console -> graceful Ctrl+C/cleanup -> one fresh prompt; normal opening reuses; repeated resume; stale identity rejection; no automatic authentication; N closes.'
} finally {
    # Cleanup only fixtures created with this unique fake config, never real MiniEAP.
    [IO.File]::WriteAllText("$conf.stop",'stop test fixture only')
    foreach($p in @(Owners)) {
        & $driver --key $p.Id N
        if(-not $p.WaitForExit(3000)){ & $driver --key $p.Id Enter; [void]$p.WaitForExit(3000) }
    }
}
