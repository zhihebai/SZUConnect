# 教程校验

《深大随连项目说明与使用教程》采用项目数字签名，并附有 DigiCert 时间戳，用于核查发布后是否被修改。阅读和打印不受限制。

## 核对文件

下载 [PDF](https://github.com/zhihebai/SZUConnect/releases/download/v1.0.0/SZUConnect-Guide.pdf) 和[校验清单](https://github.com/zhihebai/SZUConnect/releases/download/v1.0.0/SHA256SUMS.txt)，计算 PDF 的 SHA-256，并与清单中的结果比较。

Windows PowerShell：

```powershell
Get-FileHash .\SZUConnect-Guide.pdf -Algorithm SHA256
```

macOS 终端：

```sh
shasum -a 256 SZUConnect-Guide.pdf
```

## 查看签名

在支持数字签名的 PDF 阅读器中打开文件，查看签名面板。签名名称为 `SZUConnect Documentation`，设置为签署后不允许修改内容。

本证书由项目自行签发，未经第三方身份认证，首次验证可能显示签名人身份未知。可下载[公开证书](https://github.com/zhihebai/SZUConnect/releases/download/v1.0.0/SZUConnect-Document-Signing.cer)，将其 SHA-256 与签名面板中的证书指纹及校验清单核对，无需将证书导入系统信任。

签名可以识别改动，不能阻止他人重制文档。正式文件以[项目发布页](https://github.com/zhihebai/SZUConnect/releases/tag/v1.0.0)为准。
