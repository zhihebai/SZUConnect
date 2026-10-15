# 反馈问题与参与改进

欢迎通过 Issue 反馈问题、提出建议或补充说明，也欢迎提交文档修正与代码改进。

## 反馈问题

反馈时请提供以下信息：

- 使用的系统和应用版本。
- 操作步骤，以及问题出现在哪一步。
- 预期结果与实际表现。

截图或报错有助于定位问题，分享前请隐藏账号、密码、网络名称等个人信息。涉及账号泄露或安全漏洞时，请按[安全报告方式](SECURITY.md)处理。

## 修改代码或文档

文档修正和代码改进均可提交 Pull Request。较大改动建议先通过 Issue 讨论方案。

提交时简要说明改动内容、原因和验证结果。界面改动需检查实际显示效果，平台功能需在对应系统上编译和测试。离线测试通过，不代表真实校园网也已验证。

开发时请注意：

- 文案、图标和设计参数从 `shared/` 修改，再生成平台资源。
- 修改登录、自动重连或数据保存时，保留现有安全检查和数据兼容，补充测试并同步网络说明。具体约定见 [AGENTS.md](AGENTS.md)。
- 测试使用假账号和隔离数据；真实登录、切网或系统授权测试，先征得设备使用者同意。

<details>
<summary>提交前的基础检查命令</summary>

在仓库根目录运行：

```sh
python3 -B tools/generate_shared.py --check
python3 -B tools/reference/verify_fixtures.py
python3 -B tools/check_ui_resources.py
python3 -B tools/public_scan.py
python3 -B -m unittest discover -s tools -p 'public_*_tests.py'
```

平台构建和测试方法见[开发文档](docs/README.md)。

</details>

贡献内容按项目的 MIT 许可分发。请只提交你有权分享的内容，并保留第三方署名和许可。
