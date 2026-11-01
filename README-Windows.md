# MiniEAP on Windows 11 x64

本工程已接入完整 MiniEAP、EAP-MD5、RJv3 和 printer 插件。Windows 仅注册 `libpcap` 网络模块，通过 Npcap 收发 EAPOL；不编译 Linux raw socket 或 BSD BPF 后端。

## 编译

需要 Visual Studio 2022 的 C++ 桌面开发组件、MSVC v143、Windows SDK，以及已安装的 Npcap Runtime。仓库的 `third_party/npcap-sdk` 提供头文件和 x64 导入库。

在项目目录的 PowerShell 中执行：

```powershell
.\build-windows.ps1 -Configuration Debug -Rebuild
```

或在 Visual Studio 打开 `minieap-win.sln`，选择 **Debug / x64** 后重新生成解决方案。输出为 `x64\Debug\minieap-win.exe`，日志为 `port-validation\Debug-x64-build.log`。Release 可用同一脚本指定 `-Configuration Release`。

真正入口是 `src/minieap/minieap.c`。根目录原有 `main.c` 是早期网卡枚举样例，保留作为参考，不参与构建。不要将它再次加入编译，否则会出现重复 main。

工程使用 C11、UTF-8 源码、ANSI `getopt_long` 和 x64 Npcap 库。Npcap DLL 延迟加载，启动网络功能时优先从 Windows 系统目录的 `Npcap` 子目录加载，兼容系统目录中的安装方式。无需将 SDK 或 DLL 路径写进全局 PATH。

## 选择 Intel I226-V

```powershell
.\x64\Debug\minieap-win.exe --list-interfaces
```

找到目标网卡的描述，例如 `Intel(R) Ethernet Controller I226-V`。复制列表中该网卡的完整设备名；下面的 `YOUR-ADAPTER-GUID` 必须替换为本机枚举结果：

```text
\Device\NPF_{YOUR-ADAPTER-GUID}
```

将完整设备名传给 `-n` / `--nic`。列表序号和 `以太网` 友好名称仅用于辨认，不是 `-n` 的参数。PowerShell 单引号中的反斜杠不需要再次转义。重新安装网卡后 GUID 可能变化，应重新枚举。

先执行不发送认证帧的检查：

```powershell
.\x64\Debug\minieap-win.exe --check-interface `
  -n '\Device\NPF_{YOUR-ADAPTER-GUID}' `
  --if-impl libpcap --module rjv3 --service students
```

该命令检查参数、设备打开、Ethernet 链路类型、EAPOL `0x888e` 过滤器以及 MAC 读取，然后关闭设备。它不需要账号密码，也不会进入认证或发送报文。如果 Npcap 安装时限制为管理员访问，请从管理员 PowerShell 运行。

## 认证

```powershell
.\x64\Debug\minieap-win.exe `
  -u '你的学号' -p '你的密码' `
  -n '\Device\NPF_{YOUR-ADAPTER-GUID}' `
  --if-impl libpcap --module rjv3 --service students -b 0
