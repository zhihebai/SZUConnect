# Windows 应用

## 安装与升级

下载并运行 EXE，应用会安装到 `%LOCALAPPDATA%\Programs\SZUConnect`，并创建开始菜单入口。

升级时直接运行新 EXE。程序会正常退出旧实例，更新应用及说明与许可，保留账号、设置和数据；失败时恢复旧程序。已启用的登录时启动和已识别的快捷方式会同步更新，未知文件与旧下载保留。

需要从指定目录运行时使用 `SZUConnect.exe --portable`。`--observe-only` 与离线自检不安装程序，也不改写快捷方式或启动项。

## 构建

具备 Visual Studio C++ 桌面组件、Windows SDK、CMake 和 Python 3 后，在仓库根目录运行：

```powershell
.\windows\SZUConnect\scripts\package-windows.ps1
```

脚本完成构建、离线检查和打包，产物保存在仓库外。显示缩放、无线设备和校园认证仍需实机验证。

源码位于 `SZUConnect/`，使用 C++20、Win32 与 Direct2D／DirectWrite。`core/` 实现连接规则，`platform/` 对接系统服务，`app/` 提供界面、托盘和辅助功能。

[构建与测试](../BUILD-WINDOWS.md) · [项目首页](../README.md)
