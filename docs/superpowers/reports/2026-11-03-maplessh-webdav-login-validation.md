# WebDAV 测试登录按钮验证

日期：2026-11-03。新增设置窗口中的“测试登录”按钮，使用当前填写的地址和凭据，在工作线程执行只读 PROPFIND Depth: 0，检查 WebDAV 根目录与有效 DAV multistatus/collection 成功状态。测试不创建备份目录、不上传文件，也不保存设置；成功代表认证及目录访问正常，实际备份时另行验证写入权限。

## 已验证

- 先添加缺失 TestLogin API 的回归用例，编译失败；实现后通过。
- Release 编译成功，10/10 CTest 通过。
- 临时 Go WebDAV 服务和实际 MapleStore 源码的隔离回环实例均通过：有效登录、未创建备份子目录时登录、错误密码 401、拒绝访问 403、重定向不跟随、HTML 首页 200 拒绝、伪 WebDAV 207 拒绝、DAV 207 内嵌权限拒绝。
- 测试服务的写请求计数在登录测试前后保持不变；原有 SSH、备份恢复、同名保护、取消、超时及损坏回读回归通过。
- UI 使用当前字段快照，测试中禁用编辑和保存，允许取消；修改字段会清除此前成功提示，窗口生命周期等待工作线程退出。用户在交互过程中反馈“测试了，可以了”。
- 保留用户正在使用的窗口。构建时采用独立输出目录，打包到 dist，未覆盖正在运行的旧程序。
- PE 检查：x64，仅依赖 Windows 系统 DLL，保持单 EXE 绿色部署。

## 交付

- 程序：MapleSSH/dist/MapleSSH.exe。
- 大小：8,965,120 字节。
- SHA-256：965a1e0608878c3acc51c9edb06185c8ad468e87f6d75a7d0a09f601de1df7b0。
- 散列文件：MapleSSH/dist/MapleSSH.exe.sha256。
- 构建输出：MapleSSH/out/webdav-login-release/MapleSSH.exe。因为用户原先运行 out/build/bin/Release/MapleSSH.exe，本次使用 MSBuild 的 OutDir 覆盖参数输出到独立目录。常规关闭旧版后仍可按 README 的 build.ps1 构建。

验证未使用截图中的账号向生产域名发送认证请求。GUI 全流程自动化和生产 HTTPS 服务器的独立自动验证不在本次已完成范围。同步 WinHTTP 调用取消仍受现有网络超时约束。

当前交互窗口位于 out/webdav-login-ui-* 隔离目录，仍使用临时回环测试端点；该窗口、隔离配置及其配套测试服务暂时保留，避免中断用户交互。正式交付程序以 dist/MapleSSH.exe 为准。