```

使用空格分隔选项和参数，避免 `-u用户名` 这样的连写形式。程序前台运行，按 **Ctrl+C** 或 **Ctrl+Break** 请求退出；抓包循环会在主线程完成清理。Windows 第一版不支持 daemon、systemd、`--kill` 或代理认证，相关参数会明确报错。

也可复制 `minieap.windows.conf.example` 为工作目录下的 `minieap.conf`，填写账号密码后运行 EXE。默认配置文件可省略；指定其他配置文件可用 `--conf-file '路径'` 或 `--conf-file='路径'`。命令行覆盖配置文件；命令行出现 `--module` 时替换配置中的插件列表。`--save` 会将包括密码的参数以明文写入配置文件。

程序的内置 PID 默认值仍为 `none`，但 Windows 配置示例和日常配置应使用 `pid-file=F:\Tools\MiniEAP\minieap-i226v.pid`。Windows 会在进程存活期间独占文件并在退出时删除；不同实例必须使用同一路径才能互斥。文件里留有旧 PID 并不代表锁仍被占用，实际互斥依靠文件句柄。不要手工删除正在使用的 PID 文件。

RJv3 保留原有 DHCP 模式和 `--dhcp-script`，不会自动为 Windows 选择续租命令。常规运行先使用上述参数；若学校要求二次认证，需要按实际网络要求配置 DHCP 模式和 Windows 命令。

## 桌面、Windows 登录及睡眠唤醒时连接 SCNUNET

仓库的 `windows` 目录提供五个脚本：

- `Start-SCNUNET.ps1`：显示 `是否连接 SCNUNET？[Y/n]`，并在用户确认后启动 MiniEAP。
- `Open-SCNUNET.ps1`：管理窗口；正常打开时复用已有窗口，睡眠恢复时正常结束旧会话后打开一个新确认窗口。
- `Install-SCNUNETStartupTask.ps1`：为当前登录用户安装登录/桌面入口和睡眠恢复两个交互式计划任务。
- `Install-SCNUNETDesktopShortcut.ps1`：在当前用户桌面创建“连接 SCNUNET”快捷方式。
- `Uninstall-SCNUNETStartupTask.ps1`：卸载该计划任务，不删除程序、配置或脚本。

启动脚本固定使用：

```text
F:\Tools\MiniEAP\minieap-win.exe
F:\Tools\MiniEAP\minieap.conf
F:\Tools\MiniEAP\minieap-i226v.pid
```

首次安装时，将编译好的 EXE、配置文件和五个脚本放入 `F:\Tools\MiniEAP`。更新已有 EXE 前，应等到可以断开网络时，在原认证窗口按 Ctrl+C 正常退出。不要在在线期间替换 EXE 或启动第二次认证。配置文件只保存在本机，不要提交到 Git：

```powershell
Copy-Item .\x64\Release\minieap-win.exe F:\Tools\MiniEAP\
Copy-Item .\windows\*.ps1 F:\Tools\MiniEAP\
# 单独准备 F:\Tools\MiniEAP\minieap.conf，不要在命令行或脚本中写入凭据。
# 配置中设置 pid-file=F:\Tools\MiniEAP\minieap-i226v.pid
```

安装前可进行无副作用检查。第一条检查 EXE、配置文件和 PID 目录是否存在，不读取配置或启动程序；第二条构造并检查计划任务定义，不注册任务：

```powershell
& 'F:\Tools\MiniEAP\Start-SCNUNET.ps1' -ValidateOnly
& 'F:\Tools\MiniEAP\Open-SCNUNET.ps1' -ValidateOnly
& 'F:\Tools\MiniEAP\Install-SCNUNETStartupTask.ps1' -ValidateOnly
```

先在管理员 PowerShell 中手动运行启动器，确认认证成功并持续 Keep-Alive：

```powershell
& 'F:\Tools\MiniEAP\Start-SCNUNET.ps1'
```

确认手动启动稳定后，在管理员 PowerShell 中执行以下命令。2026-11-01 的窗口交接修复需要先复制完整的 `windows\*.ps1`，再更新一次任务定义；更新任务不会停止当前认证。无需替换已稳定运行的 EXE。

```powershell
& 'F:\Tools\MiniEAP\Install-SCNUNETStartupTask.ps1'
& 'F:\Tools\MiniEAP\Install-SCNUNETDesktopShortcut.ps1'
```

任务名默认为 `MiniEAP SCNUNET Startup` 和 `MiniEAP SCNUNET Startup Resume`，分别在登录和睡眠/休眠恢复约 15 秒后运行。两者均使用最高权限和 `Interactive` 登录类型，隐藏的窗口管理入口处理完成后即退出，实际连接窗口仍是可见的 PowerShell。如果恢复时仍在锁屏，解锁后可看到窗口。已有本脚本管理的同名任务会安全更新；同名其他任务会被保留，除非显式使用 `-ReplaceExisting`。

桌面的 **连接 SCNUNET** 可随时双击打开连接窗口，不必等待 15 秒。已有受管理的窗口时，只尝试恢复/激活该窗口，不停止认证、不另开一个窗口；Windows 的前台焦点限制可能只让任务栏闪烁。快捷方式仍运行 `schtasks.exe /Run /TN "\MiniEAP SCNUNET Startup"`，无需重建快捷方式；不保存账号密码，也不会在双击时直接认证。卸载任务后可右键删除桌面快捷方式。

睡眠恢复触发器监听 System 日志中的 `Microsoft-Windows-Power-Troubleshooter` 事件 1，本机 S3 唤醒已记录此事件。旧版用 `Parallel` 直接新开启动器，旧窗口却仍运行或停在退出提示，因此会累积窗口。现在两个短时任务均使用 `IgnoreNew`，另有跨任务互斥，防止同时创建窗口；长时间认证由可见启动窗口承载，不会占住任务入口。此设置不会主动唤醒电脑。

恢复后的流程是：核实旧 PowerShell 的完整 `-File` 路径、EXE/配置/PID 路径、会话号和启动时间 → 对其控制台中的 MiniEAP 发正常 Ctrl+C → 等待子进程退出和 PID 锁释放 → 关闭旧脚本的等待提示 → 只打开一个新确认窗口。新窗口仍需 Enter/Y 才认证，N 跳过，不会自行重连。当前脚本也能处理旧版遗留的多个窗口。

控制事件仅由独立辅助进程发给经过核实的控制台，并检查控制台内没有其他程序；不会按 `minieap-win` 或 `powershell` 名称批量结束进程。旧进程未正常退出、身份无法验证或控制台混有其他程序时，交接失败并提示，不强制结束，也不启动新的认证。普通交互式 PowerShell 中手动运行的会话不属于可自动关闭的独立 `-File` 窗口，需要手动退出一次。

登录后直接按 Enter 或输入 `Y`/`y` 会在当前窗口启动 MiniEAP；输入 `N`/`n` 会跳过本次连接并关闭窗口。认证日志、认证成功信息和 Keep-Alive 日志会持续显示在同一窗口中。按 Ctrl+C 可结束 MiniEAP；MiniEAP 退出后按 Enter 关闭窗口。

启动器保留 PowerShell 原生 `&` 调用，没有重定向标准输入/输出或另开进程窗口。它会切换到 EXE 目录并在结束时恢复目录，传入固定的 `--conf-file` 和 `--pid-file`。启动前保守检测同名 MiniEAP 进程：发现已在线实例便跳过，即使旧实例使用了 `pid-file=none`。并发启动的最终互斥由 MiniEAP 的 PID 文件独占保证。非零退出会显示十进制和十六进制退出码。

仅修改磁盘中的 PID 配置不会给已运行的旧进程补上锁；下次正常重连后生效。其他手动启动方式也应使用同一配置/同一 PID 路径。正常桌面打开不会中断连接；只有睡眠恢复任务调用 `Open-SCNUNET.ps1 -Resume` 才进行旧会话交接，因此在线时不要手动执行 `-Resume` 来测试。脚本的 `-MiniEapPath`、`-ConfigPath`、`-PidFilePath` 参数供离线测试或迁移目录使用，均要求绝对路径，正常使用无需指定。

### 2026-09-28 崩溃修复与安全更新

`0xC0000409` 已在离线测试中定位到 RJv3 `computeV4()` 临时缓冲区越界：某些挑战值的原有有符号十六进制格式化需要 193 字节，而旧数组只有 160 字节。修复仅增加容量，保留所有散列、格式化和 `0x60` 字节的散列输入长度。详见 [崩溃定位与验证记录](docs/SCNUNET-launcher-diagnosis.md)。

本次构建产物位于 `x64\Debug` 和 `x64\Release`，不会自动覆盖正在使用的 `F:\Tools\MiniEAP\minieap-win.exe`。等到方便重连时，在原 MiniEAP 窗口 Ctrl+C 退出，然后从仓库目录更新两个文件：

```powershell
Copy-Item .\x64\Release\minieap-win.exe F:\Tools\MiniEAP\minieap-win.exe
Copy-Item .\windows\Start-SCNUNET.ps1 F:\Tools\MiniEAP\Start-SCNUNET.ps1
& 'F:\Tools\MiniEAP\Start-SCNUNET.ps1' -ValidateOnly
& 'F:\Tools\MiniEAP\Start-SCNUNET.ps1'
```

输入 Enter/Y，检查用户名/密码请求、计费通知、认证成功以及持续 Keep-Alive。只有这一步会进行真实 SCNUNET 重连；离线回归不能替代校园网验收。不要在旧实例仍在线时进行这一步。单独部署这个崩溃修复无需修改任务或稳定 tag，也不创建 Windows Service；睡眠恢复功能的任务更新见上一节。

可在任务计划程序中确认以下属性：

```powershell
Get-ScheduledTask -TaskName 'MiniEAP SCNUNET Startup*' |
  Select-Object TaskName, State, Principal, Triggers, Actions
