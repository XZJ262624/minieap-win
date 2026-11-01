[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'Medium')]
param(
    [string]$TaskName = 'MiniEAP SCNUNET Startup',
    [string]$ShortcutPath = (Join-Path ([Environment]::GetFolderPath('Desktop')) '连接 SCNUNET.lnk'),
    [switch]$ReplaceExisting,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
if ($env:OS -ne 'Windows_NT') { throw '此脚本只能在 Windows 上运行。' }
if ([string]::IsNullOrWhiteSpace($TaskName) -or $TaskName -match '["\\/\r\n]') {
    throw 'TaskName 必须是根目录下的有效任务名。'
}
if (-not [IO.Path]::IsPathRooted($ShortcutPath) -or [IO.Path]::GetExtension($ShortcutPath) -ine '.lnk') {
    throw 'ShortcutPath 必须是 .lnk 文件的绝对路径。'
}
if (-not (Test-Path -LiteralPath (Split-Path -Parent $ShortcutPath) -PathType Container)) {
    throw '快捷方式所在目录不存在。'
}
$task = Get-ScheduledTask -TaskName $TaskName -TaskPath '\' -ErrorAction Stop
if ($task.Description -ne 'Interactive SCNUNET launcher managed by Install-SCNUNETStartupTask.ps1.') {
    throw '目标任务并非本项目管理的连接确认任务，请先安装或更新任务。'
}
# Run the pre-registered interactive/highest task. No credentials, UAC bypass,
# credential storage, or embedded PowerShell command is needed in the shortcut.
$target = Join-Path $env:SystemRoot 'System32\schtasks.exe'
$arguments = '/Run /TN "\{0}"' -f $TaskName
$shell = New-Object -ComObject WScript.Shell
try {
    $shortcut = $shell.CreateShortcut($ShortcutPath)
    if ((Test-Path -LiteralPath $ShortcutPath) -and -not $ReplaceExisting -and
        ($shortcut.TargetPath -ine $target -or $shortcut.Arguments -cne $arguments)) {
        throw '已有同名快捷方式指向其他内容，已保留。确认替换后可指定 -ReplaceExisting。'
    }
    if ($ValidateOnly) {
        Write-Host "快捷方式检查通过：$ShortcutPath；未创建文件或启动任务。"
        return
    }
    if ($PSCmdlet.ShouldProcess($ShortcutPath, '创建或更新 SCNUNET 桌面连接快捷方式')) {
        $shortcut.TargetPath = $target
        $shortcut.Arguments = $arguments
        $shortcut.WorkingDirectory = Split-Path -Parent $target
        $shortcut.IconLocation = (Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe') + ',0'
        $shortcut.WindowStyle = 7
        $shortcut.Description = '打开 SCNUNET 连接确认窗口；Enter/Y 连接，N 跳过。'
        $shortcut.Save()
        Write-Host "已创建：$ShortcutPath" -ForegroundColor Green
    }
} finally {
    if ($null -ne $shortcut) { [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) }
    [void][Runtime.InteropServices.Marshal]::FinalReleaseComObject($shell)
}
