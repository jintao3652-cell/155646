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
  --add-data "assets;assets" `
  --icon "assets\icon.ico" `
  --hidden-import webview.platforms.edgechromium `
  --collect-all webview `
  webapp.py
```

关键参数说明：

| 参数 | 作用 |
|---|---|
| `--add-data "webui\dist;webui\dist"` | 前端产物随包；`webapp.py` 用 `sys._MEIPASS` 定位 |
| `--add-data "assets;assets"` | 图标等资源随包（运行时窗口图标） |
| `--icon "assets\icon.ico"` | exe 内嵌图标：资源管理器/任务栏/任务管理器显示 |
| `--hidden-import webview.platforms.edgechromium` | Windows pywebview 的 Edge/Chromium 后端 |
| `--collect-all webview` | 收集 pywebview 全部子模块（含各平台后端与依赖 DLL） |
| `--windowed` | 无控制台窗口（GUI 应用） |

## 4. 产物说明

- **配置文件（自 v9 起为二进制）**：位于用户目录
  `%APPDATA%\XFlySim-Client\config.bin`（其它平台 `~/.config/xflysim-client/`），
  与 exe 位置无关——升级/分发不带走配置，也防止把整个软件目录拷贝给他人。
  - 格式：`pickle` 序列化后按**本机 MachineGuid** 做 XOR 混淆（前缀魔数 `XFC1`），
    复制到其它机器无法解密 → 自动回退默认配置（防共享/盗用，属混淆非加密）。
  - 首次运行发现旧版 `config.json`（仓库根 / exe 同目录）自动迁移并删除。
  - 写入为原子操作（临时文件 + `os.replace`），强杀进程不产生损坏配置。
- **webui/dist**：内嵌于 exe（onefile 运行时解包到临时目录 `_MEIPASS`）。
- **无数据库**：状态全在内存，仅配置文件持久化。
- **运行时依赖**：目标机需 **WebView2 Runtime**（Windows 10/11 自带；Server
  版需微软官网安装）。缺失时启动报错"WebView2 not found"。
- **XUpdate.exe**：`build_exe.ps1` 还会产出 `dist\XUpdate.exe`（独立更新器，
  无 WebView2 依赖），负责客户端与 MC/XC 插件的版本检查/下载/替换；
  用法与清单格式见 `XUpdate更新器.md`。CSL/FSLTL/AIG 不走这个通道。
  **必须与 `XFlySimClient.exe` 放在同一目录**：客户端在线更新（整包自更新）
  靠它完成"退出后替换 + 重启"，详见 `客户端在线更新.md`。

## 5. 独立更新器 XUpdate.exe

```powershell
XUpdate.exe --check          # 只检查，不写磁盘
XUpdate.exe --apply          # 下载 + 校验 + 备份 + 原子替换（失败回滚）
XUpdate.exe --apply-package JOB.json   # 客户端自更新用：等客户端退出后替换 exe
XUpdate.exe --sample-manifest
```

默认清单是 exe 同目录 `update/manifest.json`，可用 `--manifest` 指定本地路径
或 URL。更新 X-Plane 插件时用 `--xplane-dir`（或环境变量 `XPLANE_ROOT`）。
完整说明见 `XUpdate更新器.md`。

客户端自身的整包更新走另一条更短的通道（服务端一个 JSON 接口 + 一个 exe 直链），
由客户端写作业文件后拉起 `XUpdate.exe --apply-package`，见 `客户端在线更新.md`。

## 6. 其他入口

- **tkinter 界面** `gui.py`：`--windowed gui.py`（无 WebView2 依赖，纯标准库，
  体积更小）。
- **命令行版** `main.py`：去掉 `--windowed` 保留控制台输出。

## 7. 常见问题

| 现象 | 原因 / 处理 |
|---|---|
| 双击无反应 | 缺 WebView2；或改用 `gui.py` 打包 |
| 任务管理器显示 "python" | 说明正在**源码运行**（`python webapp.py`）；打包成 exe 后进程名即为 `XFlySimClient`，图标为 `assets\icon.ico` |
| 杀软误报 | onefile 自解压特征，加白名单或改 `--onedir` |
| 首次启动慢 | onefile 每次解包；改 `--onedir` |
| 前端改动不生效 | 必须先 `npm run build`，`webui/dist` 才会被 `--add-data` 收进包 |
| 端口占用/插件无数据 | 插件单客户端，exe 与 webapp/bridge 不要同时连 51001 |

## 8. 更新发版流程

1. 改代码 → `python -m xfly_client.selftest` 自检；
2. 前端改动先 `cd webui && npm run build`；
3. 重跑 `build_exe.ps1` → 覆盖 `dist\XFlySimClient.exe` 与 `dist\XUpdate.exe`；
4. 更新发布目录：把新文件与生成的 `manifest.json` 放到版本目录
   （模板见 `XUpdate.exe --sample-manifest`，格式见 `XUpdate更新器.md`）；
5. 分发 exe（配置在 `%APPDATA%\XFlySim-Client\config.bin`，
   用户首次运行生成后填写呼号/CID/密码即可）。