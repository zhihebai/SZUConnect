# 开发文档

日常使用见[使用教程](USER-GUIDE.md)，开发资料按需查阅：

| 任务 | 文档 |
| --- | --- |
| 编译、签名与打包 | [macOS](../BUILD-MAC.md) · [Windows](../BUILD-WINDOWS.md) |
| Windows 设备验收 | [实机测试](../WINDOWS-TESTING.md) |
| 认证与自动恢复 | [网络规则](NETWORK-BEHAVIOR.md) · [连接流程](CONNECTION-FLOW.md) · [物理通道](CAMPUS-PHYSICAL-CHANNEL.md) · [校园直连设置](PROXY-DIRECT.md) |
| 代码结构 | [架构](ARCHITECTURE.md) |
| 调整界面资源 | [设计参数](DESIGN-SYSTEM.md) · [图标](ICONS.md) · [界面布局](WINDOWS-UI-PARITY.md) |
| 查看平台功能 | [平台差异](PLATFORM-DIFFERENCES.md) · [macOS](../macos/README.md) · [Windows](../windows/README.md) |
| 提交改动 | [贡献指南](../CONTRIBUTING.md) · [开发约定](../AGENTS.md) · [源码检查](PUBLIC-SOURCE.md) |

平台代码在 `macos/` 和 `windows/`，共享资源在 `shared/`。目前发布 macOS 和 Windows 版，iOS 目标用于代码复用和编译测试。