```

卸载时，在管理员 PowerShell 中执行：

```powershell
& 'F:\Tools\MiniEAP\Uninstall-SCNUNETStartupTask.ps1'
```

卸载脚本删除上述两个计划任务，不停止当前连接。若未找到任务会正常返回；同名但不属于本项目的任务默认不会删除。该功能不会创建 Windows Service。

## 移植范围

- Windows/MSVC 使用显式 factory 表注册 `libpcap`、`rjv3`、`printer`；其他平台保留原注册分支。
- `arpa/inet.h`、`netinet/in.h` 对接 Winsock，`net/if.h` 为完整 Npcap 设备名保留足够空间；`unistd.h` 只映射有对应语义的 CRT 接口。
- Windows 使用 `GetAdaptersAddresses` 代替 POSIX `getifaddrs`、路由查询和 `/etc/resolv.conf`，按 Npcap GUID 读取当前网卡的 MAC、IPv4/IPv6、掩码、网关和 DNS。
- `strdup/strndup`、控制台 UTF-8、GBK 消息转换、Ctrl+C、PID 独占和前台运行均有 Windows 实现。
- Npcap 非阻塞 `pcap_dispatch` 配合接收事件和最多 50 ms 等待；认证超时和心跳由 `GetTickCount64` 定时队列在同一线程执行。没有报文时定时器也能继续工作。
- EAP-MD5/RJv3 散列运算和常量保留。调整限于固定宽度类型、指针对齐检查、临时缓冲容量、空地址列表及报文边界；过滤本机发出的帧，防止 Npcap 回送覆盖服务器请求。

完整变更列表见 `WINDOWS-CHANGES.md`。公开版 `port-validation/before-windows-port.zip` 只保留回归测试需要的 25 个移植前算法源文件及头文件，不含配置或运行日志。

## 验证

```powershell
.\tests\run-crypto-regression.ps1
.\tests\test-v4-bounds.ps1
.\tests\test-launcher.ps1
.\tests\test-window-manager.ps1
.\tests\test-startup-integration.ps1
.\tests\test-platform.ps1
.\tests\test-capture.ps1
.\tests\test-packets.ps1
.\tests\test-network.ps1 -Interface '\Device\NPF_{YOUR-ADAPTER-GUID}'
```

算法测试对比移植前源代码和当前代码；基线仅适配 `types.h` / `ustd.h` 才能由 MSVC 编译。覆盖 MD5 RFC 向量、RJv3 五个分支、含高位字节的挑战、密码散列及编码往返，另检查 64 字节凭据。

`test-v4-bounds.ps1` 从上述源码压缩包读取仅扩大临时数组的参考实现，不依赖本地历史 tag；比较 4096 组挑战值，在 Debug `/RTC1`、Release `/GS`、AddressSanitizer 下检查越界和输出一致性（需 VS C++ AddressSanitizer 组件）。`test-launcher.ps1` 编译不含网络代码的替代程序，使用真实 Windows 控制台处理器和 PID 锁，在 Windows PowerShell 5.1 及已安装的 PowerShell 7 中测试确认/取消、缺失文件、正常及异常退出、重复实例、PID 竞争和过期 PID 文件。独立隐藏控制台验证 Enter/Y、UTF-8、标准句柄继承、持续输出及真实 Ctrl+C；不会附加到在线 MiniEAP 的控制台。测试产物保存在忽略的 `.local-validation` 目录。

`test-window-manager.ps1` 先运行启动器回归，再使用唯一假配置和隐藏控制台复现“一个旧认证窗口 + 一个旧确认窗口”，验证 Ctrl+C 清理、PID 释放、旧等待提示关闭、只留下一个新窗口、重复桌面打开、连续恢复、进程身份不匹配时拒绝操作、替换窗口再次认证后恢复和 N 取消。所有探针都不含网络代码，真实 MiniEAP 不参与。

`test-startup-integration.ps1` 检查两个任务的登录/唤醒触发器、延迟、隐藏管理入口、桌面及权限设置、事件查询，以及临时目录中快捷方式的目标、重复安装、冲突保护、`-ValidateOnly` 和 `-WhatIf`。快捷方式测试要求已安装本项目的任务，测试本身不会注册或启动任务，也不会触发睡眠。实际睡眠恢复需由用户在方便时验收。

平台测试覆盖字符串/编码转换、`getopt_long` 二次扫描、定时器取消与回调修改队列、PID 文件独占及退出删除。网络测试只枚举和打开设备，验证 Intel I226-V、过滤器与无效设备处理。

抓包循环测试将真实后端连接到假的 pcap API，验证无包时定时器仍执行、停止标志、截断报文、发送长度和失败清理；不链接 Npcap Runtime，也不发送网络帧。启动器测试另以隔离控制台的真实 Ctrl+C 控制事件验证 Windows 停止处理器和清理流程。

Debug x64 完整重建和 Release x64 构建均成功。编译仍报告原有的 `size_t`/协议长度窄化转换警告（C4267/C4244）；未通过全局禁用警告隐藏它们。

报文集成测试使用真实配置、状态机、构包器和 RJv3 插件，网络接口与地址信息替换为测试实现。覆盖 Identity/MD5 响应、`service=staff` 被命令行 `students` 覆盖、服务字段字节、无 IP、超时重发、忽略本机回送帧、成功后的心跳和畸形属性拒绝。预期 EAP-MD5 摘要由独立的 .NET MD5 计算。

本地 `scnunet-working-v1` 稳定基线已经过 SCNUNET 实际认证和持续在线验证，保留原位置。后续启动器和窗口交接修改也已由用户完成实际验收。公开仓库从当前源码快照开始，不上传原本地历史及该 tag；离线回归使用随附的纯源码基线。
