# Windows x64 移植交付清单

本文件记录 2026-09-27 的移植交付状态；当时使用修改前 SHA-256 清单逐项比对。下文的日志和哈希清单属于本机历史验证产物，不随公开仓库上传。公开版 `port-validation/before-windows-port.zip` 只保留算法回归所需的 25 个源文件及头文件。后续修复及当前使用方法见 README-Windows.md。

## 构建结果

| 配置 | 结果 | 输出 |
| --- | --- | --- |
| Debug x64 Rebuild | 0 错误，20 条 C4267/C4244 长度转换警告 | `x64/Debug/minieap-win.exe` |
| Release x64 Build | 0 错误，20 条 C4267/C4244 长度转换警告 | `x64/Release/minieap-win.exe` |

EXE 的 PE Machine 为 `0x8664`（AMD64）。构建日志位于 `port-validation/Debug-x64-build.log` 和 `port-validation/Release-x64-build.log`。工程编译真正的 MiniEAP 入口与全部 RJv3 源码，仅使用 Npcap/libpcap 后端。

## 实现内容

1. `.vcxproj`/filters：完整源码与头文件、C11/UTF-8/ANSI getopt、Windows 与 x64 库、Npcap 延迟加载。
2. Windows 显式注册 libpcap、RJv3、printer，保留其他平台原有注册分支。
3. Winsock 头文件、字符串复制、Windows 控制台与运行时 DLL 初始化；移除类型宏污染和 LONG/DWORD 冲突。
4. `GetAdaptersAddresses` 代替 Windows 上不存在的 getifaddrs 和 POSIX 路由/DNS查询，按完整 NPF GUID 获取 MAC/IP/掩码/网关/DNS。
5. Npcap 收发、链路类型和过滤器检查、资源清理；主线程定时器驱动超时/心跳和前台停止。
6. Windows 前台模式、可选独占 PID 文件、参数多次扫描、配置覆盖、网卡枚举和无发送设备检查。
7. 空地址/无 DNS、报文长度、截断属性、echo key 最小长度、复制缓冲、日志和释放路径修复；本机发送回环不覆盖服务器请求。
8. MD5/RJv3 的散列运算与常量未替换。修改为固定宽度类型、uintptr_t 对齐检查、扩大凭据临时缓冲及边界检查；算法输出对比通过。

既有 `compat/arpa/inet.h` 和 `compat/net/if.h`（IFNAMSIZ=256）经检查已可用，保持原样。原 `main.c` 枚举样例不参与编译；Linux/BSD 后端、第三方 Npcap SDK、Linux Makefile/配置保持原样。

## 验证结果

| 验证 | 结果/记录 |
| --- | --- |
| MD5 RFC 向量、RJv3 五分支/高位挑战/密码/编码、原始源对比、64 字节凭据 | 通过；`port-validation/algorithm-tests/` |
| 编码转换、getopt 二次扫描、定时器取消/回调修改队列、PID 独占与删除 | 通过；`port-validation/platform-test.log` |
| 实际后端 + fake pcap：无包定时器、停止、收发边界、失败清理 | 通过；`port-validation/capture-tests/` |
| 实际状态机 + fake 网卡：Identity、独立 MD5 校验、students 字段、超时重发、成功心跳、畸形属性 | 通过；`port-validation/packet-tests/` |
| 真实 Intel I226-V 枚举、设备打开、Ethernet/EAPOL 过滤器、无效 GUID | Debug/Release 均通过；`port-validation/network-tests/` |
| 缺少 service 参数、未知插件/选项、不支持的 daemon/kill | 正常非零退出 |

真实网卡检查均使用 `--list-interfaces` 或 `--check-interface`，未发送认证帧。Ctrl+C 对应停止标志通路做了模拟测试，未模拟真实键盘事件。尚未使用真实账号验证校园网登录或长期心跳。

## 文件清单

以下逐项列出修改及新增的源码、工程、测试、脚本和文档。编译生成物位于 `x64/`、`minieap-win/x64/`；备份、构建日志、测试对象文件和结果位于 `port-validation/`，不计入源码文件清单。

