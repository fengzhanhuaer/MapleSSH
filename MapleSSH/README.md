# MapleSSH

Windows x64 原生 SSH 客户端初版，界面使用 wxWidgets，终端使用 libvterm，SSH 使用 libssh2 / OpenSSL。现有仓库中的 MapleStore 不作改动。

## 使用

1. 把 dist/MapleSSH.exe 放到有写权限的普通目录，直接运行，无需安装。
2. 首次设置至少 12 字符的主密码；以后启动需要解锁。程序会在 EXE 同目录创建 config/。
3. 新增会话：填写名称、分组、主机、端口、用户名，选择密码或私钥认证。导入的私钥以内容存入加密配置，不依赖原文件路径。
4. 双击会话连接；首次连接先核实服务器指纹，再选择是否信任。指纹变化会阻止认证，不自动覆盖记录。
5. 多标签终端支持 UTF-8、ANSI 颜色、备用屏幕、窗口尺寸同步、滚动和选择复制。Ctrl+Shift+C/V 复制粘贴，Ctrl+滚轮调整字号，多行粘贴需要确认。
6. 在“备份 → WebDAV 设置”填写 MapleStore 的 HTTPS 地址（含 /dav/）、账号、密码和一个备份目录名，再手动备份或恢复。程序不会自动同步或自动上传。

## 文件与备份边界

- config/vault.msv：会话和本机 WebDAV 设置，scrypt + AES-256-GCM 加密。SSH 密码、私钥内容和私钥口令均在其中。
- config/known_hosts：本机主机信任记录，不含凭据，不上传。config/.lock 用于阻止同时写同一配置。
- 云端 maplessh-*.msb：仅备份会话白名单，包括 SSH 凭据，不含指纹、WebDAV 地址/账号/密码、日志或终端输出。每次随机盐和随机 nonce，加密临时文件经 MOVE / Overwrite: F 发布，再回读校验，避免覆盖已有备份。WebDAV 账号需有创建、写入、移动及删除临时文件的权限。
- 网络中断或取消可能留下 maplessh-upload-*.tmp 加密临时文件，不会出现在备份列表中，可在 WebDAV 文件管理中手动清理。
- 恢复需要备份创建时的主密码；先完整解密和校验，再确认替换会话列表。保留当前 WebDAV 设置、主机信任和已打开连接，并先保留本机加密恢复副本。
- 主密码遗失无法恢复，请自行安全保管。EXE 可以单独分发；迁移已有本机数据需另带 config/，或在新机器手动恢复云端备份。
- 远程 WebDAV 只允许 HTTPS，证书正常校验；仅回环地址允许 HTTP。本程序不提供跳过证书或跳过 SSH 主机身份检查选项。

## 构建

构建机器需要 Windows x64、Visual Studio C++ 桌面工作负载及 Windows SDK、CMake 3.24+、原生 Windows Perl（如 Strawberry Perl）。Perl、CMake、Go 都不是最终程序的运行依赖。

在仓库根目录的 PowerShell 中执行：

    ./MapleSSH/scripts/build.ps1 -PerlPath 'C:/Strawberry/perl/bin/perl.exe'

脚本按固定版本下载并校验源码 SHA-256，静态构建 wxWidgets 3.2.11、libssh2 1.11.1、OpenSSL 3.5.9、libvterm 0.3.3，使用 nlohmann-json 3.12.0。统一使用 MSVC /MT。也可用 -WxSource 指定完整的同版本 wxWidgets 源码，避免重复下载。网络下载中断可再次执行以续传，散列不匹配则停止。

构建结束运行 CTest 和 PE 导入检查，输出 dist/MapleSSH.exe 与 SHA-256 文件；只需分发 EXE。帮助菜单可查看内嵌第三方许可证。

## 验证

    ctest --test-dir MapleSSH/out/build -C Release --output-on-failure
    ./MapleSSH/scripts/integration.ps1
    ./MapleSSH/scripts/integration.ps1 -MapleStore

联调脚本额外需要 Go 1.26，启动仅监听回环地址的临时 SSH/WebDAV 服务，使用临时随机凭据；-MapleStore 则另行编译仓库中的 MapleStore，并用隔离的数据和配置目录验证实际服务兼容性。结束后停止自己启动的进程并清理隔离目录。不会连接部署中的 MapleStore、真实 SSH 主机或使用生产凭据。

    ./MapleSSH/out/build/bin/Release/MapleSSH.exe --smoke-test

冒烟模式使用临时配置目录，打开空主窗口并在 15 秒后自动退出，不读取真实配置。它不代替真实中文输入法、vim/top、多屏 DPI、断网恢复和干净 Windows 机器的人工验收。当前不包含 SFTP、端口转发、跳板机、自动同步或密码找回。
