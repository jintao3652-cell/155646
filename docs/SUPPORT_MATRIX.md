# 支持矩阵

当前阶段**只支持/验证 Windows 10/11 x64**；Linux/macOS 暂时搁置，代码与构建脚本保留，但不纳入 CI/发布。

| 平台 | 客户端 | XConnect（X-Plane 12） | MConnect（MSFS 2020/2024） |
|---|---|---|---|
| Windows 10/11 x64 | ✅ | ✅ | ✅ |
| Linux x64 | ⏸ 暂停 | ⏸ 暂停 | ❌ SimConnect/Win32 |
| macOS arm64/x64 | ⏸ 暂停 | ⏸ 暂停 | ❌ SimConnect/Win32 |

> Linux/macOS 的 CMake 分支（LIN/APL、`-lm`、`-undefined dynamic_lookup`）仍保留，
> 便于以后恢复；当前 CI 只跑 Windows。

## 客户端

- Python 运行时随包提供（onedir 优先；onefile 仅便携）。
- WebView：Windows WebView2、Linux WebKitGTK、macOS WKWebView。
- 配置/日志可写目录优先 `%LOCALAPPDATA%` / XDG / `~/Library`。

## 模拟器插件

### XConnect（X-Plane）

- Windows `win_x64/XConnect.xpl`（`/MT` 静态 CRT）。
- Linux ELF `.xpl`（`-fPIC`，CMake/Makefile）。
- macOS universal `.xpl`（`arm64 + x86_64`，hardened runtime + notarize）。

### MConnect（MSFS）

- Windows only（SimConnect + Win32）。
- 独立 exe，不是 Community 插件；MobiFlight WASM 模块可选放入 Community。

## 功能边界

- MSFS 不支持注入管制模型：`Send:ATC` 只记录数量。
- Linux/macOS 不提供 MConnect。
- CSL 包不随安装包分发；由用户/工具单独管理。
