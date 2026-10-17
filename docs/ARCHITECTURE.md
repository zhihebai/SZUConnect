# 架构

## 共享层

`shared/` 定义公共文案、设计参数、图标、官方链接与测试样例。`tools/generate_shared.py` 生成 Swift 和 C++ 资源，`--check` 检查生成结果是否与源文件一致。

## macOS

| 模块 | 职责 |
| --- | --- |
| `AppModel` | 在主线程管理应用状态 |
| `ConnectionPolicy`、`RecoveryHolds` | 管理提交、暂停、冷却和取消 |
| `PortalClient` | 发现登录页面，完成校园认证 |
| `NetworkIdentity`、`NetworkMemory` | 观察网络变化，管理自动恢复资格 |
| `NetworkLabels`、`WiFiRecovery` | 保存显示名称，按用户设置搜索并连接校园 Wi-Fi |

`ConnectionHealth`、`InternetCheck` 和 `WebsiteDiagnostics` 汇总检测结果。SwiftUI 页面读取状态及共享资源。SRun 编码核心保留 C++ 实现。

iOS 工程用于 Swift/C++ 代码复用探索，尚不属于正式交付平台。[iOS 构建](../BUILD-MAC.md#ios-代码复用与编译探索)

## Windows

- `app/`：Win32 窗口、Direct2D 场景、托盘、UI Automation 与控制器。
- `core/`：可移植的协议编码、检测汇总、恢复规划、网络记忆与状态呈现。
- `platform/`：WinHTTP、网络观察、系统凭据、安全存储与开机启动。

界面线程和工作线程分离；取消令牌关闭未完成请求的句柄，并以消息通知界面更新。

启动参数在构造控制器前解析。`RuntimeServices` 分别提供三种运行环境：

- 正式运行：使用 Windows 服务。
- 只读检测：不建存储目录、不移动损坏文件，不写设置、凭据或启动项。
- 离线自检：使用内存假服务，不构造正式存储、网络请求或系统监视器。

网站详情在内存中保留本次结果及所用代理设置。修改设置不改变旧结果，网址与详情不写入事件日志或诊断导出。

`--offline-smoke` 启动原生窗口，执行限时模拟认证并绘制三个页面，写入独立 JSON 结果后退出。它检查程序运行及测试环境隔离，真实网络、系统权限和桌面辅助功能需另行实机验收。

## 操作归属

换网、休眠、账户编辑和取消会使旧操作失效。异步结果返回后，先确认它仍属于当前网络、账户和操作，再更新状态。已提交的旧操作按结果不明处理，继续保留保护。

## 测试

Swift 离线测试使用注入服务，C++ 核心使用对应测试样例。`tools/reference/` 的独立 Python 模型复核规则，协议标准样例交叉检查两端编码结果。系统授权、真实校园服务、切网、原生界面与签名发行需单独验证。
