# 文件浏览器右键与父目录补充交付

日期：2026-11-10（Asia/Singapore）。

## 用户追加要求

- 非根目录文件列表首行显示 ..，双击返回父目录。
- 右键支持上传到当前目录、下载所选普通文件，以及 cd 到所选目录/当前目录。
- cd 点击后直接发送，不再弹窗确认；浏览文件不会自动执行 cd。上传下载的覆盖确认及 MCP 本机文件访问批准不受影响。

## 修改与验证

- 增加 RemoteBrowsePath：路径栏的 .. 和相对路径以当前浏览目录为基准，不误用 SFTP 登录目录。对应测试先因函数不存在失败，随后通过。
- FileBrowserPanel 增加父目录行与右键菜单；根目录不显示 ..。右键空白处清空选择，禁用无有效文件的下载；目录 cd 安全引用并验证当前连接仍是面板对应的连接。
- Release 编译成功：MapleSSH/out/file-context-build.log。
- CTest：16/16，通过；新增父目录/相对导航断言。
- 隔离 SSH/SFTP/WebDAV 集成回归通过：MapleSSH/out/file-context-integration.log，未连接真实服务器。
- PE 检查为 x64、系统 DLL 导入，无外部 SCP 或额外运行库 DLL。
- 用户级 maplessh-mcp 技能已更新 SFTP 文件任务、界面批准、查询/取消和直接 cd 行为；保留默认 MapleSSH MCP 通道与禁止静默回退约束。
- 技能 quick_validate、UI metadata、PowerShell 语法及离线 helper mock 验证通过。mock 验证中文路径、默认不覆盖、异步 failed 状态可返回以及错误不自动重试，不连接真实服务器。
- 本机 MCP 端点校验时连接被拒绝；没有自动开启 GUI、重启软件或改用其他 SSH 通道。

## 发布

- MapleSSH/dist/MapleSSH-20261110-r2.exe
- 大小：9432576 字节。
- SHA-256：0da331527c6722cbc2b7aed70bbf9ea696f5f5fe4f80ab0185db19e16d9f389b
- 同目录提供 .sha256，发布副本与构建产物哈希一致，未覆盖已有版本。

桌面右键、父目录显示、cd 点击及不同 DPI 下布局未自动点验，需要启动 r2 人工验收。之前 r1 的验证报告保留其当时行为，cd 确认已由本次用户要求取消。
