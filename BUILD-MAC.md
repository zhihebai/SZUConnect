# 构建 macOS 版

本文说明 macOS 版的编译、预览与分发。

## 环境

- macOS 14+。
- Xcode 15.4+，含 macOS SDK；普通编译无需 iOS SDK。
- Python 3，用于工程生成、资源检查与打包测试。

校园通道使用 macOS 自带的 libcurl（SDK `-lcurl`），不附带代理引擎或外部命令行工具。普通编译使用仓库内已有图标；重新生成位图时才需要 Pillow、Node.js 和 `sharp`。

最低运行版本由工程配置和 `Info-*.plist` 定义。应用标识与系统凭据标识须保持兼容，避免影响已有数据访问。

## 编译

```sh
cd macos/SZUConnect
python3 Scripts/generate_project.py
bash Scripts/compile-local-mac.sh
```

也可在 Xcode 中打开 `SZUConnect.xcodeproj`，选择 `SZUConnect-macOS` scheme。

编译脚本只生成未签名应用，不安装或启动。脚本会移除可执行文件中的本地调试映射；此步骤完成后，再签名和打包。

单项测试见 `Tests/run-*.sh`。打包脚本测试：

```sh
python3 Tests/test_artifact_scripts.py
```

## 只读与离线预览

- `--observe-only`：检测真实网络，不读取钥匙串或请求 Wi-Fi 权限，也不提交账号密码、写入正式数据；主题可临时切换，退出后失效。
- `--offline-preview`：使用隔离演示状态，不执行真实认证或网络恢复。

可选 HTML 预览需要 Node.js、Playwright 和 Chromium。设置 `SZU_PREVIEW_BROWSER_CHANNEL=chrome` 可使用已安装的 Chrome；预览使用独立临时浏览器资料，不读取个人登录状态。

## 分发

公开分发使用发行者自己的 Developer ID 签名并完成 Apple 公证，保留定位权限配置（Location entitlement）与用途说明。Apple Development 和 ad hoc 签名仅用于开发测试。

以下命令在 `macos/SZUConnect` 目录运行。尖括号内容为占位符，请替换为应用路径和源码仓库外的输出目录。脚本拒绝覆盖已有输出，不会安装或启动应用。

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
  --sign-identity - \
  --notices '../../docs/DISTRIBUTION-NOTICES.txt'
```

`--sign -` 与 `--sign-identity -` 表示 ad hoc 签名。打包脚本验证源应用签名与定位权限，为暂存副本签名，并核对两种架构的代码段及挂载、复制后的签名，最后生成 DMG 与回执。未签名应用会被拒绝。

### Developer ID 分发

源应用已用 Developer ID 签名并完成公证后，可生成保留签名的 DMG：

```sh
python3 Scripts/package-dmg.py \
  --app '<app-path>/SZUConnect.app' \
  --output '<output-dir>/SZUConnect.dmg' \
  --distribution --preserve-signature --require-notarized-app \
  --notices '../../docs/DISTRIBUTION-NOTICES.txt'
```

脚本在源应用、暂存、挂载和复制阶段检查签名与公证票据，并分别验证应用和外层 DMG 的公证。

正式下载包的签名与公证状态见下载页。安装步骤见[安装说明](docs/INSTALLATION.md)。

## iOS 代码复用与编译探索

工程保留 `SZUConnect-iOS` scheme，最低目标为 iOS／iPadOS 17，仅用于代码复用与编译探索，尚未发行。

安装 iOS 与 iOS Simulator SDK 后，在 Xcode 中选择该 scheme。先检查模拟器编译，再按需验证设备签名、界面、权限与校园认证。

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
