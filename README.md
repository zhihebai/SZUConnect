# 深大随连 · SZUConnect

深圳大学校园网连接助手，支持 macOS 与 Windows。检测联网、完成校园认证，并在掉线后按设置恢复连接，适用于教学／办公区和宿舍区。

## 下载

| 平台 | 安装包 | 系统要求 |
| --- | --- | --- |
| macOS | [下载 DMG](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-macOS-universal.dmg) | macOS 14+，Apple 芯片或 Intel |
| Windows | [下载 EXE](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-win-x64.exe) · [ZIP](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-win-x64.zip) | Windows 11 x64 |

Mac 应用已签名并通过 Apple 公证。Windows 包未代码签名，首次打开可能出现系统提示。

Windows 下载并运行 EXE，即可安装或升级；macOS 替换“应用程序”中的旧应用。已保存的账号、设置和网络记忆会保留。[升级说明](docs/INSTALLATION.md#升级)

## 开始使用

1. 在系统中连接校园 Wi-Fi 或网线。
2. 打开应用，保存校园账号，检测并按需登录。
3. 需要自动恢复时，记住当前校园网络并开启“自动重连”。

需要找回无线连接时，再选择已记住的校园 Wi-Fi 并开启“自动找网”；自动打开 Wi-Fi 电源另行选择。

[使用与排查](docs/USER-GUIDE.md)

## 界面

| macOS | Windows |
| --- | --- |
| <a href="docs/images/macos-connection.png"><img src="docs/images/macos-connection.png" alt="macOS 连接页" width="360"></a> | <a href="docs/images/windows-connection.png"><img src="docs/images/windows-connection.png" alt="Windows 连接页" width="360"></a> |

## 隐私与来源

账号由系统钥匙串或 Windows 凭据管理器保存，设置与记录保存在本机。无独立遥测或广告，不修改 VPN、代理、DNS 或路由。

由 zhihebai 维护，采用 MIT 许可。SRun 实现参考并改编自 ATMLuck/AutoSZUWeb，详见[来源与许可](THIRD-PARTY-NOTICES.md)。本项目为非官方工具，与深圳大学及运营商无官方隶属关系。

[开发文档](docs/README.md)
