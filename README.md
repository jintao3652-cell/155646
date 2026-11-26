# XFlySim-Client

- **HTML/CSS/JS 界面**（pywebview + 系统 WebView2，深色 bar 风格）
- 另有 Tkinter 深色界面（`gui.py`，纯标准库备选）与 CLI（`main.py`）
- 支持 **X-Plane 11 / 12**（`XConnect` 插件）与
  **Microsoft Flight Simulator 2020 / 2024**（`MConnect`），均为模拟器侧
  原生桥接：推送本机飞行数据、把在线玩家注入/渲染为可见飞机
- 服务器：`ol.xflysim.com:6809`（可在设置中修改）

## 功能

- 飞行员登录（`#AP`，Network Rating=1，Protocol Revision=9，GB18030 编码）
- 位置上报（`@` 包 5 秒一次，PBH 编码姿态，起落架近似 on_ground）
- 管制员/其他飞行员列表本地维护（`%`/`@`/`^`/`#SL`/`#ST`/`#DP`/`#DA` 包）
- 文本消息（私聊 / 频率通话 / 接收 MOTD），可收可回
- METAR 请求（`$AX`/`$AR`）
- 模型匹配应答（PI:GEN / FSIPI），自动请求并记录对方机型
- 60s 保活（`#TM...:SERVER:ping`），断线自动清理本地 roster
- VATSIM 风格认证（clientId=55538，MD5 challenge/response，`$ZC`/`$ZR`）
- **双桥接自动探测**：启动即同时监听 XConnect(XP) 与 MConnect(MSFS)，
  **任一在线即可连线**；飞行数据 5Hz 经桥接传输；在线玩家/管制列表推送给
  插件 → 在模拟器场景中渲染飞机

## 数据源：XConnect 插件（X-Plane 11 / 12）

客户端不再直连 X-Plane UDP，而是连接 X-Plane 内的 **XConnect 插件**
（TCP 51001，编译/安装见 `XConnect/README.md`）：

- **监听插件上线**：客户端启动即持续连接插件，插件晚启动也会自动连上
- **数据通道**：插件把本机飞行数据（位置/姿态/地速/应答机/频率等）以
  JSON 行实时推给客户端（5Hz），客户端据此上报位置
- **CSL 渲染**：客户端把在线玩家/管制 roster（含位置、姿态、机型 ICAO）
  推送给插件（`Send:OPD:FSD9` / `Send:ATC`），插件用 XPLMInstance 在
  X-Plane 中渲染 CSL 模型并跟随移动
- **连线规则**：客户端同时探测 XConnect 与 MConnect 端口，**任一在线即可
  连线 FSD**（XP11/XP12/MSFS2020/2024 三选一）；所有桥都离线时禁止连线，
  当前桥断开后自动断开 FSD 并复位界面

协议细节见 `XConnect/PROTOCOL.md`；`tools/xconnect_bridge.py` 为演示客户端。

## 数据源：MConnect（MSFS 2020 / 2024）

`MConnect/` 是同一套协议的 **Microsoft Flight Simulator** 实现，形态为外部
exe（SimConnect 官方推荐的 out-of-process 方式）：

- **协议完全兼容**：报文格式与 XConnect 一致，默认监听 `127.0.0.1:51002`
  （占用时退 51003/51004）。客户端启动即自动探测这些端口，**无需手动改端口**：
  X-Plane 与 MSFS 可同时开，哪个在线就连哪个（XConnect 优先）
- **双版本兼容**：`SimConnect.dll` 运行时动态装载，`_EX1` 创建接口与涂装枚举
  按能力探测——2020 走基础版 `AICreateNonATCAircraft`，2024 走 `_EX1` + 涂装，
  同一份 exe 两边都能跑
- **他机显示**：把在线玩家注入为模拟器里的真实 AI 飞机
  （`AICreateNonATCAircraft` + 每帧 `SetDataOnSimObject`）
- **反向控制**：客户端可设置应答机（码/模式/IDENT）与 COM1-3 频率
  （主频/备频/互换）——频率与应答机码是只读 SimVar，走 SimConnect 事件
