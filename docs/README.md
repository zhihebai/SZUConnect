# 开发文档

日常使用见[使用手册](USER-GUIDE.md)。开发资料按任务查阅：

| 任务 | 文档 |
| --- | --- |
| 编译、签名与打包 | [macOS](../BUILD-MAC.md) · [Windows](../BUILD-WINDOWS.md) |
| Windows 设备验收 | [实体测试](../WINDOWS-TESTING.md) |
| 理解认证与自动恢复 | [网络行为](NETWORK-BEHAVIOR.md) |
| 理解代码结构 | [架构](ARCHITECTURE.md) |
| 调整界面资源 | [设计参数](DESIGN-SYSTEM.md) |
| 提交改动 | [贡献指南](../CONTRIBUTING.md) |

平台代码位于 `macos/` 和 `windows/`，共享资源位于 `shared/`。当前交付桌面应用；保留的 iOS 目标用于代码复用探索。
