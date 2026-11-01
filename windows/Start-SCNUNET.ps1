[CmdletBinding()]
param(
    [switch]$ValidateOnly,
    [string]$MiniEapPath = 'F:\Tools\MiniEAP\minieap-win.exe',
    [string]$ConfigPath = 'F:\Tools\MiniEAP\minieap.conf',
    [string]$PidFilePath = 'F:\Tools\MiniEAP\minieap-i226v.pid'
)

$ErrorActionPreference = 'Stop'

# PowerShell 7 can otherwise turn a native nonzero exit into a catchable error
# before we report its real exit code. This variable is inert in Windows PS 5.1.
$PSNativeCommandUseErrorActionPreference = $false

function Test-LauncherPrerequisites {
    if (-not [IO.Path]::IsPathRooted($MiniEapPath) -or
        -not [IO.Path]::IsPathRooted($ConfigPath) -or
        -not [IO.Path]::IsPathRooted($PidFilePath)) {
        throw 'EXE、配置和 PID 文件必须使用绝对路径。'
    }
    if (-not (Test-Path -LiteralPath $miniEapPath -PathType Leaf)) {
        throw "未找到 MiniEAP：$miniEapPath"
    }
    if (-not (Test-Path -LiteralPath $configPath -PathType Leaf)) {
        throw "未找到配置文件：$configPath"
    }
    if (-not (Test-Path -LiteralPath (Split-Path -Parent $PidFilePath) -PathType Container)) {
        throw 'PID 文件所在目录不存在。'
    }
}

try {
    Test-LauncherPrerequisites

    if ($ValidateOnly) {
        Write-Host '启动脚本路径检查通过；未读取配置内容，也未启动 MiniEAP。运行时将启用 PID 文件独占。'
        exit 0
    }

    while ($true) {
        $answer = Read-Host '是否连接 SCNUNET？[Y/n]'
        if ([string]::IsNullOrWhiteSpace($answer) -or $answer -match '^[Yy]$') {
            break
        }
        if ($answer -match '^[Nn]$') {
            Write-Host '本次不启动 MiniEAP。'
            exit 0
        }
        Write-Host '请输入 Y、N，或直接按 Enter。' -ForegroundColor Yellow
    }

    # Also protect a legacy instance that started with pid-file=none. Do not
    # inspect its command line (which could contain credentials), or stop it.
    # The native PID file lock arbitrates simultaneous new launches atomically.
    $processName = [IO.Path]::GetFileNameWithoutExtension($MiniEapPath)
    $existing = @(Get-Process -Name $processName -ErrorAction SilentlyContinue |
        Where-Object { -not $_.HasExited })
    if ($existing.Count -gt 0) {
        Write-Host '已有 MiniEAP 实例正在运行，本次跳过；现有连接保持不变。' -ForegroundColor Yellow
        [void](Read-Host '按 Enter 关闭窗口')
        exit 0
    }

    Write-Host '正在启动 MiniEAP；按 Ctrl+C 可结束认证程序。' -ForegroundColor Cyan

    # Call the native executable directly so all authentication and Keep-Alive
    # output remains attached to this visible PowerShell window.
    Push-Location -LiteralPath (Split-Path -Parent $miniEapPath)
    try {
        & $miniEapPath '--conf-file' $configPath '--pid-file' $PidFilePath
        $miniEapExitCode = $LASTEXITCODE
    }
    finally {
        Pop-Location
    }

    if ($miniEapExitCode -eq 0) {
        Write-Host 'MiniEAP 已退出。' -ForegroundColor Cyan
    }
    else {
        $hexExitCode = [BitConverter]::ToUInt32([BitConverter]::GetBytes([int]$miniEapExitCode), 0).ToString('X8')
        Write-Host "MiniEAP 已退出，返回码：$miniEapExitCode (0x$hexExitCode)" -ForegroundColor Red
    }

    [void](Read-Host '按 Enter 关闭窗口')
    exit $miniEapExitCode
}
catch {
    Write-Host "SCNUNET 启动失败：$($_.Exception.Message)" -ForegroundColor Red
    if (-not $ValidateOnly) {
        [void](Read-Host '按 Enter 关闭窗口')
    }
    exit 1
}
