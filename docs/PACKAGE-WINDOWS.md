# 深大随连 Windows 版

适用于 Windows 11 x64，不用另外安装运行库。运行下载的 EXE，或解压 ZIP 后运行 `SZUConnect.exe`，就能安装应用并创建开始菜单入口。

升级时运行新 EXE，程序会正常退出旧版、完成更新并重新打开，保留账号、设置和网络记忆，无需查找旧程序目录。

Windows 版尚未签名，首次打开可能出现系统安全提示。请从本项目下载。

## 使用

1. 先通过系统连上校园 Wi-Fi 或网线。
2. 跟着引导保存校园账号，点“检测连接”，需要认证时再登录。
3. 点“核验并记住校园网络”，再开启“自动重连”。Wi-Fi 和网线要分别记住。

需要自动找网时，在设置里选中已记住的校园 Wi-Fi，再开启“自动找网”。电脑未连接 Wi-Fi 或网线时，应用会搜索并连接所选 Wi-Fi；也可单独允许应用打开 Wi-Fi 开关。已有 Wi-Fi、热点或网线连接时，应用不会切换网络。

设置中还可开启“开机启动”。常用网址检测、连接记录及网络排查工具，可按需使用。

关掉窗口后，应用留在托盘；从托盘菜单选“退出”，才会完全关闭。

程序和“说明与许可”在 `%LOCALAPPDATA%\Programs\SZUConnect`；设置在 `%LOCALAPPDATA%\SZUConnect`，账号在 Windows 凭据管理器里。删除程序不会清掉账号和设置。

[安装与升级](https://github.com/zhihebai/SZUConnect/blob/main/docs/INSTALLATION.md) · [使用教程](https://github.com/zhihebai/SZUConnect/blob/main/docs/USER-GUIDE.md)

本项目是非官方工具，与深圳大学及运营商没有隶属关系。许可与来源资料放在“说明与许可”文件夹里。
