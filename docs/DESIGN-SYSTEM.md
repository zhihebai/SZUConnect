# 设计与共享资源

设计参数的源文件为 `shared/design-tokens.json`，文案为 `shared/copy.zh-CN.json`，图标为 `shared/icons/`。生成器将资源转换为 Swift 与 C++ 定义。

颜色按浅色／深色主题及语义角色组织。状态使用文字、图标和颜色共同表达，不能只依赖颜色。字体与间距以 token 值为准；平台窗口尺寸允许受原生布局约束影响。

所有交互控件应有可读名称、键盘焦点和清晰状态。尊重减少动态效果和 Windows 高对比度设置。界面变更同时检查真实平台渲染及读屏行为；HTML 预览用于布局讨论，不能替代原生界面验证。

```sh
python3 tools/generate_shared.py --check
python3 tools/check_ui_resources.py
python3 tools/check_contrast.py
```
