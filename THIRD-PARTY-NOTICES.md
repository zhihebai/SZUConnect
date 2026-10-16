# 第三方声明

深大随连（SZUConnect）采用 [MIT 许可](LICENSE)，与深圳大学及上游项目无官方隶属关系。

## AutoSZUWeb

- 来源：[ATMLuck/AutoSZUWeb](https://github.com/ATMLuck/AutoSZUWeb/tree/7fed1a589f5bd1a42d77e323a85046c8a95a1a8f)。
- Copyright (c) 2026 ATMLuck。
- 许可：MIT；[完整许可文本](docs/licenses/AutoSZUWeb-MIT.txt)。
- 用途：macOS 与 Windows 的 SRun 协议兼容实现。

上游完整许可同时作为 `macos/SZUConnect/Resources/AutoSZUWeb-LICENSE.txt` 随 Apple 平台应用分发。再分发相关代码或实质部分时，应保留上游版权和完整许可文本。

## 系统框架与开发工具

Apple 平台使用 Apple 系统框架；Windows 使用 Windows SDK 和 MSVC 运行库。它们分别受供应商条款约束，项目 MIT 许可不改变供应商条款。

校园物理通道在 macOS 上调用系统 libcurl，在 Windows 上静态链接 libcurl，来源与许可见下节。图标由仓库内几何生成器生成。

## AutoSZUWeb MIT 许可全文

```text
MIT License

Copyright (c) 2026 ATMLuck

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## libcurl

Windows 的校园直连通道静态链接 libcurl 8.22.0，使用 Windows Schannel 和系统证书验证。构建从固定版本的[上游源码](https://curl.se/download/curl-8.22.0.tar.xz)生成运行库。

完整版权及许可见 [curl COPYING](docs/licenses/curl-COPYING.txt)，Windows 便携包同时包含 `curl-COPYING.txt`。
