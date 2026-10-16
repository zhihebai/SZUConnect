# 深大随连 · SZUConnect

深圳大学校园网连接助手，适用于粤海、丽湖校区的教学／办公区和宿舍区，支持 macOS 与 Windows。

检测联网、登录校园网，并在掉线后自动重连。开启自动找网后，还可连接已记住的校园 Wi-Fi。

## 下载

| 平台 | 安装包 | 系统要求 |
| --- | --- | --- |
| macOS | [下载 DMG](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-macOS-universal.dmg) | macOS 14+，Apple 芯片或 Intel |
| Windows | [下载 EXE](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-win-x64.exe) · [ZIP](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-win-x64.zip) | Windows 11 x64 |

macOS 版已签名并通过 Apple 公证。Windows 版尚未签名，首次打开可能出现系统提示。

Windows 下载并运行 EXE，即可安装或升级；macOS 将应用拖入“应用程序”。升级会保留账号、设置和网络记忆。[安装与升级](docs/INSTALLATION.md)

## 开始使用

1. 在系统中连接校园 Wi-Fi 或网线。
2. 跟随引导保存校园账号，点击“检测连接”，需要认证时再登录。
3. 选择“核验并记住校园网络”，开启“自动重连”。Wi-Fi 与网线分别记住。

需要自动连接校园 Wi-Fi 时，在设置中开启“自动找网”；是否允许自动打开 Wi-Fi，可另行选择。

[使用教程](docs/USER-GUIDE.md)

## 界面

| macOS | Windows |
| --- | --- |
| <a href="docs/images/macos-connection.png"><img src="docs/images/macos-connection.png" alt="macOS 连接页" width="360"></a> | <a href="docs/images/windows-connection.png"><img src="docs/images/windows-connection.png" alt="Windows 连接页" width="360"></a> |

## 隐私与来源

账号保存在系统钥匙串或 Windows 凭据管理器中，设置与记录保存在本机。应用无广告，不收集使用统计，不修改 VPN、代理、DNS 或路由。

由 zhihebai 维护，采用 MIT 许可。SRun 实现参考并改编自 ATMLuck/AutoSZUWeb，详见[来源与许可](THIRD-PARTY-NOTICES.md)。本项目为非官方工具，与深圳大学及运营商无官方隶属关系。

欢迎通过 [Issue](https://github.com/zhihebai/SZUConnect/issues) 反馈问题，也欢迎点亮 Star 支持项目。

[开发文档](docs/README.md)
