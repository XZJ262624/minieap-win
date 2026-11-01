[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'Medium')]
param(
    [string]$TaskName = 'MiniEAP SCNUNET Startup',
    [string]$LauncherPath,
    [ValidateRange(0, 3600)]
    [int]$DelaySeconds = 15,
    [switch]$ReplaceExisting,
    [switch]$ValidateOnly,
    [switch]$PassThru
)

$ErrorActionPreference = 'Stop'
$taskPath = '\'
$managedDescription = 'Interactive SCNUNET launcher managed by Install-SCNUNETStartupTask.ps1.'
$scriptDirectory = Split-Path -Parent $MyInvocation.MyCommand.Path

if ([string]::IsNullOrWhiteSpace($LauncherPath)) {
    $LauncherPath = Join-Path $scriptDirectory 'Start-SCNUNET.ps1'
}

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
    # Windows PowerShell 5.1 does not define $IsWindows.
    if ($env:OS -ne 'Windows_NT') {
        throw '此安装脚本只能在 Windows 上运行。'
    }
}

$resolvedLauncherPath = (Resolve-Path -LiteralPath $LauncherPath).Path
if ([IO.Path]::GetExtension($resolvedLauncherPath) -ine '.ps1') {
    throw 'LauncherPath 必须指向 PowerShell 脚本。'
}
$windowManagerPath = Join-Path (Split-Path -Parent $resolvedLauncherPath) 'Open-SCNUNET.ps1'
if (-not (Test-Path -LiteralPath $windowManagerPath -PathType Leaf)) { throw '缺少 Open-SCNUNET.ps1，请复制完整 windows 脚本目录。' }
$resumeTaskName = "$TaskName Resume"

$interactiveUser = $null
try {
    $interactiveUser = (Get-CimInstance -ClassName Win32_ComputerSystem -ErrorAction Stop).UserName
}
catch {
    if ([Environment]::UserInteractive) {
        $interactiveUser = [Security.Principal.WindowsIdentity]::GetCurrent().Name
    }
    else {
        throw '无法确定当前交互式 Windows 用户。'
    }
}
if ([string]::IsNullOrWhiteSpace($interactiveUser)) {
    throw '当前没有可用于交互式登录任务的 Windows 用户。'
}

$windowsPowerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
if (-not (Test-Path -LiteralPath $windowsPowerShell -PathType Leaf)) {
    throw "未找到 Windows PowerShell：$windowsPowerShell"
}

$actionArguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{0}" -LauncherPath "{1}"' -f $windowManagerPath,$resolvedLauncherPath
$action = New-ScheduledTaskAction `
    -Execute $windowsPowerShell `
    -Argument $actionArguments `
    -WorkingDirectory (Split-Path -Parent $resolvedLauncherPath)
$resumeAction = New-ScheduledTaskAction -Execute $windowsPowerShell -Argument ($actionArguments + ' -Resume') -WorkingDirectory (Split-Path -Parent $resolvedLauncherPath)

$trigger = New-ScheduledTaskTrigger -AtLogOn -User $interactiveUser
$trigger.Delay = 'PT{0}S' -f $DelaySeconds

# S3 sleep/hibernate resume is not a new user logon. This provider/event is
# emitted by Windows when this machine resumes; the interactive token keeps
# the prompt on the signed-in user's desktop (visible after unlocking).
$wakeSubscription = @'
<QueryList><Query Id="0" Path="System"><Select Path="System">*[System[Provider[@Name='Microsoft-Windows-Power-Troubleshooter'] and EventID=1]]</Select></Query></QueryList>
'@
$wakeClass = Get-CimClass -Namespace 'Root/Microsoft/Windows/TaskScheduler' -ClassName 'MSFT_TaskEventTrigger'
$wakeTrigger = New-CimInstance `
    -CimClass $wakeClass `
    -ClientOnly `
    -Property @{ Enabled = $true; Subscription = $wakeSubscription; Delay = ('PT{0}S' -f $DelaySeconds) }

$principal = New-ScheduledTaskPrincipal `
    -UserId $interactiveUser `
    -LogonType Interactive `
    -RunLevel Highest

$settings = New-ScheduledTaskSettingsSet `
    -AllowStartIfOnBatteries `
    -DontStopIfGoingOnBatteries `
    -StartWhenAvailable `
    -MultipleInstances IgnoreNew `
    -ExecutionTimeLimit ([TimeSpan]::Zero)

$taskDefinition = New-ScheduledTask `
    -Action $action `
    -Trigger $trigger `
    -Principal $principal `
    -Settings $settings `
    -Description $managedDescription
$resumeDefinition = New-ScheduledTask -Action $resumeAction -Trigger $wakeTrigger -Principal $principal -Settings $settings -Description $managedDescription

if ($ValidateOnly) {
    Write-Host '任务定义检查通过；未创建或修改计划任务。'
    Write-Host "任务名称：$TaskName"
    Write-Host "唤醒任务：$resumeTaskName"
    Write-Host "执行程序：$($action.Execute)"
    Write-Host "执行参数：$($action.Arguments)"
    Write-Host "工作目录：$($action.WorkingDirectory)"
    Write-Host "交互用户：$($principal.UserId)"
    Write-Host "登录延迟：$($trigger.Delay)"
    Write-Host "唤醒触发：System / Microsoft-Windows-Power-Troubleshooter / 1；延迟 $($wakeTrigger.Delay)"
    Write-Host "运行级别：$($principal.RunLevel)"
    Write-Host "登录类型：$($principal.LogonType)"
    Write-Host "多实例策略：$($settings.MultipleInstances)"
    Write-Host "运行时限：$($settings.ExecutionTimeLimit)"
    if ($PassThru) { $taskDefinition; $resumeDefinition }
    return
}

if (-not (Test-IsAdministrator)) {
    throw '安装最高权限计划任务需要管理员 PowerShell。请以管理员身份重新运行此脚本。'
}

foreach ($name in @($TaskName, $resumeTaskName)) {
    $existingTask = Get-ScheduledTask -TaskName $name -TaskPath $taskPath -ErrorAction SilentlyContinue
    if ($null -ne $existingTask -and -not (Test-IsManagedTask -Task $existingTask) -and -not $ReplaceExisting) {
        throw "已存在同名但并非本脚本管理的任务 '$name'。如确认需要替换，请显式使用 -ReplaceExisting。"
    }
}

if ($PSCmdlet.ShouldProcess("$TaskName / $resumeTaskName", '安装或更新 SCNUNET 窗口管理任务')) {
    Register-ScheduledTask `
        -TaskName $TaskName `
        -TaskPath $taskPath `
        -InputObject $taskDefinition `
        -Force | Out-Null
    Register-ScheduledTask -TaskName $resumeTaskName -TaskPath $taskPath -InputObject $resumeDefinition -Force | Out-Null

    Write-Host "安装或更新完成：$TaskName / $resumeTaskName" -ForegroundColor Green
    Write-Host "用户 $interactiveUser 下次登录或睡眠唤醒约 $DelaySeconds 秒后会看到连接确认窗口。"
    Write-Host '桌面入口复用已有窗口；唤醒时正常结束受管理的旧会话，再打开一个确认窗口。'
    if ($PassThru) { Get-ScheduledTask -TaskName $TaskName -TaskPath $taskPath; Get-ScheduledTask -TaskName $resumeTaskName -TaskPath $taskPath }
}
