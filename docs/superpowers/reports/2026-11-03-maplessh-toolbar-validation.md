# 工具栏图标比例修正

日期：2026-11-03。

## 修改

工具栏原来调用未指定尺寸的 wxArtProvider::GetBitmap，各图标使用提供方默认尺寸，工具栏随后按图标尺寸决定整体高度。改为所有按钮均请求 16 × 16 DIP 的 wxBitmapBundle，并使用 FromDIP 设置工具栏图标尺寸，保持文字标签与系统 DPI 适配。

## 验证

- Release 编译成功；11/11 CTest 通过。
- x64 PE 导入检查通过，仅依赖 Windows 系统 DLL。
- 交付文件与本次构建输出 SHA-256 一致，git diff --check 通过。
- 全程后台操作；未关闭、覆盖用户运行中的 r2 程序，未启动 GUI，实际显示效果尚未人工验收。

## 交付

- MapleSSH/dist/MapleSSH-20261103-r3.exe。
- 大小：9,035,776 字节。
- SHA-256：d092ae4adc514e50178ca0992b5fc94e80c00705c465beb694a0d7fdc7112fb2。
- 散列文件：MapleSSH/dist/MapleSSH-20261103-r3.exe.sha256。
