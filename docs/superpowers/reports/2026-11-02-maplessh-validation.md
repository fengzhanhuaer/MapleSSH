# MapleSSH 0.1.0 本地交付与验证

日期：2026-11-02。这是首版实现及本地验证记录，不代表远程发布或完整人工验收。

## 交付

- 可执行文件：MapleSSH/dist/MapleSSH.exe，Windows x64，9,004,544 字节（约 8.59 MiB）。
- SHA-256：215d414e541f8cadf3756dafe6caa14e8adb1c72d8854aec3320651916e177ea
- 校验文件：MapleSSH/dist/MapleSSH.exe.sha256。单独分发 EXE 即可，首次运行会在其同目录建立 config/。
- 文档：MapleSSH/README.md。
- 固定依赖：wxWidgets 3.2.11、libssh2 1.11.1、OpenSSL 3.5.9、libvterm 0.3.3、nlohmann-json 3.12.0。使用校验过的源码归档构建，未修改上游库源码。
- 本机工具链：Visual Studio 2026 Professional / MSVC 14.51.36231，Windows SDK 10.0.26100.0，CMake 4.2.1。第三方库和 CRT 静态链接。

## 已实现

中文 wxWidgets 主窗口、分组会话维护、多标签终端、密码和私钥认证、首次主机信任确认、变更密钥阻止认证、连接/断开/手动重连、libvterm 绘制/滚屏/选择复制/粘贴/缩放/尺寸同步。本机密码和私钥内容由主密码加密保存；WebDAV 设置保留在加密本机配置中。

备份仅通过手动操作上传会话白名单，包含 SSH 密码、私钥内容和私钥口令，排除本机主机指纹及 WebDAV 凭据。恢复先完整解密和校验，再确认替换会话，保存加密恢复副本，保留本机设置、信任记录和已打开连接。

## 验证结果

| 项目 | 已执行与结果 |
| --- | --- |
| Release 编译 | 文档中的 build.ps1 成功完成构建、测试、PE 检查和打包 |
| CTest | 8/8 通过：terminal_sequences、model_roundtrip、backup_whitelist、invalid_models、crypto_roundtrip、crypto_rejects_damage、atomic_storage、url_and_host_trust |
| 主机认证边界 | 拒绝首次主机信任后，测试服务端认证回调计数保持不变，证明未发送认证请求 |
| SSH 认证与交互 | 密码认证、错误密码拒绝、OpenSSH Ed25519、加密 Ed25519、RSA PEM、PTY shell、输入回显、服务端确认 100×30 尺寸更新、同一客户端重复重连全部通过 |
| WebDAV 正常流程 | 创建目录、加密上传、备份列表、下载/解密/私钥还原、回读校验、同名冲突保留原字节全部通过 |
| 实际 MapleStore 兼容 | 编译现有 MapleStore 源码，以回环地址、随机临时凭据和隔离数据/配置目录运行，上述正常流程通过 |
| 网络失败注入 | 401、403、重定向不跟随（接收端计数为零）、损坏回读拒绝、在途取消、10 秒网络超时全部通过；故障注入由临时测试服务提供 |
| PE 导入 | x64，只导入 Windows 系统 DLL/API set，未导入 wxWidgets、libssh2、OpenSSL 或 VC 运行库 DLL |
| 资源 | Manifest、版本及第三方许可证编译进 EXE，帮助菜单提供许可证查看器 |
| 桌面冒烟 | 隔离配置模式成功启动并自动退出；通过 computer-use 实际查看主窗口截图和可访问性树，中文菜单、工具栏、欢迎页与状态栏文字正常 |

本次用失败测试定位到 Go WebDAV 的 PUT 会忽略 If-None-Match；改为随机加密临时文件加 MOVE / Overwrite: F 后，同名上传失败且原备份字节保持不变。临时 SSH 服务也改为并发处理 shell 数据及窗口请求，尺寸测试要求收到服务端确认，避免只验证客户端调用。

## 可复现命令

在仓库根目录 PowerShell 中运行，原生 Windows Perl 仅在首次编译 OpenSSL 时需要：

    ./MapleSSH/scripts/build.ps1 -PerlPath 'C:/Strawberry/perl/bin/perl.exe' -Integration
    ./MapleSSH/scripts/integration.ps1 -MapleStore
    ./MapleSSH/dist/MapleSSH.exe --smoke-test

可用 -WxSource 指向完整 wxWidgets 3.2.11 源码以复用已下载依赖。本次使用仓库下 .build-tools/wxwidgets-3.2.11，下载缓存及构建目录均已加入 .gitignore。

## 人工验收与限制

- 尚未在另一台干净 Windows 机器执行，也未验证真实远程 SSH 主机上的 vim/top、中文输入法、组合键、剪贴板、多屏 DPI 和断网行为。
- 主窗口截图检查不等同于完整桌面操作验收；会话编辑保存、主密码交互及备份恢复按钮的完整人工流程待验证。
- 未连接部署中的 NAS/WebDAV，未验证其反向代理、实际 TLS 证书或账号权限；本地实际 MapleStore 测试使用 HTTP 回环地址。生产地址仍强制 HTTPS，WinHTTP 默认执行证书检查，没有忽略开关。
- 网络取消受同步 WinHTTP 调用超时约束；中断可能留下不列入备份列表的加密上传临时文件。
- 字体缩放暂不持久化；不包含 SFTP、转发、跳板机、SSH Agent、自动同步、自动命令或密码找回。

MapleStore 源码、已部署服务和生产数据未改动。没有创建 Git 分支、提交、远程发布、CI 工作流或进行远程安装。测试辅助进程和随机数据目录在脚本结束后清理。
