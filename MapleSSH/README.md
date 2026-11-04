# MapleSSH

Windows x64 原生 SSH 客户端初版，界面使用 wxWidgets，终端使用 libvterm，SSH 使用 libssh2 / OpenSSL。现有仓库中的 MapleStore 不作改动。

## 使用

1. 把 dist/MapleSSH.exe 放到有写权限的普通目录，直接运行，无需安装。
2. 启动直接进入主界面，无需创建账号或设置主密码。程序会在 EXE 同目录创建 config/。
3. 通过工具栏“凭据”或“会话 → 凭据管理”建立命名凭据，填写用户名及密码或私钥；一份凭据可用于多台主机。导入的私钥以内容保存，不依赖原文件路径。编辑共享凭据后，相关主机下次连接或重连使用更新值；正在使用的凭据不能直接删除。
4. 新增或编辑会话时填写名称、主机、端口；分组可下拉选择已有名称，也可输入新名称，留空表示未分组。再从下拉框选择已有凭据，也可点“管理凭据…”创建或修改。取消会话编辑会同时放弃该窗口中的凭据修改。右键分组名称可重命名，同组会话一并更新。
5. 双击会话连接；首次连接先核实服务器指纹，再选择是否信任。指纹变化会阻止认证，不自动覆盖记录。
6. 多标签终端支持 UTF-8、ANSI 颜色、备用屏幕、窗口尺寸同步、滚动和选择复制。Ctrl+Shift+C/V 复制粘贴，Ctrl+滚轮调整字号，多行粘贴需要确认。
7. 在“备份 → WebDAV 设置”填写 MapleStore 的 HTTPS 地址（含 /dav/）、账号、密码和一个备份目录名，点击“测试登录”检查当前填写的设置。成功后点击 OK 保存，再手动备份或恢复。程序不会自动同步或自动上传。

“测试登录”在后台只读检查 WebDAV 根目录，显示登录成功或账号密码、权限、地址、证书/连接等错误；备份子目录尚未创建也可测试。测试不保存设置、不创建目录或上传文件，写入权限由实际备份验证。填写网页登录首页会提示检查 WebDAV 地址；测试中可取消，修改设置后需重新测试。

手动恢复列表按最新备份在前排列，将标准备份文件名中的 UTC 时间转换为本机时间，显示为 YYYY-MM-DD HH:MM:SS，并隐藏随机标识。同一显示时间的多份备份加序号区分；无法识别时间的文件保留原名，选中后仍按对应原文件恢复。

## 命令输入与快捷按钮

- 每个会话标签底部有独立多行命令框，可直接粘贴多行文本；Enter 或 Ctrl+Enter 直接发送全部内容，Shift+Enter 在框内换行，也可点击“执行”。键盘发送不再二次确认，点击“执行”发送多行内容时仍确认目标主机。结果仍显示在该会话终端。
- 命令只发送到对应会话当前的交互终端，不广播、不另建 SSH 连接；如果前台是 vim、数据库客户端等程序，输入也会由该程序接收，请先确认当前状态。
- 输入按原顺序保留缩进、空行和中文，统一处理 CRLF、LF、CR，末行自动补一个回车；单次发送上限 1 MiB，拒绝空白内容和不支持的控制字符。发送不等于远程执行成功，需看终端返回。
- 命令框上方点击“添加…”可保存命名快捷命令，默认带入当前框内内容；点击快捷按钮直接发送全部命令，不再二次确认，右键可编辑或删除。请只保存自己确认过的命令。
- 快捷命令在所有会话共享，最多 200 条，保存在本机配置中并随手动备份、恢复。恢复和重连不会自动执行快捷命令。会话命令框草稿不持久化、不备份；Enter 或 Ctrl+Enter 成功提交发送后清空并保留框内焦点，未连接或发送失败时保留原文。点击“执行”仍保留原文，也可用“清空”移除。成功提交不代表远程执行完成。
- 未连接或断线时不发送，也不会排队等待重连后自动执行。
- 整个命令区与终端使用一致的深色背景、浅色文字，不显示多余标题，缩减上下留白，保留细分隔线。拖动终端与命令区之间的横向分隔线可调整高度，各会话独立；窗口变大时优先扩展终端，保留命令区高度。
- 命令输入框不显示横向、纵向滚动条；长行按当前宽度显示折行，不额外写入换行或改变发送内容。多行内容仍可通过光标移动、鼠标滚轮查看，输入时自动跟随位置。
- 工具栏“会话栏”、菜单“视图 → 会话栏”或 Ctrl+Shift+L 可收起、展开左侧列表；同次运行内展开时恢复收起前宽度，不断开连接、不清空命令。布局尺寸暂不跨重启保存。

## 文件与备份边界

