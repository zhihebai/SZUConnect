# 公开源码检查

在仓库根目录运行以下检查，源码包保存在仓库外：

```sh
python3 tools/public_scan.py
python3 -B -m unittest discover -s tools -p 'public_*_tests.py'
python3 tools/public_package.py --output '<output-dir>/SZUConnect-source.zip'
```

扫描器检查私人记录、本机路径、密钥、签名材料、构建产物和符号链接，只报告类别与相对位置。测试假值和固定校园服务地址不按私密信息处理。

使用 `--deny-file` 加载本机禁用词列表，`--json` 输出报告，相关文件保存在仓库外。打包器复核源码内容，拒绝覆盖已有输出或将仓库外文件打入源码包。

发布前人工复核扫描结果。系统应用标识用于数据兼容，第三方版权和许可按原文保留。
