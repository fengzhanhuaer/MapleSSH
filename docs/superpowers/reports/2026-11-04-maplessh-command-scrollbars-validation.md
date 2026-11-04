# 命令框滚动条移除

日期：2026-11-04。

## 原因与修改

命令框原来使用 wxTE_DONTWRAP，即原生横向滚动条；wxTE_RICH2 在默认纵向滚动配置下还会启用 ES_DISABLENOSCROLL，使没有足够内容时也显示禁用滚动条。

改用局部 CommandTextCtrl 子类，取消不换行模式并使用 wxTE_NO_VSCROLL。在原生创建样式中明确移除 WS_HSCROLL、WS_VSCROLL，同时保留 ES_AUTOVSCROLL，避免仅禁用纵向滚动条后失去自动跟随输入的能力。子类采用两段创建方式，保证创建时应用自身的原生样式覆盖。

长行按控件宽度显示折行，不主动改写命令文本；多行输入、回车发送后清空和既有快捷命令均保留。只改变命令输入框，不修改终端或快捷按钮列表的滚动逻辑。

## 验证

- Release 编译成功，现有 13/13 CTest 通过。
- x64 PE 检查通过，仅依赖 Windows 系统 DLL；交付文件与构建输出散列一致；git diff --check 通过。
- 全程后台执行，未启动 GUI 或操作用户桌面；本次未新增 UI 自动化用例、未重复网络联调。实际无滚动条显示、自动折行、长文本滚轮浏览、末尾输入跟随等仍待人工验收；现有测试通过不等同于这些桌面验收。

## 交付

- MapleSSH/dist/MapleSSH-20261104-r6.exe，9,109,504 字节。
- SHA-256：cba63910c491af78d9ab9de99947ee0736514191e24bd944ab247584de6176a8。
- MapleSSH/dist/MapleSSH-20261104-r6.exe.sha256。
