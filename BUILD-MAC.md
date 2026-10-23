# 构建 macOS 版

本文说明 macOS 版的编译、预览与分发。

## 环境

- macOS 14+。
- Xcode 15.4+，含 macOS SDK；普通编译无需 iOS SDK。
- Python 3，用于工程生成、资源检查与打包测试。
- Node.js，用于校园直连助手的固定脚本隔离测试。

校园通道使用系统 libcurl（SDK `-lcurl`）。普通编译使用仓库内已有图标；重新生成位图时需要 Pillow、Node.js 和 `sharp`。

最低运行版本由工程配置和 `Info-*.plist` 定义。应用标识与系统凭据标识须保持兼容，避免影响已有数据访问。

## 编译

```sh
cd macos/SZUConnect
python3 Scripts/generate_project.py
bash Scripts/compile-local-mac.sh
```

也可在 Xcode 中打开 `SZUConnect.xcodeproj`，选择 `SZUConnect-macOS` 构建方案（scheme）。

编译脚本只生成未签名应用，不安装或启动。macOS Release 使用体积优先的 `-Osize` 和全模块优化；签名前移除本地符号和调试映射。调试符号另存为 `.dSYM`，不放进安装包，排查崩溃时应与对应构建一起保留。协议、传输、自动核验及双连接回归使用相同的 Swift 优化参数。

完整许可由工程直接引用根目录 `LICENSE`、`macos/SZUConnect/Resources/AutoSZUWeb-LICENSE.txt` 和 `docs/licenses/curl-COPYING.txt`，在应用内离线展开阅读、复制。更新许可后重新生成工程并检查分发包资源。macOS 调用系统 libcurl，无需打包 Windows 静态 libcurl 及其运行库。

单项测试见 `Tests/run-*.sh`。以下回归覆盖受限解析、定期核验、配置事务及帮助呈现，均使用假服务或临时数据，不改系统网络：

```sh
bash Tests/run-bounded-resolution.sh
bash Tests/run-automatic-campus-verification.sh
bash Tests/run-proxy-assistant.sh
bash Tests/run-help-presentation.sh
bash Tests/run-network-details-presentation.sh
bash Tests/run-diagnostic-regressions.sh
bash Tests/run-cumulative-recovery.sh
bash Tests/run-manual-network-context-model.sh
```

配置助手测试仅执行项目生成的固定脚本与合成对象，不执行用户选中的脚本。打包脚本测试：

```sh
python3 Tests/test_artifact_scripts.py
python3 Tests/test_dmg_layout.py
```

## 只读与离线预览

- `--observe-only`：检测真实网络，不读取钥匙串或请求 Wi-Fi 权限，也不提交账号密码、写入正式数据；主题可临时切换，退出后失效。
- `--offline-preview`：使用隔离演示状态，不执行真实认证或网络恢复。

HTML 预览需要 Node.js、Playwright 和 Chromium。设置 `SZU_PREVIEW_BROWSER_CHANNEL=chrome` 可使用已安装的 Chrome。预览使用独立临时浏览器配置，不读取个人登录状态。

## 分发

公开分发使用发行者自己的 Developer ID 签名并完成 Apple 公证，保留定位权限配置（Location entitlement）与用途说明。Apple Development 和 ad hoc 签名仅用于开发测试。

以下命令在 `macos/SZUConnect` 目录运行。尖括号内容需替换为应用路径和仓库外的输出目录。打包不会安装或启动应用，也不覆盖已有输出。

### 本地打包

先为源应用做 ad hoc 签名，再打包。此方式不使用发行证书，也不包含 Apple 公证。

```sh
codesign --force --sign - --options runtime --timestamp=none \
  --entitlements SZUConnect-macOS.entitlements \
  '<app-path>/SZUConnect.app'
codesign --verify --deep --strict '<app-path>/SZUConnect.app'
python3 Scripts/package-dmg.py \
  --app '<app-path>/SZUConnect.app' \
  --output '<output-dir>/SZUConnect-macOS.dmg' \
  --sign-identity -
```

`--sign -` 与 `--sign-identity -` 表示 ad hoc 签名。脚本要求源应用已签名且保留定位权限，再为暂存副本签名。核对两种架构的代码段及挂载、复制后的签名后，生成 DMG 与回执。

### Developer ID 分发

源应用已用 Developer ID 签名并完成公证后，可生成保留签名的 DMG：

```sh
python3 Scripts/package-dmg.py \
  --app '<app-path>/SZUConnect.app' \
  --output '<output-dir>/SZUConnect.dmg' \
  --distribution --preserve-signature --require-notarized-app
```

脚本在源应用、暂存、挂载和复制阶段检查应用签名与公证票据。完整许可随 `.app` 的资源提供，打包时核对原始字节。DMG 显示应用和“应用程序”快捷入口，用箭头及中文提示引导拖动安装。安装盘使用应用图标，标题包含版本号。背景由系统框架绘制，窗口布局随镜像写入，不依赖 Finder 自动操作或额外的 Python 包。外层 DMG 的签名、公证需另行完成和核验。

正式下载包的签名与公证状态见下载页。安装步骤见[安装说明](docs/INSTALLATION.md)。

## iOS 代码复用与编译探索

工程保留 `SZUConnect-iOS` scheme，最低目标为 iOS／iPadOS 17，仅用于代码复用与编译探索，尚未发行。

安装 iOS 与 iOS Simulator SDK 后，在 Xcode 中选择该构建方案。先检查模拟器编译，再按需验证设备签名、界面、权限与校园认证。

菜单栏、登录项、Wi-Fi 找网及后台运行需按 iOS 重新适配。编译完成后，设备功能仍需逐项验证。

可选扩展脚本会运行离线回归，并构建 macOS、iOS 设备与 iOS 模拟器目标：

```sh
cd macos/SZUConnect
bash Scripts/build.sh
```

该脚本需要相应 SDK，普通 macOS 编译无需运行。可选环境变量：

- `SZU_OUTPUT_DIR`：输出目录。
- `SZU_TEAM`：iOS 开发签名团队。
- `SZU_SIGN_IDENTITY`：macOS 本机签名证书。

证书、团队信息与私钥请保留在本机，不要写入源码。
