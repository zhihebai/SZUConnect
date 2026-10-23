# 第三方声明

深大随连（SZUConnect）采用 [MIT 许可](LICENSE)。以下列出第三方来源及许可。

## AutoSZUWeb

- 来源：[ATMLuck/AutoSZUWeb](https://github.com/ATMLuck/AutoSZUWeb/tree/7fed1a589f5bd1a42d77e323a85046c8a95a1a8f)。
- 许可来源：[上游 LICENSE](https://github.com/ATMLuck/AutoSZUWeb/blob/fccd0efd7998dca0a26e76184e7117518bcb5ca2/LICENSE)。
- Copyright (c) 2026 ATMLuck。
- 许可：MIT；[完整许可文本](docs/licenses/AutoSZUWeb-MIT.txt)。
- 用途：macOS 与 Windows 的 SRun 协议兼容实现，其中 XEncode 编码核心保留了上游改编。

Apple 平台应用内附 `AutoSZUWeb-LICENSE.txt`，源文件位于 `macos/SZUConnect/Resources/`。分发相关代码或实质部分时，应保留上游版权及完整许可。

macOS 与 Windows 均在应用内附带项目和第三方完整许可，可在“设置 → 关于 → 开源许可”中离线展开阅读、复制。

## 系统框架与开发工具

Apple 平台使用系统框架，Windows 使用 Windows SDK 和 MSVC 运行库，均按各自的供应商条款使用。

校园直连在 macOS 上调用系统 libcurl，在 Windows 上静态链接 libcurl，来源与许可见下文。图标由本项目的几何图形生成器生成。

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

macOS 的校园直连通道动态调用系统提供的 libcurl，由 macOS 维护其版本和系统依赖；应用不打包 Windows 的静态运行库。项目 MIT、AutoSZUWeb MIT 与 curl 的完整许可均作为 `.app` 资源保存。

Windows 的校园直连通道静态链接 libcurl 8.22.0，使用 Windows Schannel 和系统证书验证。构建从固定版本的[上游源码](https://curl.se/download/curl-8.22.0.tar.xz)生成运行库。

完整版权及许可见 [curl 许可](docs/licenses/curl-COPYING.txt)及[上游许可说明](https://curl.se/docs/copyright.html)。

## yaml-cpp

- 来源：[jbeder/yaml-cpp 0.9.0](https://github.com/jbeder/yaml-cpp/tree/yaml-cpp-0.9.0)。
- 许可：MIT；[完整许可文本](docs/licenses/yaml-cpp-MIT.txt)。
- 用途：Windows 校园直连设置中的 YAML 配置解析。

Windows 程序内附完整版权和许可，可在“设置 → 关于 → 开源许可”中查看、复制。
