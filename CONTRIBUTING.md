# 反馈问题与参与改进

欢迎通过 Issue 反馈问题或提出建议，也欢迎改进文档与代码。

## 反馈问题

反馈时请提供以下信息：

- 使用的系统和应用版本。
- 操作步骤，以及问题出现在哪一步。
- 预期结果与实际表现。

可附截图或报错，分享前请隐藏账号、密码、网络名称等个人信息。账号泄露或安全漏洞请按[安全报告](SECURITY.md)说明处理。

## 修改代码或文档

文档修正和代码改进均可提交 Pull Request。较大改动建议先通过 Issue 讨论方案。

提交时简要说明改动、原因和验证结果。界面改动需检查实际显示效果，平台功能需在对应系统上编译和测试；真实校园网测试请单独注明结果。

开发时请注意：

- 文案、图标和设计参数从 `shared/` 修改，再生成平台资源。
- 修改登录、自动重连或数据保存时，保留安全检查与数据兼容，补充测试并同步说明。详见[开发约定](AGENTS.md)。
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
