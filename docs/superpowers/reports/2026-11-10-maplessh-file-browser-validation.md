# MapleSSH 文件浏览器交付验证

日期：2026-11-10（Asia/Singapore）。

## 实现范围

- 每会话右侧深色文件浏览器：路径、上一级、刷新、目录双击、上传、下载、进度、取消，支持分隔线调宽和收起/展开（视图菜单 / Ctrl+Shift+F），当前运行保留宽度。隐藏不停止传输。
- 独立后台 SFTP，认证前固定检查当前已认证终端公钥；当前连接凭据快照复用，终端断开/重连取消文件任务，不跨线程访问同一 libssh2 session。
- 文件浏览与 shell 目录分离；只有明确点击并确认“终端进入此目录”才发送 POSIX cd，路径单引号/元字符正确引用，重新连接后拒绝使用旧面板任务上下文。
- 普通文件上传下载、默认拒绝覆盖、临时文件提交、确认后原子覆盖；不删除旧文件以模拟覆盖。提交中断提示检查实际结果，不自动重试。上传取消/断网可能残留临时文件，尽力清理不保证成功。
- MCP 增加 list_directory、upload_file、download_file、get_file_job、cancel_file_job，共 11 工具。传输异步返回任务 ID，访问本机文件须逐次界面批准，旧批准不能跨重连使用。
- MCP UI 桥接等待可中断，HTTP 大响应完整发送；关闭服务可打断空闲客户端等待。

## 验证执行

1. 路径测试先失败：file_paths.hpp 不存在；实现后通过。覆盖中文、空格、单引号、shell 元字符、父目录、NUL/控制字符、非法 UTF-8、过长路径，以及 Windows 保留设备名、ADS、相对路径拒绝。
2. 文件任务测试先失败：sftp_client.hpp 不存在；实现后通过。无公钥、非法本机/远程路径、停止后提交、未知任务和序列化边界。
3. MCP 文件发现测试先失败：MCP file tool missing；实现后通过。HTTP 初始化、通知、11 工具发现、文件参数校验与回调分发、非法 Origin、400000 字符响应、启停重启。另捕获 idle client 导致 Stop 等待约 10 秒，改为非阻塞且可中断收发后连续 3 次通过。
4. 最新 Release：cmake --build --preset release 成功（日志 MapleSSH/out/files-delivery-build.log）。原有 wxWidgets CRT 弃用告警不影响构建。
5. 最新 CTest：16/16，通过（ctest --preset release）。
6. 最新隔离集成：MapleSSH/scripts/integration.ps1 成功（日志 MapleSSH/out/files-delivery-integration.log）。fixture 仅回环监听，SFTP 为内存文件系统，不访问真实服务器或生产凭据。
   - 固定公钥错误时认证计数不增加。
   - 中文含引号目录/文件名、512 KiB 二进制上传下载内容逐字节相同。
   - 本机/远程已有目标拒绝覆盖，明确覆盖后再下载内容正确。
   - 传输中取消 8 MiB 上传，旧目标保持原内容；下载取消不产生最终文件。
   - 正在上传时终端输入/实际回显仍正常；断开取消任务，终止耗时小于测试 3 秒界限。
   - 不存在目录/下载目录返回失败而不终止交互终端。
   - 原有 SSH 主机拒绝、密码/私钥认证、重连、UTF-8、WebDAV 上传恢复/权限/重定向/取消/超时回归全部通过。
7. 最终 EXE PE 检查：x64，全部导入为 Windows 系统 DLL，无 wxWidgets、OpenSSL、libssh2 或 C/C++ 运行库附带 DLL。
8. git diff --check 通过；未提交代码，未修改 MapleStore，未停止用户运行中的 MapleSSH-20261109-r2.exe。

## 发布产物

- MapleSSH/dist/MapleSSH-20261110-r1.exe
- 9425920 字节。
- SHA-256：ef55d18c7204d5ca1bab64f6881b4a681404c419d7f9da322768570995816e34
- 校验文件：MapleSSH/dist/MapleSSH-20261110-r1.exe.sha256。
- 发布副本与当前 Release 构建产物 SHA-256 一致。

## 验证边界与限制

- 未自动打开 GUI 或点击用户窗口；右侧布局、DPI、收起/恢复宽度、系统文件选择器、实际 cd 确认、MCP 非模态批准窗口需用户启动新版后人工验收。
- MCP HTTP 测试使用无 GUI 回调 fixture，真实主窗口桥接和本机批准交互未声称已点验；旧版正在运行的 MCP 未被替换或调用新工具。
- SFTP 独立 SSH 认证的隔离测试已执行；真实服务器是否启用 SFTP、是否支持原子覆盖扩展及实际权限仍以目标服务器为准。
- 当前仅单文件，不递归、不跟随文件符号链接、不提供传统 SCP、断点续传或终端 CWD 自动双向同步。MCP 仍仅回环、无访问令牌，不应代理给其他机器；本机文件传输增加逐次批准。
- 配置与备份格式未改变；文件内容、目录位置和任务不进入 WebDAV 配置备份。
