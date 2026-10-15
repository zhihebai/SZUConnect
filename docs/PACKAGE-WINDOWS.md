# 深大随连 Windows 版

适用于 Windows 11 x64，无需安装额外运行库。运行下载的 EXE，或完整解压 ZIP 后运行 `SZUConnect.exe`，应用会安装到当前用户的程序目录，并创建开始菜单入口。

升级时直接运行新 EXE，应用会退出旧实例、更新并重新打开，保留已保存的账号、设置和网络记忆。

Windows 包未代码签名，首次打开可能出现系统提示。请确认来源并遵守设备安全策略。

## 使用

1. 先通过系统接入校园 Wi-Fi 或网线。
2. 打开应用检测，按需保存账号、核对区域并登录。
3. 需要自动恢复时，记住网络并开启“自动重连”。

自动找网仅恢复你选定的校园 Wi-Fi；自动打开 Wi-Fi 电源另行选择。连接其他网络时不会抢网。

关闭窗口后应用留在托盘，从托盘菜单“退出”才会关闭。程序与“说明与许可”保存在 `%LOCALAPPDATA%\Programs\SZUConnect`；设置保存在 `%LOCALAPPDATA%\SZUConnect`，账号保存在 Windows 凭据管理器。删除程序目录不会清除这些数据。

[安装、升级与卸载](https://github.com/zhihebai/SZUConnect/blob/main/docs/INSTALLATION.md)

[使用与排查](https://github.com/zhihebai/SZUConnect/blob/main/docs/USER-GUIDE.md)

本项目为非官方工具，与深圳大学及运营商无官方隶属关系。许可与来源资料保存在“说明与许可”文件夹。
