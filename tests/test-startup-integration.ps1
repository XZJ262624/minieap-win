param([string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root ('.local-validation\startup-' + [guid]::NewGuid().ToString('N')) }
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
function Assert($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
$installer = Join-Path $root 'windows\Install-SCNUNETStartupTask.ps1'
$shortcutInstaller = Join-Path $root 'windows\Install-SCNUNETDesktopShortcut.ps1'
$definitions = @(& $installer -ValidateOnly -PassThru)
Assert ($definitions.Count -eq 2) 'Expected separate logon/desktop and resume tasks.'
$task = $definitions[0]
$resumeTask = $definitions[1]
Assert ($task.Triggers.Count -eq 1 -and $resumeTask.Triggers.Count -eq 1) 'Expected one trigger on each task.'
$logon = @($task.Triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskLogonTrigger' })
$wake = @($resumeTask.Triggers | Where-Object { $_.CimClass.CimClassName -eq 'MSFT_TaskEventTrigger' })
Assert ($logon.Count -eq 1 -and $wake.Count -eq 1) 'Wrong trigger types.'
Assert ($logon[0].Delay -eq 'PT15S' -and $wake[0].Delay -eq 'PT15S') 'Wrong delay.'
Assert ($task.Principal.RunLevel -eq 'Highest' -and $task.Principal.LogonType -eq 'Interactive') 'Wrong desktop/elevation settings.'
Assert ($task.Settings.MultipleInstances -eq 'IgnoreNew' -and $resumeTask.Settings.MultipleInstances -eq 'IgnoreNew') 'Concurrent window-manager invocations should coalesce.'
Assert ($task.Settings.ExecutionTimeLimit -eq 'PT0S' -and -not $task.Settings.WakeToRun) 'Task should neither time out authentication nor wake the PC itself.'
Assert ($task.Actions[0].Arguments.Contains('-WindowStyle Hidden -File "') -and $task.Actions[0].Arguments.Contains('Open-SCNUNET.ps1')) 'The short-lived window manager should be hidden.'
Assert (-not $task.Actions[0].Arguments.EndsWith(' -Resume') -and $resumeTask.Actions[0].Arguments.EndsWith(' -Resume')) 'Only wake should retire the previous session.'
Assert ($resumeTask.Principal.RunLevel -eq 'Highest' -and $resumeTask.Principal.LogonType -eq 'Interactive') 'Resume task must run on the same elevated interactive desktop.'
$query = [xml]$wake[0].Subscription
$select = [string]$query.QueryList.Query.Select.'#text'
Assert ($select -eq "*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1]]") 'Wrong wake subscription.'
# Only queries past Windows events; never fabricates an event or sleeps the PC.
$event = Get-WinEvent -FilterXml $wake[0].Subscription -MaxEvents 1 -ErrorAction SilentlyContinue
if ($event) { Assert ($event.Id -eq 1 -and $event.ProviderName -eq 'Microsoft-Windows-Power-Troubleshooter') 'Unexpected matching event.' }
Write-Host 'PASS: two task definitions, desktop reuse vs resume handoff, interactive desktop, delay, wake event query; no task registered or started.'

$path = Join-Path $OutputDirectory 'test connection.lnk'
& $shortcutInstaller -ShortcutPath $path -ValidateOnly
Assert (-not (Test-Path -LiteralPath $path)) 'Validation wrote a shortcut.'
& $shortcutInstaller -ShortcutPath $path -WhatIf
Assert (-not (Test-Path -LiteralPath $path)) 'WhatIf wrote a shortcut.'
& $shortcutInstaller -ShortcutPath $path
& $shortcutInstaller -ShortcutPath $path
$shell = New-Object -ComObject WScript.Shell
$shortcut = $shell.CreateShortcut($path)
try {
    Assert ($shortcut.TargetPath -ieq "$env:SystemRoot\System32\schtasks.exe") 'Wrong shortcut target.'
    Assert ($shortcut.Arguments -ceq '/Run /TN "\MiniEAP SCNUNET Startup"') 'Wrong task command quoting.'
    Assert ($shortcut.WindowStyle -eq 7) 'The transient schtasks window should be minimized.'
    $shortcut.TargetPath = "$env:SystemRoot\System32\notepad.exe"
    $shortcut.Save()
    $rejected = $false
    try { & $shortcutInstaller -ShortcutPath $path } catch { $rejected = $true }
    Assert $rejected 'An unrelated existing shortcut was overwritten.'
} finally {
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut)
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
}
Write-Host 'PASS: shortcut validation, WhatIf, exact task target, repeat install, unrelated shortcut protection; no shortcut launched.'
