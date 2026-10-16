# 图标资源

图标由项目内几何生成器维护。应用图标使用 Wi-Fi 信号与连接底座；小尺寸版本加粗以保持可辨认。菜单栏使用单色模板图标，Windows 托盘按任务栏主题选择资源。

| 命令 | 作用 |
| --- | --- |
| `python3 tools/icon_source.py` | 生成 `shared/icons/` 的线性 SVG |
| `python3 tools/make_icons.py` | 从几何母版生成 SVG、PNG、ICNS 与 ICO |
| `python3 tools/generate_shared.py` | 生成平台图标路径表 |

普通构建使用已提交的资源。重新生成位图需要 Pillow、Node.js 和 `sharp` 模块；可通过 `SZU_SHARP` 指定模块位置。修改后检查小尺寸、浅色／深色主题和菜单栏效果。
