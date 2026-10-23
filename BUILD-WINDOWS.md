# 构建 Windows 版

Windows 版使用 C++20 与 Win32，界面基于 Direct2D／DirectWrite，并提供 UI Automation 辅助功能支持。

MSVC 运行库与固定版本的 libcurl、yaml-cpp 静态链接，证书由系统 Schannel 验证。首次构建需联网获取依赖源码，发行包无需额外 DLL。

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

窗口检查另覆盖反复从托盘唤回、最小化恢复及最大化状态保留，核对显示前的完整绘制与显示后的同步重绘。顶栏帮助与系统按钮之间保留 24 DIP 拖动区，并检查原生命中结果。

窗口按钮的悬停、按下和取消只更新按钮反馈。离线自检执行 200 轮交互，检查未重建页面，并保留输入框内容、选区和焦点；耗时另记入回执，不设机器相关的速度门槛。

离线界面检查还会以 125%、150%、175%、200% 配置原生 Direct2D／DirectWrite 渲染目标，在 480／500 DIP 和两档文字倍率下检查顶栏、三页布局及网络对比列。正常窗口默认宽度和最小客户端宽度均为 500 DIP，480 DIP 用于边界回归。字体测试另查实际字形、基线及 ClearType／灰阶策略；回执区分当前显示器 DPI 和配置目标，不代替跨屏及系统缩放切换实测。

内部默认 10 秒截止，脚本外层 15 秒；超时或失败保留 stdout、stderr 及已有回执。核心测试的 JUnit 位于报告目录中的 `ctest-<Configuration>-<随机标识>/core-tests.xml`。

校园认证、无线接口及 VPN／代理环境下的恢复按[实机测试](WINDOWS-TESTING.md)验证。

校园直连设置的配置变换和文件事务测试使用假配置与临时目录，覆盖规则顺序、DNS 模式、解析限制、加密备份、并发修改及撤销。自动测试不读取实际订阅，不改正在运行的代理配置，也不代替客户端重新加载后的实测。

## 发行包

下载文件名只保留产品版本，如 `SZUConnect-1.0.0-win-x64.exe`；构建号保留在内部版本元数据、诊断文件及打包回执中，界面只显示产品版本。打包核对资源文件、应用常量、manifest、EXE 版本资源及 x64 PE 标识，生成独立 EXE、内部验证 ZIP 与回执。Windows 公开下载仅提供 EXE。

内部 ZIP 仅包含 `SZUConnect.exe`，须与独立 EXE 字节一致，不对外发布。原生离线回执、独立包清单和校验文件保存在仓库外的构建产物中，不放入 ZIP。

程序内嵌使用说明、项目 MIT 许可、第三方声明及 curl 许可。第三方声明包含 AutoSZUWeb 和 yaml-cpp 的完整 MIT 文本。完整许可可在“设置 → 关于 → 开源许可”中离线查看、复制，便携模式同样可用，无需读取下载目录中的其他文件。

安装器仍将上述四份文档部署到 `%LOCALAPPDATA%\Programs\SZUConnect\current\开源许可`，沿用清单校验、升级备份和回滚流程。构建与测试文档保留在仓库中。

打包使用独立临时目录，检查完整性后输出。保留已有包，失败只清理当前操作创建的临时或未完成产物。脚本不执行代码签名。

## 运行

- 普通启动时安装或更新 `%LOCALAPPDATA%\Programs\SZUConnect`，创建开始菜单入口，并迁移已启用的登录时启动与已识别的旧快捷方式。程序自动请求旧实例正常退出后更新，失败时恢复旧程序。未知文件与旧下载保留，不扫描其他目录。
- 旧版“说明与许可”目录通过原有清单与哈希校验后，随程序更新为“开源许可”；旧程序及目录保留在升级备份中。
- `--portable`：从当前目录运行，不自动部署。
- `--background`：仅显示通知区域图标。
- `--observe-only`：只检测，不提交账号密码，不部署程序或改写正式数据、快捷方式和启动项；离线自检同样不部署。
- 关闭主窗口后应用保留在托盘；从托盘菜单退出。
- 窗口内按 `Ctrl+Q` 可正常退出，输入框获得焦点时也可用。
- `Ctrl+1/2/3` 切换连接、记录和设置，`Ctrl+R` 或 `F5` 检测连接；输入框获得焦点时同样可用。
- 设置和记录保存在 `%LOCALAPPDATA%\SZUConnect`，账号保存在 Windows 凭据管理器，可在“设置 → 关于”中删除。
- 开机启动仅在用户启用设置后注册。

Windows 可找回选定的已记住 Wi-Fi：开放网络使用临时连接，需要无线密码的网络使用 Windows 已保存的连接，不读取密码或改写无线配置。是否允许打开 Wi-Fi 电源单独设置。网络核验、权限及设备检查见[实机测试](WINDOWS-TESTING.md)。发行者使用自己的证书，并说明签名状态。

## 卸载

普通安装在当前用户的卸载注册项中登记应用。系统“设置 → 应用 → 已安装的应用 → 深大随连 → 卸载”和应用内“设置 → 关于 → 卸载应用”打开同一原生确认窗口。

卸载命令在创建正式运行时前处理。确认后正常退出应用，清理受管程序、所属快捷方式和开机启动项。默认保留账号及本机数据；勾选“同时删除已保存账号、设置、网络记忆和记录”后才清理凭据与数据目录。

删除前核对固定目录、清单及文件类型。遇到未知文件、链接、文件占用或权限错误时保留未完成部分，报告原因，允许重试。不扫描下载包、导出报告、源码或用户备份。

卸载前先确认退出后清理程序能够运行。清理程序等待本轮卸载实例退出，再核对并删除其临时 EXE 和空目录；不创建计划任务，不调整脚本执行策略。

自动卸载回归使用隔离目录、假账号和模拟数据，不接触正式凭据；临时清理程序就绪后，继续检查正式卸载所用的映像和目录校验。实机回归使用已备份的安装并保留本机数据，删除账号由用户确认。

## Windows 本机验收

保留打包结果及离线回执，按[验收矩阵](WINDOWS-TESTING.md#验收矩阵)检查 Windows 11 实际桌面。CI 在 Windows Server 2022 上构建和测试，设备、权限及校园网络需单独验收。

只读与离线模式使用独立实例标识。实机检查先使用只读模式，再正常退出并按需使用普通模式。真实登录和自动恢复由账号持有人在相应校园网络中验证。
