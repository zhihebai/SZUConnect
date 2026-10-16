# 构建 Windows 版

Windows 端使用 C++20、Win32、Direct2D／DirectWrite、UI Automation 和 Windows SDK，静态链接 MSVC 运行库。校园物理通道静态链接固定版本 libcurl，由 Windows Schannel 验证证书。

首次构建需联网获取固定源码，发行包无需额外 DLL。

MSVC Release 构建在编译器支持时启用跨文件优化，Debug 保持原有调试方式。

## 环境

- Windows 11 x64。
- Visual Studio 2022 的“使用 C++ 的桌面开发”，包含 MSVC v143、Windows SDK 和 CMake 3.21+。
- Python 3，用于共享资源、测试数据和 ZIP 完整性检查。脚本使用 `-B -X utf8`，不在源码中生成缓存，也不依赖终端代码页输出中文。

## 一次构建与打包

在仓库根目录的 Developer PowerShell for VS 2022 中运行：

```powershell
.\windows\SZUConnect\scripts\package-windows.ps1
```

默认构建目录位于系统临时目录，按源目录区分；报告和包分别位于其中的 `reports` 和 `dist`。三个目录均须在仓库外。已有产物不会被覆盖，再次打包可指定新的输出目录。

```powershell
$build = Join-Path $env:TEMP 'SZUConnect-build'
$reports = Join-Path $env:TEMP 'SZUConnect-reports'
$dist = Join-Path $env:TEMP 'SZUConnect-dist'
.\windows\SZUConnect\scripts\package-windows.ps1 -BuildDir $build -ReportDir $reports -OutputDir $dist
```

只构建或检查已有构建：

```powershell
.\windows\SZUConnect\scripts\build-windows.ps1 -Configuration Debug -BuildDir $build
.\windows\SZUConnect\scripts\test-windows.ps1 -Configuration Debug -BuildDir $build -ReportDir $reports -SkipBuild
```

`test-windows.ps1` 默认先构建；`-SkipBuild` 仍运行核心测试、共享资源检查及原生离线自检。打包仅使用 Release，默认构建并检查一次，已有构建时可加 `-SkipBuild`。

缺少依赖、检查失败或回执无效时返回非零。跳过的检查不能记为通过。

直接使用 CMake 时也指定仓库外目录：

```powershell
cmake -S windows/SZUConnect -B $build -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON
cmake --build $build --config Release
ctest --test-dir $build -C Release --output-on-failure
```

其他系统可检查业务核心：

```sh
cmake -S windows/SZUConnect -B /tmp/SZUConnect-core -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/SZUConnect-core
ctest --test-dir /tmp/SZUConnect-core --output-on-failure
```

核心测试与 mock SDK 语法检查不覆盖原生 Windows 构建、UI Automation、凭据管理器、无线接口或真实校园认证。

## 原生离线自检

脚本运行 `--offline-smoke --smoke-output <绝对 JSON 路径>`，独立实例绘制连接、记录、设置三页并执行一次假认证。正式数据、凭据、联网、网络观察和开机启动写入的计数必须为零。

每次生成独立报告目录，通过须同时满足：

- 进程退出码为零，回执版本匹配。
- 绘制与假认证检查通过。
- 正式服务的隔离计数均为零。

内部默认 10 秒截止，脚本外层 15 秒；超时或失败保留 stdout、stderr 及已有回执。核心测试的 JUnit 位于报告目录中的 `ctest-<Configuration>-<随机标识>/core-tests.xml`。

离线自检不提交真实账号，也不证明真实校园认证、无线接口或 VPN／代理恢复已经通过。

## 发行包

名称以 CMake 的四段版本为唯一输入，并核对资源文件、应用常量、manifest 和 exe 的版本资源及 x64 PE 标识。打包生成独立 EXE、可选 ZIP 与回执，EXE 与 ZIP 内的程序字节须一致，文件名以脚本输出为准。

程序内嵌使用说明与完整许可，独立 EXE 首次部署时写入“说明与许可”。ZIP 顶层仅包含 `SZUConnect.exe` 及该文件夹；README、LICENSE、完整第三方声明、原生离线回执和 `PACKAGE-MANIFEST.json` 位于文件夹内。构建及实体测试文档保留在 GitHub。

打包使用独立临时目录，检查完整性后输出。保留已有包，失败只清理当前操作创建的临时或未完成产物。脚本不执行代码签名。

## 运行

- 普通启动安装或更新 `%LOCALAPPDATA%\Programs\SZUConnect`，创建开始菜单入口，并迁移已启用的登录时启动与已识别的旧快捷方式。先正常退出旧实例，更新失败恢复旧程序；保留未知文件，不扫描或删除其他目录的下载。
- `--portable`：从当前目录运行，不自动部署。
- `--background`：仅显示通知区域图标。
- `--observe-only`：只检测，不提交账号，不部署程序或改写正式数据、快捷方式和启动项；离线自检同样不部署。
- 关闭主窗口后应用保留在托盘；从托盘菜单退出。
- 窗口内按 `Ctrl+Q` 可正常退出，输入框获得焦点时也可用。
- `Ctrl+1/2/3` 切换连接、记录和设置，`Ctrl+R` 或 `F5` 检测连接；输入框获得焦点时同样可用。
- 设置和记录保存在 `%LOCALAPPDATA%\SZUConnect`，凭据保存在 Windows 凭据管理器。
- 开机启动仅在用户启用设置后注册。

Windows 提供已记住开放校园 Wi-Fi 的可选找网及独立电源开关。来源核验、权限及设备检查见[实体测试](WINDOWS-TESTING.md)。发行者应使用自己的证书并说明签名状态。

## Windows 本机验收

先保留打包结果及离线回执，再按[验收矩阵](WINDOWS-TESTING.md#验收矩阵)检查 Windows 11 实际桌面。CI 使用 Windows Server 2022，不能替代设备、权限及校园网络测试。

只读与离线模式使用独立实例标识。设备验证先运行只读模式，再正常退出并按需使用普通模式；真实登录和自动恢复由账号持有人在适用校园网络中验证。
