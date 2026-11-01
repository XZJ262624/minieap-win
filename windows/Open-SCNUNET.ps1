[CmdletBinding()]
param(
    [switch]$Resume,
    [switch]$ValidateOnly,
    [string]$LauncherPath,
    [string]$MiniEapPath = 'F:\Tools\MiniEAP\minieap-win.exe',
    [string]$ConfigPath = 'F:\Tools\MiniEAP\minieap.conf',
    [string]$PidFilePath = 'F:\Tools\MiniEAP\minieap-i226v.pid',
    # Internal helper mode runs in a detached process, never the caller's console.
    [int]$ConsoleProcessId,
    [long]$ConsoleStartTicks,
    [switch]$StopConsole,
    [switch]$ClosePrompt,
    [ValidateSet('Normal', 'Hidden')][string]$ConnectionWindowStyle = 'Normal',
    [switch]$NoErrorWindow
)
$ErrorActionPreference = 'Stop'
if (($StopConsole -or $ClosePrompt) -and $ConsoleProcessId -le 0) { throw '控制台操作缺少有效目标。' }
if (-not $LauncherPath) { $LauncherPath = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'Start-SCNUNET.ps1' }
foreach ($path in @($LauncherPath, $MiniEapPath, $ConfigPath, $PidFilePath)) {
    if (-not [IO.Path]::IsPathRooted($path) -or $path.Contains('"')) { throw '启动路径必须是有效绝对路径。' }
}
$LauncherPath = [IO.Path]::GetFullPath($LauncherPath)
$MiniEapPath = [IO.Path]::GetFullPath($MiniEapPath)
$ConfigPath = [IO.Path]::GetFullPath($ConfigPath)
$PidFilePath = [IO.Path]::GetFullPath($PidFilePath)
if (-not (Test-Path -LiteralPath $LauncherPath -PathType Leaf)) { throw '未找到连接启动脚本。' }
if ($ValidateOnly) {
    Write-Host '窗口管理入口检查通过；未启动或停止任何进程。'
    exit 0
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class ScnunetConsole {
    [DllImport("shell32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CommandLineToArgvW(string text, out int count);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr ptr);
    [DllImport("kernel32.dll")] static extern bool FreeConsole();
    [DllImport("kernel32.dll")] static extern bool AttachConsole(uint pid);
    [DllImport("kernel32.dll")] static extern uint GetConsoleProcessList([Out] uint[] ids, uint count);
    [DllImport("kernel32.dll")] static extern bool SetConsoleCtrlHandler(IntPtr handler, bool add);
    [DllImport("kernel32.dll")] static extern bool GenerateConsoleCtrlEvent(uint type, uint group);
    [DllImport("kernel32.dll")] static extern IntPtr GetConsoleWindow();
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern IntPtr CreateFileW(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    [StructLayout(LayoutKind.Explicit, Size=20)] struct KeyRecord {
        [FieldOffset(0)] public ushort Type;
        [FieldOffset(4)] public int Down;
        [FieldOffset(8)] public ushort Repeat;
        [FieldOffset(10)] public ushort VirtualKey;
        [FieldOffset(14)] public char Character;
    }
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern bool WriteConsoleInputW(IntPtr input, KeyRecord[] records, uint count, out uint written);
    [DllImport("user32.dll")] static extern IntPtr GetAncestor(IntPtr window, uint flags);
    [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    public static string[] Parse(string text) {
        int count; IntPtr ptr = CommandLineToArgvW(text, out count);
        if (ptr == IntPtr.Zero) return new string[0];
        try {
            string[] result = new string[count];
            for (int i=0;i<count;i++) result[i]=Marshal.PtrToStringUni(Marshal.ReadIntPtr(ptr,i*IntPtr.Size));
            return result;
        } finally { LocalFree(ptr); }
    }
    public static bool Control(uint owner, uint[] allowed, int mode) {
        FreeConsole();
        if (!AttachConsole(owner)) return false;
        try {
            // A Ctrl+C broadcast reaches EVERY attached process. Refuse if this
            // console contains anything except the verified launcher, its MiniEAP
            // child, and this detached helper. Never signal a generic terminal.
            uint[] ids=new uint[64]; uint count=GetConsoleProcessList(ids,64);
            if(count==0 || count>64 || Array.IndexOf(ids,owner)<0) return false;
            for(int i=0;i<count;i++) if(Array.IndexOf(allowed,ids[i])<0) return false;
            if(mode==1) {
                if(!SetConsoleCtrlHandler(IntPtr.Zero,true)) return false;
                bool sent=GenerateConsoleCtrlEvent(0,0);
                System.Threading.Thread.Sleep(100);
                return sent;
            }
            if(mode==2) {
                // Only after the native child has exited: N cancels an initial
                // confirmation and also dismisses a legacy final Read-Host.
                IntPtr input=CreateFileW("CONIN$",0x40000000,3,IntPtr.Zero,3,0,IntPtr.Zero);
                if(input==new IntPtr(-1)) return false;
                try {
                    KeyRecord[] keys=new KeyRecord[4];
                    for(int i=0;i<4;i++) {keys[i].Type=1;keys[i].Down=i%2==0?1:0;keys[i].Repeat=1;keys[i].VirtualKey=(ushort)(i<2?0x4e:13);keys[i].Character=i<2?'N':'\r';}
                    uint written; return WriteConsoleInputW(input,keys,4,out written) && written==4;
                } finally {CloseHandle(input);}
            }
            IntPtr window=GetConsoleWindow();
            IntPtr root=GetAncestor(window,3);
            if(root!=IntPtr.Zero) window=root;
            if(window!=IntPtr.Zero) { ShowWindowAsync(window,9); SetForegroundWindow(window); }
            return true; // Foreground policy may flash the taskbar instead.
        } finally { FreeConsole(); }
    }
}
'@

function Get-ArgumentPath($Tokens, [string]$Name, [string]$Default) {
    for ($i=0; $i -lt $Tokens.Count-1; $i++) {
        if ($Tokens[$i] -ieq $Name) { return [IO.Path]::GetFullPath($Tokens[$i+1]) }
    }
    return [IO.Path]::GetFullPath($Default)
}
function Get-OwnedLaunchers {
    $session = (Get-Process -Id $PID).SessionId
    $processes = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe' OR Name='pwsh.exe'")
    foreach ($process in $processes) {
        if ($process.SessionId -ne $session -or -not $process.CommandLine) { continue }
        $tokens = [ScnunetConsole]::Parse($process.CommandLine)
        if ($tokens -icontains '-NoExit' -or $tokens -icontains '-Command' -or $tokens -icontains '-EncodedCommand') { continue }
        $index = -1
        for ($i=0; $i -lt $tokens.Count; $i++) { if ($tokens[$i] -ieq '-File') { $index=$i; break } }
        if ($index -lt 0 -or $index+1 -ge $tokens.Count) { continue }
        if ([IO.Path]::GetFullPath($tokens[$index+1]) -ine $LauncherPath) { continue }
        if ((Get-ArgumentPath $tokens '-MiniEapPath' 'F:\Tools\MiniEAP\minieap-win.exe') -ine $MiniEapPath -or
            (Get-ArgumentPath $tokens '-ConfigPath' 'F:\Tools\MiniEAP\minieap.conf') -ine $ConfigPath -or
            (Get-ArgumentPath $tokens '-PidFilePath' 'F:\Tools\MiniEAP\minieap-i226v.pid') -ine $PidFilePath) { continue }
        $live = Get-Process -Id $process.ProcessId -ErrorAction SilentlyContinue
        if ($live) { $live }
    }
}
function Get-OwnedChildren($Owner) {
    @(Get-CimInstance Win32_Process -Filter "ParentProcessId=$($Owner.Id)" | Where-Object {
        $_.ExecutablePath -ieq $MiniEapPath -and $_.SessionId -eq $Owner.SessionId -and $_.CreationDate -ge $Owner.StartTime
    } | ForEach-Object { Get-Process -Id $_.ProcessId -ErrorAction SilentlyContinue })
}

if ($ConsoleProcessId) {
    $owner = @(Get-OwnedLaunchers | Where-Object { $_.Id -eq $ConsoleProcessId -and $_.StartTime.ToUniversalTime().Ticks -eq $ConsoleStartTicks })
    if ($owner.Count -ne 1) { exit 3 }
    $children = @(Get-OwnedChildren $owner[0])
    if ($ClosePrompt -and $children.Count) { exit 4 }
    $allowed = @([uint32]$PID, [uint32]$owner[0].Id) + @($children | ForEach-Object { [uint32]$_.Id })
    $mode = if ($StopConsole) { 1 } elseif ($ClosePrompt) { 2 } else { 0 }
    if ([ScnunetConsole]::Control($ConsoleProcessId, [uint32[]]$allowed, $mode)) { exit 0 }
    exit 4
}

$powerShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
$common = '-LauncherPath "{0}" -MiniEapPath "{1}" -ConfigPath "{2}" -PidFilePath "{3}"' -f $LauncherPath,$MiniEapPath,$ConfigPath,$PidFilePath
function Invoke-ConsoleHelper($Owner, [switch]$Stop, [switch]$Dismiss) {
    $info = New-Object Diagnostics.ProcessStartInfo
    $info.FileName = $powerShell
    $info.Arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}" {1} -ConsoleProcessId {2} -ConsoleStartTicks {3}' -f $PSCommandPath,$common,$Owner.Id,$Owner.StartTime.ToUniversalTime().Ticks
    if ($Stop) { $info.Arguments += ' -StopConsole' }
    if ($Dismiss) { $info.Arguments += ' -ClosePrompt' }
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $child = [Diagnostics.Process]::Start($info)
    try {
        if (-not $child.WaitForExit(10000)) { throw '窗口控制辅助程序未及时完成；未启动新认证。' }
        if ($child.ExitCode -ne 0 -and -not $Owner.HasExited) { throw '无法确认旧窗口的安全归属；请检查原窗口。' }
    } finally { $child.Dispose() }
}

$hash = [Security.Cryptography.SHA256]::Create()
$key = [BitConverter]::ToString($hash.ComputeHash([Text.Encoding]::UTF8.GetBytes(($LauncherPath+'|'+$ConfigPath).ToUpperInvariant()))).Replace('-','')
$hash.Dispose()
$mutex = New-Object Threading.Mutex($false, ('Local\SCNUNET-Window-' + $key))
$locked = $false
try {
    $waitMilliseconds = if ($Resume) { 15000 } else { 0 }
    try { $locked = $mutex.WaitOne($waitMilliseconds) } catch [Threading.AbandonedMutexException] { $locked = $true }
    if (-not $locked) { exit 0 }
    $owners = @(Get-OwnedLaunchers | Sort-Object StartTime)
    if (-not $Resume -and $owners.Count) {
        Invoke-ConsoleHelper $owners[0]
        exit 0
    }
    if ($Resume) {
        foreach ($owner in $owners) {
            $children = @(Get-OwnedChildren $owner)
            if ($children.Count) { Invoke-ConsoleHelper $owner -Stop }
            foreach ($child in $children) {
                if (-not $child.WaitForExit(5000)) { throw '旧 MiniEAP 未正常退出；为避免重复认证，已取消打开新窗口。' }
            }
            # With no native child left, N safely closes either a pending
            # initial confirmation or the legacy final Read-Host.
            if (-not $owner.WaitForExit(500)) { Invoke-ConsoleHelper $owner -Dismiss }
            if (-not $owner.WaitForExit(3000)) { throw '旧连接窗口仍未退出；已取消打开新窗口。' }
        }
    }
    $arguments = '-NoLogo -NoProfile -ExecutionPolicy Bypass -File "{0}" -MiniEapPath "{1}" -ConfigPath "{2}" -PidFilePath "{3}"' -f $LauncherPath,$MiniEapPath,$ConfigPath,$PidFilePath
    # This is the user-requested visible connection window. Native authentication
    # still uses the original direct & invocation inside Start-SCNUNET.ps1.
    $window = Start-Process -FilePath $powerShell -ArgumentList $arguments -WindowStyle $ConnectionWindowStyle -PassThru
    # Keep the cross-task mutex until the new command line is discoverable.
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    while (-not $window.HasExited -and [DateTime]::UtcNow -lt $deadline) {
        if (@(Get-OwnedLaunchers | Where-Object Id -eq $window.Id).Count) { break }
        Start-Sleep -Milliseconds 100
    }
} catch {
    # A hidden broker must surface failure without silently starting a duplicate.
    # Only fixed text is passed; neither command lines nor credentials are shown.
    $errorCode = "Write-Host '无法安全切换 SCNUNET 旧窗口，本次未启动新连接。请查看原窗口后重试。' -ForegroundColor Red; Read-Host '按 Enter 关闭窗口'; exit 1"
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($errorCode))
    if (-not $NoErrorWindow) { Start-Process -FilePath $powerShell -ArgumentList "-NoProfile -EncodedCommand $encoded" -WindowStyle Normal | Out-Null }
    Write-Error $_
    exit 1
} finally {
    if ($locked) { $mutex.ReleaseMutex() }
    $mutex.Dispose()
}
