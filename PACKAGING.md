# 打包成 exe（Windows / PyInstaller）

XFlySim-Client 是纯 Python 项目（依赖仅 `pywebview`），用 PyInstaller 打包。
前端 `webui/dist` 必须作为**数据文件**随 exe 一起打包，配置 `config.json`
运行时自动生成在 exe 同目录。

## 1. 一次性准备

```powershell
# Python 3.10+（开发环境为 3.12）
pip install pyinstaller pywebview
```

> `requirements.txt` 中的 `pywebview` 是运行期依赖，打包工具 `pyinstaller` 单独装。

## 2. 一键打包

```powershell
powershell -ExecutionPolicy Bypass -File build_exe.ps1
```

脚本依次：构建前端（`webui` → vite build）→ PyInstaller onefile 打包 →
产物 `dist\XFlySimClient.exe`。

目录版（启动更快、便于排查）：

```powershell
powershell -ExecutionPolicy Bypass -File build_exe.ps1 -OneDir
# 产物 dist\XFlySimClient\XFlySimClient.exe
```

## 3. 手动命令（等价，供定制）

```powershell
cd webui; npm ci; npm run build; cd ..

python -m PyInstaller --onefile --noconfirm --clean --windowed `
  --name XFlySimClient --distpath dist `
  --add-data "webui\dist;webui\dist" `
  --hidden-import webview.platforms.edgechromium `
  --collect-all webview `
  webapp.py
```

关键参数说明：

| 参数 | 作用 |
|---|---|
| `--add-data "webui\dist;webui\dist"` | 前端产物随包；`webapp.py` 用 `sys._MEIPASS` 定位 |
| `--hidden-import webview.platforms.edgechromium` | Windows pywebview 的 Edge/Chromium 后端 |
| `--collect-all webview` | 收集 pywebview 全部子模块（含各平台后端与依赖 DLL） |
| `--windowed` | 无控制台窗口（GUI 应用） |

## 4. 产物说明

- **config.json**：打包后自动读写 **exe 同目录**（`sys.executable` 旁），首次
  运行生成；源码运行时仍在仓库根目录。升级 exe 不会丢配置。
- **webui/dist**：内嵌于 exe（onefile 运行时解包到临时目录 `_MEIPASS`）。
- **无数据库**：状态全在内存，仅 config.json 持久化。
- **运行时依赖**：目标机需 **WebView2 Runtime**（Windows 10/11 自带；Server
  版需微软官网安装）。缺失时启动报错"WebView2 not found"。

## 5. 其他入口

- **tkinter 界面** `gui.py`：`--windowed gui.py`（无 WebView2 依赖，纯标准库，
  体积更小）。
- **命令行版** `main.py`：去掉 `--windowed` 保留控制台输出。

## 6. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 双击无反应 | 缺 WebView2；或改用 `gui.py` 打包 |
| 杀软误报 | onefile 自解压特征，加白名单或改 `--onedir` |
| 首次启动慢 | onefile 每次解包；改 `--onedir` |
| 前端改动不生效 | 必须先 `npm run build`，`webui/dist` 才会被 `--add-data` 收进包 |
| 端口占用/插件无数据 | 插件单客户端，exe 与 webapp/bridge 不要同时连 51001 |

## 7. 更新发版流程

1. 改代码 → `python -m xfly_client.selftest` 自检；
2. 前端改动先 `cd webui && npm run build`；
3. 重跑 `build_exe.ps1` → 覆盖 `dist\XFlySimClient.exe`；
4. 分发 exe（用户首次运行会在同目录生成 config.json，填呼号/CID/密码即可）。