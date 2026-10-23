# 图标资源

应用图标由 Wi-Fi 信号和连接底座组成，通过项目内几何生成器生成，小尺寸版本适当加粗。菜单栏使用单色模板图标，Windows 托盘按任务栏主题选择资源。

| 命令 | 作用 |
| --- | --- |
| `python3 tools/icon_source.py` | 生成 `shared/icons/` 的线性 SVG |
| `python3 tools/make_icons.py` | 从几何母版生成 SVG、PNG、ICNS 与 ICO |
| `python3 tools/make_icons.py --windows-only` | 只更新 Windows ICO |
| `python3 tools/generate_shared.py` | 生成平台图标路径表 |

普通构建使用已提交的资源。重新生成位图需要 Pillow、Node.js 和 `sharp` 模块；可通过 `SZU_SHARP` 指定模块位置。修改后检查小尺寸、浅色／深色主题和菜单栏效果。

Windows ICO 各尺寸使用无损 PNG 编码，保留原有像素和透明度。原生测试检查应用及托盘资源的各档尺寸、透明度和系统加载接口。
