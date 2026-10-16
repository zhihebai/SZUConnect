# 深大随连 Windows 版

适用于 Windows 11 x64，无需安装额外运行库。运行下载的 EXE，或解压 ZIP 后运行 `SZUConnect.exe`，即可安装应用并创建开始菜单入口。

升级前建议先从托盘退出应用，再运行新 EXE。程序会完成更新并重新打开，保留账号、设置和网络记忆，无需查找旧程序目录。

Windows 版尚未签名，首次打开可能出现系统提示。请确认文件来自本项目，并遵守设备的安全策略。

## 使用

1. 先通过系统接入校园 Wi-Fi 或网线。
2. 跟随引导保存校园账号，点击“检测连接”，需要认证时再登录。
3. 选择“核验并记住校园网络”，开启“自动重连”。Wi-Fi 与网线分别记住。

需要自动连接校园 Wi-Fi 时，在设置中开启“自动找网”；是否允许自动打开 Wi-Fi，可另行选择。已接入其他 Wi-Fi、热点或网线时，应用不会另行找网。

关闭窗口后应用留在托盘；从托盘菜单选择“退出”才会完全关闭。

程序与“说明与许可”保存在 `%LOCALAPPDATA%\Programs\SZUConnect`；设置保存在 `%LOCALAPPDATA%\SZUConnect`，账号保存在 Windows 凭据管理器。删除程序不会清除账号和设置。

[安装、升级与卸载](https://github.com/zhihebai/SZUConnect/blob/main/docs/INSTALLATION.md)

[使用教程](https://github.com/zhihebai/SZUConnect/blob/main/docs/USER-GUIDE.md)

本项目为非官方工具，与深圳大学及运营商无官方隶属关系。许可与来源资料保存在“说明与许可”文件夹。
