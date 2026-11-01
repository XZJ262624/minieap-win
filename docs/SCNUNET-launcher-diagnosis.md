# SCNUNET 启动异常定位与验证（2026-09-28）

## 结论与证据范围

确认存在可稳定复现的 MiniEAP 本体栈越界：`computeV4()` 分支 0 的 `wtmp[160]` 容量不足。直接执行离线复现程序和通过 PowerShell 启动器执行都返回 `-1073740791 / 0xC0000409`。更换进程启动 API 不能修复这个缺陷；保留 `&` 调用。

历史崩溃的 WER 报告仍在，但临时 minidump 已被清理，因此无法恢复当次服务器挑战或完整调用栈。以下现场信息与离线复现一致；不能声称从历史 dump 逐帧证实了唯一原因，也不能把离线验证当成真实校园网重连测试。

## 本机证据

- Application Error 1000：2026-09-28 18:01:52，模块 `minieap-win.exe`，异常 `c0000409`，RVA `0x12e51`。
- WER 1001 / BEX64：异常数据 `0000000000000002`，即 `FAST_FAIL_STACK_COOKIE_CHECK_FAILURE`。
- 运行目录 EXE 与修复前仓库 Release EXE 的 SHA-256 均为 `6B95556BEF08DC00F26866BFB2128E975A6A22F188B1C7BF29651CAF93DDE56B`。
- 重建前保存匹配的 EXE/PDB。用 DbgHelp 解析故障 RVA，结果为 `__report_gsfailure + 0x1d`。
- 原始代码构建的离线 MSVC `/O2 /GS` 复现程序：输入 16 个 `0x80` 字节，直接执行与通过启动器执行均得到 `-1073740791`。
- 同一输入的 MSVC AddressSanitizer 报告为 `stack-buffer-overflow`，指向旧 `checkV4.c:1044` 的 `strcpy((char*)wtmp + wpos, tbuf)`。复现程序没有配置凭据、Npcap、网卡操作或网络发送。

`0xC0000409` 本身可以表示多种 fast-fail，必须结合原因码。这里原因码 2 指向栈 cookie 检查，不是原因码 5 的 invalid parameter。参考 [Microsoft fastfail 文档](https://learn.microsoft.com/en-us/cpp/intrinsics/fastfail?view=msvc-170)及 [Microsoft 原因码说明](https://devblogs.microsoft.com/oldnewthing/20190108-00/?p=100655)。

## 越界机制与最小修复

分支 0 将两段 MD5 格式化为共 64 个十六进制字符，并插入 16 个挑战字节。已有 `%02x` 对负的 `signed char` 在 MSVC 上会输出 8 个字符（例如 `ffffff80`），而不是 2 个；字段宽度 2 不是最大长度。

设挑战中有 `h` 个高位字节，格式化长度为 `96 + 6h`，还需要一个 NUL。`h >= 11` 时，160 字节数组不足；最坏需要 `64 + 16*8 + 1 = 193` 字节。输入全 `0x80` 同时满足分支 0 的原有选择规则。

只把临时数组扩大为上述容量。保留有符号格式化、分支选择、常量表、所有散列及 `wpos = 0x60` 不变，避免“修正”字节格式而破坏服务端兼容。EAP-MD5、Npcap、`service=students`、计时器和控制台处理代码未改。

不同认证会收到不同服务器挑战，这个条件性越界可以解释先崩溃、随后手动认证成功的现象；两次命令相同不能证明输入帧相同。由于历史转储缺失，无法验证那两次的具体挑战差异。

## 启动与控制台检查

- 原脚本使用绝对路径，切换到 EXE 目录，直接 `&` 调用；未重定向标准输入/输出，没有额外控制台处理器。
- Windows 初始化只设置输出代码页为 UTF-8，退出恢复；Ctrl+C / Ctrl+Break 处理器只原子设置停止标志，由主线程清理。源码中没有在控制线程进行认证或日志格式化。
- 新启动器保留直接调用和同窗口输出，恢复调用前目录，显示准确的十进制/十六进制退出码，兼容 PowerShell 7 的 native error preference。
- 同名进程预检查保护已使用 `pid-file=none` 的旧实例；真正的并发互斥依靠现有原生 PID 独占文件，不依赖存在性检查或 PID 数值。
- 日常配置仅更改 PID 行为 `F:\Tools\MiniEAP\minieap-i226v.pid`，其余字节保持不变；启动器也传入同一路径，防止配置意外关闭互斥。不输出或提交真实配置。

## 实际验证

1. Debug x64、Release x64 完整重建成功。保留现有窄化转换编译警告。
2. 原有密码学回归：MD5 RFC 向量、RJv3 五个分支、高位字节、密码散列、64 字节凭据、RJ 编解码均通过，与移植前输出一致。
3. 新增 4096 组挑战，包括最大展开长度、旧容量边界和随机挑战；Debug `/RTC1`、Release `/GS`、AddressSanitizer 均通过。与稳定 tag 中仅把临时数组扩大到 8192 字节的参考实现输出完全一致，输出文件 SHA-256 为 `CCF65034CC7F59408FA99CAB25EB42033B0F72FC362088DF182F4F6714BFC8BA`（PowerShell 7 的测试输出编码）。
4. 启动器在 Windows PowerShell 5.1 和 PowerShell 7 下均通过：语法、`-ValidateOnly`、N/n、Enter/Y/y、错误输入后重试、缺失 EXE/conf、正常退出、非零退出、异常码传播、已有同名实例、不同进程名竞争同一 PID 锁、过期 PID 文件处理。
5. 真实隔离控制台中验证 Enter/Y：stdin/stdout/stderr 均为 console、输出代码页 65001、中文/标准错误/连续模拟 Keep-Alive 可从该控制台读取；发送真实 Ctrl+C 后，生产控制处理器设置停止标志，探针清理并释放 PID、恢复代码页，PowerShell 退出码 0。控制事件只发给测试创建的控制台。
6. 离线平台测试通过，涵盖 PID 独占及退出删除；离线报文集成测试通过，涵盖 EAP-MD5/RJv3 构包、students 服务字段、计时器、认证成功及 Keep-Alive。没有加载 Npcap 或发送网络帧。
7. 默认路径 `-ValidateOnly` 通过。计划任务脚本只执行 `-ValidateOnly`：15 秒延迟、Highest、Interactive、IgnoreNew、无运行时限的定义有效；没有安装、更新或启动任务。

异常退出传播测试使用替代程序 `ExitProcess(0xc0000409)`，仅用于验证退出码显示；根因证据来自另行执行的旧 V4 真实越界复现，二者不混淆。

## 部署与剩余验收

验证期间没有停止或调试附加在线 MiniEAP，没有替换运行目录 EXE/启动脚本，没有发起真实认证。修复的源码、脚本及 Debug/Release 产物在仓库中。计划任务及 `scnunet-working-v1` 保留原样。

日常 PID 配置下次启动才生效；当前旧进程不会自动取得锁。待用户方便断网时，按 README 的安全更新步骤替换 Release EXE 和启动脚本，手动 Enter/Y 进行一次真实 SCNUNET 重连，观察认证成功及持续 Keep-Alive。在此之前不声称修复版已通过校园网在线验收。
