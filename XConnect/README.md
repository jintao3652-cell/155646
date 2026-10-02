# XConnect — X-Plane 数据桥接与 CSL 渲染插件

`XConnect` 是一个原生 X-Plane 11/12 插件（XPLM C API），功能：

1. **桥接数据**：在 X-Plane 内读取本机飞行数据，通过 TCP 实时发送给外部客户端
   （Python/XFlySim-Client 等）
2. **回传渲染**：接收客户端推送的在线玩家/管制列表，用 XPLMInstance 将 CSL
   模型渲染到 X-Plane 场景中（其他玩家的飞机实时显示、跟随移动）

与 ISFP-xLink 插件协议兼容（JSON 行协议），是纯 Python 客户端的可选数据
源增强：相比 UDP RREF，插件方式无需配置 X-Plane 数据输出，且能渲染 CSL。

## 目录结构

```
XConnect/
  src/
    xconnect.c       插件入口：flight loop、数据读取/发送、玩家管理
    json.c / json.h  轻量 JSON 解析/序列化助手
    net.c / net.h    TCP 跨平台封装（WinSock / POSIX）
    csl.c / csl.h    CSL 模型加载与 XPLMInstance 实例渲染
  CMakeLists.txt     跨平台构建
  Makefile           Linux/macOS 简单构建
  PROTOCOL.md        TCP JSON 协议文档
  README.md          本文件
  CSL/               存放 CSL 模型包（如 X-CSL），见下文
```

## 编译要求

- X-Plane SDK（含 `XPLM.h`、`XPLMInstance.h`、`lib/XPLM.lib`）
  下载：https://developer.x-plane.com/sdk/ （或 GitHub: github.com/x-plane/xplm）
- Windows：Visual Studio / Build Tools；Linux：gcc；macOS：clang

### CMake 构建（推荐）

```bash
cmake -B build -DXPlaneSDK=<SDK路径>
cmake --build build --config Release
# 产物: build/XConnect.xpl
```

### GitHub Action（一键编译 + 上传）

仓库已内置 `.github/workflows/build-xconnect.yml`：

1. 把项目推送到 GitHub 仓库（需 `git init` + 首次 push）
2. Action 在 `ubuntu-latest`（gcc）与 `windows-latest`（MSVC）上自动编译
3. 编译产物 `XConnect.xpl` 作为 artifact 上传，可直接下载

Linux 构建自动从官方头文件仓库拉取 SDK；Windows 需要官方 SDK 包的
`XPLM.lib`，若候选下载地址失效，请在仓库 **Settings → Secrets** 配置
`XPLANE_SDK_URL` 指向 SDK zip 直链。

### Linux 简单构建

```bash
make SDK=<SDK路径>
# 产物: XConnect.xpl
```

## 安装

1. 将编译产物 `XConnect.xpl` 拷贝到
   `X-Plane 目录/Resources/plugins/XConnect/`
   （Windows 上目录名任意，`.xpl` 为 64 位插件文件）
2. 将 CSL 模型包放入 `Resources/plugins/XConnect/CSL/`，
   目录结构支持两种格式：
   - `CSL/{ICAO}.obj`（如 `CSL/B738.obj`）
   - `CSL/{包名}/{ICAO}.obj`（如 `CSL/X-CSL/CSL_B738/B738.obj`）
3. 启动 X-Plane，插件自动监听 TCP 端口 **51001**
4. 启动客户端并连接

> CSL 模型包可从 fly.vatsim.net / X-CSL 分发获取（自由使用协议因包而异）。

## 客户端配合

插件启动后等待客户端连接。客户端需要：

- 连接 `127.0.0.1:51001`
- 接收 `connected` 与 `flight_data` 消息（本机飞行数据，5Hz）
- 发送 `Send:OPD:FSD9` 推送在线玩家（含位置/姿态/机型 ICAO）→ 插件渲染 CSL
- 发送 `Send:ATC` 推送管制列表（可附带，用于标注）
- 断开连接时插件清空已渲染模型

完整报文见 `PROTOCOL.md`。仓库根目录现有 Python 客户端可参考
`tools/xconnect_bridge.py`（将 XFlySim-Client 的 FSD 数据转发给插件）。

## 备注

- 渲染使用 `XPLMInstance`（实例化模型），性能开销低，支持大量玩家
- 玩家模型动画（襟翼/起落架/灯光/应答机）由 CSL 模型声明的 dataref 驱动，
  遵循 XPMP2 / X-CSL 约定
- 插件版权：GPL-3.0（与 XFlySim-Client 一致）