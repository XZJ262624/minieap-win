# MiniEAP for Windows x64

MiniEAP 的 Windows 11 x64 移植，使用 Visual Studio 2022 / MSVC 和 Npcap 收发 EAPOL 帧，支持 EAP-MD5、RJv3 及 SCNUNET `students` 服务。

Windows 版本前台运行，认证与 Keep-Alive 日志显示在 PowerShell 中。提供桌面快捷方式、登录/睡眠恢复后的连接确认、PID 独占和旧窗口交接功能，不创建 Windows Service。

## 编译与使用

1. 安装 Visual Studio 2022 的 C++ 桌面开发组件、Windows SDK 和 Npcap Runtime。仓库包含构建所用的 Npcap SDK。
2. 在仓库目录执行 `.\build-windows.ps1 -Configuration Release`，或使用 Visual Studio 打开 `minieap-win.sln`，选择 Release / x64。
3. 用生成的 `x64\Release\minieap-win.exe --list-interfaces` 获取本机 Npcap 设备名。
4. 复制 `minieap.windows.conf.example` 为本机 `minieap.conf`，填写账号、密码、设备名和固定 PID 路径。真实配置不应提交 Git。
5. 在 PowerShell 中运行 `& '.\x64\Release\minieap-win.exe' --conf-file '.\minieap.conf'`。按 Ctrl+C 正常退出。

完整的安装、网卡选择、桌面快捷方式、自动启动确认、睡眠恢复与卸载说明见 [README-Windows.md](README-Windows.md)。日常启动脚本默认使用 `F:\Tools\MiniEAP`；迁移目录时请按文档配置路径。同一接口请只运行一个认证进程。

## 测试与发布内容

本版本已在 SCNUNET 环境完成实际认证、持续在线和窗口交接验收。离线测试及适用范围见 [Windows 验证说明](README-Windows.md#验证)，历史移植记录见 [WINDOWS-CHANGES.md](WINDOWS-CHANGES.md)，崩溃定位记录见 [诊断文档](docs/SCNUNET-launcher-diagnosis.md)。

公开仓库从当前验证版本的源码快照开始，不包含本机认证配置、运行日志、构建输出或原本地开发历史。本地稳定标签 `scnunet-working-v1` 保留在原仓库，不随公开快照上传。算法回归所需的 `port-validation/before-windows-port.zip` 仅包含 25 个算法源文件及头文件，不含配置；测试不依赖旧的本地标签。

## 来源与许可

原 MiniEAP 说明和作者信息保留在 [src/minieap/README.md](src/minieap/README.md)。项目许可证见 [LICENSE](LICENSE)（GNU GPL v3）；第三方组件保留各自的版权声明与许可。Npcap SDK 与运行时的使用条件见其随附文档；仓库不包含 Npcap Runtime 安装包。
