# 深大随连 · SZUConnect

深圳大学校园网连接助手，适用于粤海、丽湖校区的教学／办公区和宿舍区，支持 macOS 与 Windows。

- **检测与登录**：检查能否上网，按需完成校园网登录认证；掉线后可自动重连。
- **自动找网**：电脑未连接网络时，搜索并连接已记住的校园 Wi-Fi，也可允许应用自动打开 Wi-Fi 开关。
- **开机自启**：随系统启动，关闭窗口后仍可在托盘或菜单栏运行。
- **网址与记录**：添加常用网址进行检测，查看连接记录；使用代理或 VPN 时，可通过网络详情排查问题。

## 下载

| 平台 | 安装包 | 系统要求 |
| --- | --- | --- |
| macOS | [下载 DMG](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-macOS-universal.dmg) | macOS 14+，Apple 芯片或 Intel |
| Windows | [下载 EXE](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-win-x64.exe) · [ZIP](https://github.com/zhihebai/SZUConnect/releases/download/v0.10.1/SZUConnect-0.10.1-32-win-x64.zip) | Windows 11 x64 |

macOS 版已签名并通过 Apple 公证。Windows 版还没有签名，首次打开时可能出现系统提示。

Windows 下载 EXE 后运行，就能安装或升级；macOS 把应用拖进“应用程序”即可。升级会保留账号、设置和网络记忆。[安装与升级](docs/INSTALLATION.md)

## 开始使用

1. 先在系统里连上校园 Wi-Fi 或网线。
2. 跟着引导保存校园账号，点“检测连接”，需要认证时再登录。
3. 点“核验并记住校园网络”，再开启“自动重连”。Wi-Fi 和网线要分别记住。

需要自动找网时，在设置里选中已记住的校园 Wi-Fi，再开启“断连时，自动找回记住的校园 Wi-Fi”。允许打开 Wi-Fi 开关是独立选项。已连接其他 Wi-Fi、手机热点或网线时，应用不会切换网络。

[使用教程](docs/USER-GUIDE.md)

## 界面

| macOS | Windows |
| --- | --- |
| <a href="docs/images/macos-connection.png"><img src="docs/images/macos-connection.png" alt="macOS 连接页" width="360"></a> | <a href="docs/images/windows-connection.png"><img src="docs/images/windows-connection.png" alt="Windows 连接页" width="360"></a> |

## 隐私与来源

账号保存在系统钥匙串或 Windows 凭据管理器里，设置和记录都存在本机。应用没有广告，不收集使用统计，也不会修改 VPN、代理、DNS 或路由。

项目由 zhihebai 维护，采用 MIT 许可。SRun 的实现参考并改编自 ATMLuck/AutoSZUWeb，详见[来源与许可](THIRD-PARTY-NOTICES.md)。本项目是非官方工具，与深圳大学及运营商没有隶属关系。

遇到问题，欢迎到 [Issue](https://github.com/zhihebai/SZUConnect/issues) 反馈；如果它帮到了你，也欢迎点个 Star。

[开发文档](docs/README.md)
