# XFlySim-Client

- **HTML/CSS/JS 界面**（pywebview + 系统 WebView2，深色 bar 风格）
- 另有 Tkinter 深色界面（`gui.py`，纯标准库备选）与 CLI（`main.py`）
- 支持 X-Plane 11 / 12：数据经 **XConnect 插件**（X-Plane 原生插件）桥接，
  并可将在线玩家渲染为 CSL 模型
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
- **XConnect 插件桥接**：启动即持续监听插件上线；飞行数据 5Hz 经插件传输；
  在线玩家/管制列表推送给插件 → X-Plane 场景中渲染 CSL 飞机

## 数据源：XConnect 插件

客户端不再直连 X-Plane UDP，而是连接 X-Plane 内的 **XConnect 插件**
（TCP 51001，编译/安装见 `XConnect/README.md`）：

- **监听插件上线**：客户端启动即持续连接插件，插件晚启动也会自动连上
- **数据通道**：插件把本机飞行数据（位置/姿态/地速/应答机/频率等）以
  JSON 行实时推给客户端（5Hz），客户端据此上报位置
- **CSL 渲染**：客户端把在线玩家/管制 roster（含位置、姿态、机型 ICAO）
  推送给插件（`Send:OPD:FSD9` / `Send:ATC`），插件用 XPLMInstance 在
  X-Plane 中渲染 CSL 模型并跟随移动
- **连线规则**：插件未上线时禁止连线 FSD；插件断开（X-Plane 关闭）后
  自动断开 FSD 连接并复位界面

协议细节见 `XConnect/PROTOCOL.md`；`tools/xconnect_bridge.py` 为演示客户端。

## 目录结构

```
webapp.py          pywebview 应用入口（HTML 界面）
webui/
  index.html       界面结构
  style.css        深色主题
  app.js           前端逻辑（pywebview 桥接 + 事件轮询）
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
  config.py      config.json 读写
  selftest.py    协议层自测
XConnect/        X-Plane 原生插件（数据桥接 + CSL 渲染），含 GitHub Action 编译
tools/
  xconnect_bridge.py   插件演示客户端（收数据/推演示玩家）
main.py           CLI 入口
gui.py            Tkinter 深色界面（备选）
requirements.txt  依赖：pywebview
```

## 使用

1. 编译并安装 **XConnect 插件**（见 `XConnect/README.md`，或用 GitHub Action
   一键编译后拷贝 `XConnect.xpl`）与 CSL 模型包
2. 安装客户端依赖：`pip install -r requirements.txt`（Windows 10/11 自带 WebView2）
3. 启动 X-Plane 11/12（插件自动监听 51001），再启动客户端：
   - HTML 界面：`python webapp.py`（推荐）
   - Tkinter：`python gui.py`
   - CLI：`python main.py`
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

## 自测

```bat
python main.py --selftest
```

覆盖 PBH 编码/解码、各 FSD 包构造/解析、频率换算、认证算法确定性（34 项断言）。

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
  "xconnect_port": 51001
}
```

## 协议要点（与 C# 版一致）

- 无 `$DI`/`$ID` 握手，TCP 连接后直接发送 `#AP`
- 文本编码 GB18030（兼容中文）
- 位置包：`@模式:呼号:应答机:等级:纬度:经度:高度:地速:PBH:修正`
- PBH 位域：bit1=on_ground，bit2-11 heading，bit12-21 bank，bit22-31 pitch
- X-Plane 数据单位换算：米→英尺（×3.28084）、m/s→节（×1.943844）