- config/vault.json：版本 3 明文 JSON，独立保存会话、凭据目录、快捷命令和本机 WebDAV 设置。会话通过 credential_id 引用凭据；SSH 密码、私钥内容、私钥口令及 WebDAV 密码均保存在配置中，没有额外主密码或密钥文件。
- config/known_hosts：本机主机信任记录，不含凭据，不上传。config/.lock 用于阻止同时写同一配置。
- 云端 maplessh-*.msb：内容为版本化明文 JSON，备份会话、完整凭据目录（包括尚未分配的凭据）、引用关系及快捷命令，不含指纹、WebDAV 地址/账号/密码、命令框草稿、日志或终端输出。临时文件经 MOVE / Overwrite: F 发布，再回读校验，避免覆盖已有备份。WebDAV 账号需有创建、写入、移动及删除临时文件的权限。
- 网络中断或取消可能留下 maplessh-upload-*.tmp 明文临时文件，不会出现在备份列表中，可在 WebDAV 文件管理中手动清理。
- 恢复无需主密码；先完整解析和校验，再确认替换会话列表、凭据目录和快捷命令。保留当前 WebDAV 设置、主机信任和已打开连接，并先保留 config/vault.before-restore.*.json 本机恢复副本。版本 1、2 备份没有快捷命令，确认恢复后快捷命令列表为空。
- 版本 1、2 明文配置与备份可自动读取，快捷命令初始为空。版本 1 还会将原会话内的相同认证资料合并为可复用凭据；不同认证资料保留独立记录。首次保存旧配置前，分别保留 config/vault.before-credentials.*.json（版本 1）或 config/vault.before-quick-commands.*.json（版本 2）原文件副本，再写入版本 3。旧程序不能读取版本 3，回退时需使用原文件副本。
- HTTPS 保护传输，配置、备份和恢复副本本身不加密；有这些文件读取权限的人可以读取其中凭据。EXE 可以单独分发；迁移已有本机数据需另带 config/，或在新机器填写 WebDAV 设置后手动恢复备份。
- 旧版加密 vault.msv 会被识别并保留，不自动重置或覆盖；须先用旧版读取并迁移。旧版加密备份也需先用旧版解密，新版不会尝试无密码解密。
- 远程 WebDAV 只允许 HTTPS，证书正常校验；仅回环地址允许 HTTP。本程序不提供跳过证书或跳过 SSH 主机身份检查选项。

## 构建

构建机器需要 Windows x64、Visual Studio C++ 桌面工作负载及 Windows SDK、CMake 3.24+、原生 Windows Perl（如 Strawberry Perl）。Perl、CMake、Go 都不是最终程序的运行依赖。

在仓库根目录的 PowerShell 中执行：

    ./MapleSSH/scripts/build.ps1 -PerlPath 'C:/Strawberry/perl/bin/perl.exe'

脚本按固定版本下载并校验源码 SHA-256，静态构建 wxWidgets 3.2.11、libssh2 1.11.1、OpenSSL 3.5.9、libvterm 0.3.3，使用 nlohmann-json 3.12.0。统一使用 MSVC /MT。也可用 -WxSource 指定完整的同版本 wxWidgets 源码，避免重复下载。网络下载中断可再次执行以续传，散列不匹配则停止。

构建结束运行 CTest 和 PE 导入检查，输出 dist/MapleSSH.exe 与 SHA-256 文件；只需分发 EXE。帮助菜单可查看内嵌第三方许可证。

### VS Code 直接构建

用 VS Code 打开仓库根目录，安装 CMake Tools 扩展。本仓库已设置相对源码目录和 CMake Presets，无需再手动选择工具包。预设使用 Visual Studio 2026 x64 和 CMake 4.2+，复用 out/build，不影响已运行的 dist/ 程序。

首次在新机器构建仍需先运行上面的 build.ps1，准备并校验依赖及静态 OpenSSL；预设不会自动下载安装依赖。默认 wxWidgets 路径与 prepare-deps.ps1 一致，已有构建缓存或自定义 MAPLE_WX_SOURCE 会保留。

1. 执行命令面板中的 CMake: Select Configure Preset，选择 windows-x64。
2. 执行 CMake: Select Build Preset，选择 release，然后点击 CMake 面板的“生成”。测试预设同样选择 release。
3. 输出为 out/build/bin/Release/MapleSSH.exe；IDE 构建不自动复制到 dist/，以免覆盖正在使用的程序。

如果面板仍显示“未选择工具包”，执行 Developer: Reload Window 后重新选择预设。也可在仓库根目录终端执行：

    cmake --preset windows-x64 -S MapleSSH
    cmake --build MapleSSH/out/build --config Release --parallel 8
    ctest --test-dir MapleSSH/out/build -C Release --output-on-failure

## 验证

    ctest --test-dir MapleSSH/out/build -C Release --output-on-failure
    ./MapleSSH/scripts/integration.ps1
    ./MapleSSH/scripts/integration.ps1 -MapleStore

联调脚本额外需要 Go 1.26，启动仅监听回环地址的临时 SSH/WebDAV 服务，使用临时随机凭据；-MapleStore 则另行编译仓库中的 MapleStore，并用隔离的数据和配置目录验证实际服务兼容性。结束后停止自己启动的进程并清理隔离目录。不会连接部署中的 MapleStore、真实 SSH 主机或使用生产凭据。

    ./MapleSSH/out/build/bin/Release/MapleSSH.exe --smoke-test

冒烟模式使用临时配置目录，打开空主窗口并在 15 秒后自动退出，不读取真实配置。它不代替真实中文输入法、vim/top、多屏 DPI、断网恢复和干净 Windows 机器的人工验收。当前不包含 SFTP、端口转发、跳板机、自动同步或密码找回。
