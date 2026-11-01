[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'Medium')]
param(
    [string]$TaskName = 'MiniEAP SCNUNET Startup',
    [switch]$RemoveUnrecognized,
    [switch]$ValidateOnly
)

$ErrorActionPreference = 'Stop'
$taskPath = '\'
$managedDescription = 'Interactive SCNUNET launcher managed by Install-SCNUNETStartupTask.ps1.'

function Test-IsAdministrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    return $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-IsManagedTask {
    param([Microsoft.Management.Infrastructure.CimInstance]$Task)

    if ($Task.Description -eq $managedDescription) {
        return $true
    }
    foreach ($taskAction in @($Task.Actions)) {
        if ($taskAction.Arguments -like '*Start-SCNUNET.ps1*') {
            return $true
        }
    }
    return $false
}

if (-not $IsWindows) {
    if ($env:OS -ne 'Windows_NT') {
        throw '此卸载脚本只能在 Windows 上运行。'
    }
}

if ($ValidateOnly) {
    Write-Host '卸载脚本检查通过；未删除计划任务。'
    Write-Host "目标任务：$taskPath$TaskName / $taskPath$TaskName Resume"
    exit 0
}

if (-not (Test-IsAdministrator)) {
    throw '卸载最高权限计划任务需要管理员 PowerShell。请以管理员身份重新运行此脚本。'
}

$names = @($TaskName, "$TaskName Resume")
$existingTasks = @($names | ForEach-Object { Get-ScheduledTask -TaskName $_ -TaskPath $taskPath -ErrorAction SilentlyContinue })
foreach ($task in $existingTasks) {
    if (-not (Test-IsManagedTask -Task $task) -and -not $RemoveUnrecognized) {
        throw "任务 '$($task.TaskName)' 并非本脚本管理，已保留。若确认需要删除，请显式使用 -RemoveUnrecognized。"
    }
}
foreach ($task in $existingTasks) {
    if ($PSCmdlet.ShouldProcess("$taskPath$($task.TaskName)", '卸载 SCNUNET 窗口管理任务')) {
        Unregister-ScheduledTask -TaskName $task.TaskName -TaskPath $taskPath -Confirm:$false
        Write-Host "已卸载计划任务：$taskPath$($task.TaskName)" -ForegroundColor Green
    }
}
Write-Host '任务检查完成；启动脚本、MiniEAP 程序、配置文件和当前连接均未删除或停止。'