| 状态 | 文件 |
| --- | --- |
| 新增 | [build-windows.ps1](F:/VS2022Project/minieap-win/build-windows.ps1) |
| 修改 | [compat/getopt.c](F:/VS2022Project/minieap-win/compat/getopt.c) |
| 修改 | [compat/getopt.h](F:/VS2022Project/minieap-win/compat/getopt.h) |
| 新增 | [compat/netinet/in.h](F:/VS2022Project/minieap-win/compat/netinet/in.h) |
| 修改 | [compat/unistd.h](F:/VS2022Project/minieap-win/compat/unistd.h) |
| 新增 | [compat/win32_compat.c](F:/VS2022Project/minieap-win/compat/win32_compat.c) |
| 新增 | [compat/win32_compat.h](F:/VS2022Project/minieap-win/compat/win32_compat.h) |
| 修改 | [minieap-win.vcxproj](F:/VS2022Project/minieap-win/minieap-win.vcxproj) |
| 修改 | [minieap-win.vcxproj.filters](F:/VS2022Project/minieap-win/minieap-win.vcxproj.filters) |
| 新增 | [minieap.windows.conf.example](F:/VS2022Project/minieap-win/minieap.windows.conf.example) |
| 新增 | [README-Windows.md](F:/VS2022Project/minieap-win/README-Windows.md) |
| 修改 | [src/minieap/config.c](F:/VS2022Project/minieap-win/src/minieap/config.c) |
| 修改 | [src/minieap/eap_state_machine.c](F:/VS2022Project/minieap-win/src/minieap/eap_state_machine.c) |
| 修改 | [src/minieap/if_impl/if_impl.c](F:/VS2022Project/minieap-win/src/minieap/if_impl/if_impl.c) |
| 修改 | [src/minieap/if_impl/libpcap/if_impl_libpcap.c](F:/VS2022Project/minieap-win/src/minieap/if_impl/libpcap/if_impl_libpcap.c) |
| 修改 | [src/minieap/include/config.h](F:/VS2022Project/minieap-win/src/minieap/include/config.h) |
| 修改 | [src/minieap/include/minieap_common.h](F:/VS2022Project/minieap-win/src/minieap/include/minieap_common.h) |
| 修改 | [src/minieap/include/misc.h](F:/VS2022Project/minieap-win/src/minieap/include/misc.h) |
| 修改 | [src/minieap/include/module_init.h](F:/VS2022Project/minieap-win/src/minieap/include/module_init.h) |
| 修改 | [src/minieap/include/net_util.h](F:/VS2022Project/minieap-win/src/minieap/include/net_util.h) |
| 修改 | [src/minieap/include/sched_alarm.h](F:/VS2022Project/minieap-win/src/minieap/include/sched_alarm.h) |
| 修改 | [src/minieap/include/types.h](F:/VS2022Project/minieap-win/src/minieap/include/types.h) |
| 修改 | [src/minieap/include/ustd.h](F:/VS2022Project/minieap-win/src/minieap/include/ustd.h) |
| 修改 | [src/minieap/minieap.c](F:/VS2022Project/minieap-win/src/minieap/minieap.c) |
| 修改 | [src/minieap/packet_builder.c](F:/VS2022Project/minieap-win/src/minieap/packet_builder.c) |
| 修改 | [src/minieap/packet_plugin/packet_plugin.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/packet_plugin.c) |
| 修改 | [src/minieap/packet_plugin/printer/packet_plugin_printer.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/printer/packet_plugin_printer.c) |
| 修改 | [src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_keepalive.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_keepalive.c) |
| 修改 | [src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_priv.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_priv.c) |
| 修改 | [src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_prop.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/packet_plugin_rjv3_prop.c) |
| 修改 | [src/minieap/packet_plugin/rjv3/packet_plugin_rjv3.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/packet_plugin_rjv3.c) |
| 修改 | [src/minieap/packet_plugin/rjv3/packet_plugin_rjv3.h](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/packet_plugin_rjv3.h) |
| 修改 | [src/minieap/packet_plugin/rjv3/rjv3_hashes/byte_order.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/rjv3_hashes/byte_order.c) |
| 修改 | [src/minieap/packet_plugin/rjv3/rjv3_hashes/byte_order.h](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/rjv3_hashes/byte_order.h) |
| 修改 | [src/minieap/packet_plugin/rjv3/rjv3_hashes/checkV4.c](F:/VS2022Project/minieap-win/src/minieap/packet_plugin/rjv3/rjv3_hashes/checkV4.c) |
| 修改 | [src/minieap/util/conf_parser.c](F:/VS2022Project/minieap-win/src/minieap/util/conf_parser.c) |
| 修改 | [src/minieap/util/logging.c](F:/VS2022Project/minieap-win/src/minieap/util/logging.c) |
| 修改 | [src/minieap/util/misc.c](F:/VS2022Project/minieap-win/src/minieap/util/misc.c) |
| 修改 | [src/minieap/util/net_util.c](F:/VS2022Project/minieap-win/src/minieap/util/net_util.c) |
| 修改 | [src/minieap/util/packet_util.c](F:/VS2022Project/minieap-win/src/minieap/util/packet_util.c) |
| 修改 | [src/minieap/util/pid_lock.c](F:/VS2022Project/minieap-win/src/minieap/util/pid_lock.c) |
| 修改 | [src/minieap/util/sched_alarm.c](F:/VS2022Project/minieap-win/src/minieap/util/sched_alarm.c) |
| 新增 | [tests/capture_smoke.c](F:/VS2022Project/minieap-win/tests/capture_smoke.c) |
| 新增 | [tests/crypto_regression.c](F:/VS2022Project/minieap-win/tests/crypto_regression.c) |
| 新增 | [tests/packet_smoke.c](F:/VS2022Project/minieap-win/tests/packet_smoke.c) |
| 新增 | [tests/platform_smoke.c](F:/VS2022Project/minieap-win/tests/platform_smoke.c) |
| 新增 | [tests/run-crypto-regression.ps1](F:/VS2022Project/minieap-win/tests/run-crypto-regression.ps1) |
| 新增 | [tests/test-capture.ps1](F:/VS2022Project/minieap-win/tests/test-capture.ps1) |
| 新增 | [tests/test-network.ps1](F:/VS2022Project/minieap-win/tests/test-network.ps1) |
| 新增 | [tests/test-packets.ps1](F:/VS2022Project/minieap-win/tests/test-packets.ps1) |
| 新增 | [tests/test-platform.ps1](F:/VS2022Project/minieap-win/tests/test-platform.ps1) |
| 新增 | [WINDOWS-CHANGES.md](F:/VS2022Project/minieap-win/WINDOWS-CHANGES.md) |