- **机型匹配**：扫描 `Community`/`Official` 下全部 `aircraft.cfg` +
  `livery.cfg`（真实机器实测 7800+ 条），沿用 XConnect 的四级规则
  （尾号 → 航司 → 机型 → 同族别名）

```bash
cd MConnect
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --config Release
build\MConnect.exe            # 先启动 MSFS，再运行
build\MConnect.exe --scan-only  # 只看机型目录扫描结果（排查用）
```

详见 `MConnect/README.md` 与 `MConnect/PROTOCOL.md`。

## 目录结构

```
webapp.py          pywebview 应用入口（HTML 界面）
webui/
  src/              Vue3 + Element Plus 界面（工具栏 / 日志 / 设置 / 飞行计划）
  dist/             构建产物，随应用分发（npm run build）
_noise.py          过滤 pywebview 内部噪音日志（pythonnet 3.x 环境）
xfly_client/
  fsd/
    packet.py    FSD 报文构造 + PBH 编码 + 频率换算
    parser.py    全类型 FSD 报文解析
    auth.py      VATSIM 风格认证
    link.py      底层 TCP（GB18030 增量解码、CRLF 分帧）
    session.py   会话：登录、位置上报、保活、事件分发
  xconnect/
    client.py    XConnect 插件 TCP 客户端（自动重连监听上线、飞行数据、CSL roster 推送）
  model.py       roster 管理、机型应答、飞行数据 → FSD 上报、roster 推送插件
  registration.py 尾号归一化与「手动 > 模拟器 > 缓存」优先级
  simbrief.py    SimBrief 最新 OFP 导入（用户名 → 飞行计划表单字段）
  config.py      config.json 读写
  selftest.py    协议层自测
XConnect/        X-Plane 原生插件（数据桥接 + CSL 渲染），含 GitHub Action 编译
tools/
  xconnect_bridge.py   插件演示客户端（收数据/推演示玩家）
main.py           CLI 入口
start-client.bat  双击启动（RMP 面板界面）
start-client.ps1  启动脚本的实际逻辑（找 Python / 检查依赖 / 启动）
gui.py            Tkinter 深色界面（备选）
requirements.txt  依赖：pywebview
```

## 使用

1. 编译并安装 **XConnect 插件**（见 `XConnect/README.md`，或用 GitHub Action
   一键编译后拷贝 `XConnect.xpl`）与 CSL 模型包
2. 安装客户端依赖：`pip install -r requirements.txt`（Windows 10/11 自带 WebView2）。
   用启动脚本的话这一步可以跳过，缺什么它会自动装
3. 启动 X-Plane 11/12（插件自动监听 51001），再启动客户端：
   - **双击 `start-client.bat`**（推荐：自动找 Python、缺依赖自动装、不弹控制台）
   - HTML 界面：`python webapp.py`
   - Tkinter：`python gui.py`
   - CLI：`python main.py`

   启动脚本支持三个参数：`start-client.bat`（正常启动）、`start-client.bat debug`
   （带 DevTools 前台运行，能看报错）、`start-client.bat check`（只检查环境）。
   实际逻辑在同目录的 `start-client.ps1`。
4. 等待状态栏/胶囊显示插件上线（`PLG OK`）；未上线时点连线会被拒绝
5. 填写 **呼号、机型 ICAO、航司 ICAO（涂装）**，设置里填 **真实姓名、CID、密码**，点「连线」
6. 在线后状态栏显示频率、应答机、地速、高度；💬 消息面板收发文本；
   其他玩家飞机以 CSL 渲染在 X-Plane 场景中

CLI 示例：

```bat
python main.py --callsign CCA1234 --cid 1001 --password ***** ^
  --realname "张三" --aircraft A20N --airline CCA --sim xp12 ^
  --xconnect-host 127.0.0.1 --xconnect-port 51001
```

CLI 命令：`/text <呼号> <消息>`、`/freq <MHz> <消息>`、`/metar <ICAO>`、
`/pi <呼号>`、`/status`、`/help`、`/quit`

