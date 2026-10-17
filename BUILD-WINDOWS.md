# 构建 Windows 版

Windows 版使用 C++20 与 Win32，界面基于 Direct2D／DirectWrite，并提供 UI Automation 辅助功能支持。

MSVC 运行库与固定版本的 libcurl 静态链接，证书由系统 Schannel 验证。首次构建需联网获取依赖源码，发行包无需额外 DLL。

编译器支持时，MSVC Release 构建启用跨文件优化。

## 环境

- Windows 11 x64。
- Visual Studio 2022 的“使用 C++ 的桌面开发”，包含 MSVC v143、Windows SDK 和 CMake 3.21+。
- Python 3，用于共享资源、测试数据和 ZIP 完整性检查。脚本统一使用 `-B -X utf8`，避免生成缓存或受终端编码影响。

## 一次构建与打包

在仓库根目录的 Developer PowerShell for VS 2022 中运行：

```powershell
.\windows\SZUConnect\scripts\package-windows.ps1
```

默认构建目录位于系统临时目录，按源目录区分；报告与包分别位于其中的 `reports` 和 `dist`。构建、报告和输出目录均须在仓库外。再次打包时指定新的输出目录，保留已有产物。

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

`test-windows.ps1` 默认先构建；加 `-SkipBuild` 后仍运行核心测试、共享资源检查及原生离线自检。打包只使用 Release，已有构建时也可加 `-SkipBuild`。

缺少依赖、检查失败或回执无效时，脚本返回非零退出码。报告分别记录通过、失败和跳过的检查。

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

原生构建、UI Automation、凭据管理器、无线接口和校园认证需在 Windows 上验证。

## 原生离线自检

脚本运行 `--offline-smoke --smoke-output <绝对 JSON 路径>`，独立实例绘制连接、记录、设置三页并执行一次假认证。正式数据、凭据、联网、网络观察和开机启动写入的计数必须为零。

每次检查生成独立报告目录。通过条件：

- 进程退出码为零，回执版本匹配。
- 绘制与假认证检查通过。
- 正式服务的隔离计数均为零。

内部默认 10 秒截止，脚本外层 15 秒；超时或失败保留 stdout、stderr 及已有回执。核心测试的 JUnit 位于报告目录中的 `ctest-<Configuration>-<随机标识>/core-tests.xml`。

校园认证、无线接口及 VPN／代理环境下的恢复按[实机测试](WINDOWS-TESTING.md)验证。

## 发行包

文件名由 CMake 的四段版本生成，并核对资源文件、应用常量、manifest、EXE 版本资源及 x64 PE 标识。打包生成独立 EXE、ZIP 与回执，文件名以脚本输出为准。独立 EXE 须与 ZIP 内的程序字节一致。

程序内嵌使用说明与完整许可，独立 EXE 首次部署时写入“说明与许可”。ZIP 顶层为 `SZUConnect.exe` 和“说明与许可”文件夹，其中包含 README、LICENSE、完整第三方声明、原生离线回执和 `PACKAGE-MANIFEST.json`。构建与测试文档保留在仓库中。

打包使用独立临时目录，检查完整性后输出。保留已有包，失败只清理当前操作创建的临时或未完成产物。脚本不执行代码签名。

## 运行

- 普通启动时安装或更新 `%LOCALAPPDATA%\Programs\SZUConnect`，创建开始菜单入口，并迁移已启用的登录时启动与已识别的旧快捷方式。程序自动请求旧实例正常退出后更新，失败时恢复旧程序。未知文件与旧下载保留，不扫描其他目录。
- `--portable`：从当前目录运行，不自动部署。
- `--background`：仅显示通知区域图标。
- `--observe-only`：只检测，不提交账号密码，不部署程序或改写正式数据、快捷方式和启动项；离线自检同样不部署。
- 关闭主窗口后应用保留在托盘；从托盘菜单退出。
- 窗口内按 `Ctrl+Q` 可正常退出，输入框获得焦点时也可用。
- `Ctrl+1/2/3` 切换连接、记录和设置，`Ctrl+R` 或 `F5` 检测连接；输入框获得焦点时同样可用。
- 设置和记录保存在 `%LOCALAPPDATA%\SZUConnect`，账号保存在 Windows 凭据管理器，可在“设置 → 关于”中删除。
- 开机启动仅在用户启用设置后注册。

Windows 可找回已记住的开放校园 Wi-Fi，是否允许打开 Wi-Fi 电源单独设置。网络核验、权限及设备检查见[实机测试](WINDOWS-TESTING.md)。发行者使用自己的证书，并说明签名状态。

## Windows 本机验收

保留打包结果及离线回执，按[验收矩阵](WINDOWS-TESTING.md#验收矩阵)检查 Windows 11 实际桌面。CI 在 Windows Server 2022 上构建和测试，设备、权限及校园网络需单独验收。

只读与离线模式使用独立实例标识。实机检查先使用只读模式，再正常退出并按需使用普通模式。真实登录和自动恢复由账号持有人在相应校园网络中验证。
