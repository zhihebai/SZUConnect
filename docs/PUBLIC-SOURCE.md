# 公开源码检查

发布前在仓库根目录运行，产物放在源码树外：

```sh
python3 tools/public_scan.py
python3 -B -m unittest discover -s tools -p 'public_*_tests.py'
python3 tools/public_package.py --output '<output-dir>/SZUConnect-source.zip'
```

扫描器检查私人记录、本机路径、密钥、签名材料、构建产物和符号链接，只报告类别与相对位置。测试假值和固定校园服务地址不作为秘密断言。

`--deny-file` 可加载本机禁用词列表，`--json` 可输出报告；这些文件都应保存在仓库外。打包器复核源码内容，拒绝覆盖已有输出或携带仓库外文件。

扫描不能替代人工审阅。系统应用标识用于数据兼容，不代表个人账号；第三方版权和许可必须保留。
