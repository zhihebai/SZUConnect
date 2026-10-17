# 反馈问题与参与改进

欢迎到 Issue 反馈问题、提建议，也欢迎一起改进文档和代码。

## 反馈问题

反馈时，请带上这些信息：

- 用的系统和应用版本。
- 操作步骤，以及问题出现在哪一步。
- 预期结果和实际表现。

可以附截图或报错，但分享前请隐藏账号、密码、网络名称等个人信息。账号泄露或安全漏洞，请按[安全报告](SECURITY.md)的说明处理。

## 修改代码或文档

文档修正和代码改进都可以提 Pull Request。改动较大时，建议先在 Issue 里讨论方案。

提交时简单说明改了什么、为什么改、验证结果如何。界面改动要检查实际显示效果；平台功能要在对应系统上编译和测试；在真实校园网里测过的话，请单独注明结果。

开发时有几点要注意：

- 文案、图标和设计参数从 `shared/` 改，再生成平台资源。
- 改登录、自动重连或数据保存时，保留安全检查和数据兼容，补上测试并同步说明。详见[开发约定](AGENTS.md)。
- 测试用假账号和隔离数据；真实登录、切网或系统授权测试，先征得设备使用者同意。

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