## 注册号（尾号）

飞行计划表单里有「注册号」输入框，留空即自动。取值优先级：

| 顺序 | 来源 | 说明 |
|---|---|---|
| 1 | 手动填写 | 飞行计划里的注册号，存 `config.registration`；清空 = 恢复自动 |
| 2 | 模拟器尾号 | 插件读 `sim/aircraft/view/acf_tailnum`（付费机模通常已填） |
| 3 | 模拟器涂装名 | 尾号为空时从 `acf_livery_path` 目录名提取（`.../B-1234 China Southern`） |
| 4 | 同机型上次值 | `config.registration_cache[机型]`，覆盖插件刚上线/切机模的空档 |
| 5 | 空 | **不伪造**：PI 的 `LIVERY` 退回航司码，对端 CSL 用「同航司同机型」 |

尾号随 `#SB PI:GEN` 的 `LIVERY` 字段广播（这样能被 swift 中继给第三方），并附带
`REG=` 扩展键；对端据此优先选「同一架飞机」的 CSL 涂装。界面右上角 `REG` 胶囊
显示当前生效值，悬停可见来源。

## SimBrief 导入

飞行计划页顶部的 **SimBrief** 一栏可直接按用户名拉取最新一份 OFP，自动填表：

- 输入 SimBrief **用户名**（或个人页地址里的数字 **用户 ID**），点「导入」或回车；
- 起降/备降场、航路、巡航高度（ft → `FL350`）、巡航 TAS、计划起飞时间（UTC）、
  航程/燃油时:分、机型与**注册号**都会填进表单，核对后可再改；
- 只填表、**不自动提交** $FP，避免拿到旧计划误发；
- 用户名会记到配置（`config.simbrief_user`），下次打开自动回填。

映射说明（`xfly_client/simbrief.py`）：航程优先用 `est_off → est_on` 时刻相减，
回退 `est_enroute`（`HH:MM` / `HHMM` / 秒 / 分钟都能解析）；燃油栏是**可用时间**
而非重量，取「总油量 ÷ 航段耗油 × 航程」，没有油耗数据时按航程 +45 分钟。
接口免登录（`api/xml.fetcher.php?...&json=1`），失败区分「用户不存在 / 没有计划」
「网络不通」「返回异常」三类提示。

## 自测

```bat
python main.py --selftest
```

覆盖 PBH 编码/解码、各 FSD 包构造/解析、频率换算、认证算法确定性、尾号优先级、
SimBrief 计划映射（68 项断言，纯离线，不联网）。

## 配置

软件根目录 `config.json`（含密码请勿外传）：

```json
{
  "callsign": "CCA1234",
  "cid": "1001",
  "password": "*****",
  "real_name": "quanquan",
  "aircraft_icao": "A20N",
  "airline_icao": "CCA",
  "remember_password": true,
  "server_host": "ol.xflysim.com",
  "server_port": 6809,
  "xconnect_host": "127.0.0.1",
  "xconnect_port": 51001,
  "auto_start_mconnect": true,
  "mconnect_stop_on_exit": false
}
```

`auto_start_mconnect`（默认 `true`）：客户端启动时自动拉起无窗口的
`MConnect.exe`（MSFS 桥接），已运行则复用、不重复启动；X-Plane 用户可设为
`false`。`mconnect_stop_on_exit`（默认 `false`）：退出客户端时是否关闭插件，
默认保留以便下次快速重连。诊断请用 `MConnectConsole.exe`（见 `MConnect/README.md`）。

## 协议要点（与 C# 版一致）

- 无 `$DI`/`$ID` 握手，TCP 连接后直接发送 `#AP`
- 文本编码 GB18030（兼容中文）
- 位置包：`@模式:呼号:应答机:等级:纬度:经度:高度:地速:PBH:修正`
- PBH 位域：bit1=on_ground，bit2-11 heading，bit12-21 bank，bit22-31 pitch
- X-Plane 数据单位换算：米→英尺（×3.28084）、m/s→节（×1.943844）
