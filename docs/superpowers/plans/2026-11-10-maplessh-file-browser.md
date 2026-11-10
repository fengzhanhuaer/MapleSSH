# MapleSSH File Browser Implementation Plan

**Goal:** 可收起的右侧 SFTP 文件浏览器、后台上传下载和 MCP 文件工具。

**Architecture:** 抽取现有 SSH 网络辅助函数；独立 SFTP worker 使用已认证终端的公钥与凭据快照，任务轮询驱动 wxWidgets 面板及 MCP，不共享 libssh2 session 的跨线程访问。

**Tech Stack:** C++20 / wxWidgets 3.2.11 / libssh2 1.11.1 / OpenSSL / Go SSH fixture。

**Spec:** docs/superpowers/specs/2026-11-10-maplessh-file-browser-design.md

## Global Constraints

- Windows x64，静态链接，单 EXE；不修改 MapleStore，不读取真实配置或操作用户界面。
- 不自动信任主机，不默认覆盖，不执行未请求的远程命令。
- 不提交代码，不停止或覆盖用户运行中的 EXE。

## Tasks

- [x] 1. 添加路径、POSIX cd 和文件任务测试，观察失败；实现有界参数验证及安全引用。
- [x] 2. 抽取 SSH transport；实现 SFTP 后台任务、进度、取消、临时文件提交及已认证公钥固定。
- [x] 3. 增加隔离 SFTP fixture 及文件往返/覆盖/取消集成测试；验证终端未被传输干扰。
- [x] 4. 新建 FileBrowserPanel，接入会话标签、收起菜单、宽度、上传下载对话框和显式 cd。
- [x] 5. 增加异步 MCP 文件工具、输入校验、本机路径界面批准、任务查询与取消测试。
- [x] 6. Release / CTest / 集成 / PE 检查；更新 README 与验证报告，发布独立版本到 dist。桌面交互验收留给用户，不在后台自动打开窗口。
