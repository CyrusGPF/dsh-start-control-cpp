# dsh-start-control（原生 Win32 C++ 版）

轻量级 DeepSeek Harness 控制中心。用于托盘运行 `dsh web`，避免每次手工打开终端。

## 当前功能

- 启动、停止、重启 DSH Web
- 直接通过 Windows TCP API 检测 3080 监听状态
- 捕获带 token 的本地网页地址并打开浏览器
- 运行日志保存到 `%LOCALAPPDATA%\dsh-start-control\logs\dsh-latest.log`
- 自动检测或手动输入 `dsh.cmd`、`npm.cmd` 路径
- 从 npm 查询 `@deepseek-ai/dsh` 可安装版本
- 选择目标版本进行升级或回退
- 切换成功后按选项恢复切换前的运行状态
- 系统托盘、关闭隐藏、单实例、Windows 登录启动

## 构建

需要 Windows x64、CMake 和 MinGW-w64。请确保 MinGW-w64 的 `bin` 目录已加入当前终端的 `PATH`：

```powershell
cmake -S . -B build -G "MinGW Makefiles"
cmake --build build --config Release
```

如果编译器没有加入 `PATH`，请在本机终端中按实际安装位置临时指定 CMake 编译器，不要将本机绝对路径写入 README 或提交到仓库。

Release 产物是 `build\dsh-start-control.exe`，静态链接 C++ 运行库，不需要随程序附带 MinGW DLL。目标电脑仍需自行安装 Node.js/npm 和 DeepSeek Harness；程序只负责调用用户配置的命令文件。

## 兼容性

程序继续使用旧版 C# 控制中心的窗口标题、单实例名、配置目录和日志目录，因此可以在两个版本之间回退使用。旧 C# 项目保持封版，不在本目录之外修改。
