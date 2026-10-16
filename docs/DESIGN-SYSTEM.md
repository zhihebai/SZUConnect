# 设计与共享资源

设计参数的源文件为 `shared/design-tokens.json`，文案为 `shared/copy.zh-CN.json`，图标为 `shared/icons/`。生成器将资源转换为 Swift 与 C++ 定义。

颜色按浅色／深色主题及用途组织。状态同时使用文字、图标和颜色表达。字体与间距以共享参数为准，窗口尺寸按平台布局调整。

交互控件须提供可读名称、键盘焦点和清晰状态，并支持减少动态效果和 Windows 高对比度设置。界面变更需检查原生显示及读屏效果，HTML 预览仅用于讨论布局。

```sh
python3 tools/generate_shared.py --check
python3 tools/check_ui_resources.py
python3 tools/check_contrast.py
```
