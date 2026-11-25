# FSD 连接后上报流程（swift pilotclient 参考 + 本地实现对照）

> 参考源码：[swift-project/pilotclient](https://github.com/swift-project/pilotclient)
> `src/core/fsd/fsdclient.cpp`（连接/登录/定时器）；本地实现
> `xfly_client/fsd/session.py`（模型侧驱动见 `xfly_client/model.py`）。
> 协议细节（报文字段、PBH、频率偏移）见 `xfly_client/fsd/FSD_PROTOCOL.md`。

---

## 1. 上报内容清单（连接后）

| # | 上报项 | 报文 | 触发方式 | 频率/条件 | 本地是否实现 |
|---|--------|------|----------|-----------|------------|
| 1 | 飞行员登录 | `#AP` | TCP 连上立即 | 一次 | ✅ |
| 2 | 客户端标识 | `$ID` | VATSIM 专用 | 一次 | ○（私有网不用） |
| 3 | 位置更新 | `@`（PilotDataUpdate） | 定时器 | **每 5s**，需已有飞行数据 | ✅ |
| 4 | 中间位置 | `#SB VI` | 定时器 | 每 1s（需 INTERIMPOS 能力） | ○（只读不报） |
| 5 | 视觉位置 | `^` / `#SL` / `#ST` | 定时器 | 200ms（需 VISUPDATE+Velocity） | ○（不发送） |
| 6 | 机模配置 | `$CQ...:ACC` | 机模部件变化 | 100ms 检测（插件 5Hz 数据），5s 令牌桶，发 `@94836` | ✅ |
| 7 | 飞行计划 | `$FP` | 登录后/计划载入时（应用层） | 一次/次 | ✅（webapp 自动补交） |
| 8 | 机型信息应答 | `#SB PI/FSIPI` | 收到 `#SB PIR/FSIPIR` 请求 | 事件 | ✅ |
| 9 | Ping 应答 | `$PO` | 收到 `$PI` | 事件 | ✅ |
| 10 | 认证应答 | `$ZR` | 收到 `$ZC` 挑战 | 事件 | ✅ |
| 11 | 保活 | `$PI`（swift）/ `#TM SERVER ping`（本地） | 定时器 | 本地每 60s | ✅ |
| 12 | 注销 | `#DP` | 主动断开 | 一次（发后等 200ms） | ✅ |

## 2. 连接后时序

### 2.1 swift（Legacy FSD，协议版本 9）

```
TCP connect(host:6809)
   │
   ├─ 立即发送  #AP  ← 登录（无任何前置握手）
   │
   └─ 置 Connected，启动位置定时器群（startPositionTimers）
        ├─ m_positionUpdateTimer   5000ms → 发送 @
        ├─ m_interimPositionTimer  1000ms → 发送 #SB VI（能力开启时）
        ├─ m_visualPositionTimer   200ms  → 发送 ^（能力开启时）
        ├─ m_scheduledConfigUpdate 100ms  → 机模部件变化检查
        └─ m_fsdSendMessageTimer   10ms   → 队列批量出队（单次 ≤6 条）

事件驱动（连接后随时）：
  收到 $PI → 回 $PO
  收到 $ZC → 回 $ZR（VATSIM 认证）
  收到 #SB PIR/FSIPIR → 回 #SB PI/FSIPI（机型信息）
  收到 $CQ 查询 → 回 $CR（CAPS/机型等）
  应用层载入飞行计划 → 发 $FP
  断开 → 发 #DP
```

关键代码（`fsdclient.cpp`）：

```cpp
void CFSDClient::handleSocketConnected() {
    if (m_protocolRevision == PROTOCOL_REVISION_CLASSIC) {  // 9
        this->sendLogin();                    // 发 #AP
        this->updateConnectionStatus(CConnectionStatus::Connected);
    }
}
void CFSDClient::startPositionTimers() {
    m_positionUpdateTimer.start(c_updatePositionIntervalMsec);       // 5000
    m_scheduledConfigUpdate.start(c_processingIntervalMsec);         // 100
    m_fsdSendMessageTimer.start(c_sendFsdMsgIntervalMsec);           // 10
    // 能力开关：
    if (isInterimPositionSendingEnabledForServer())
        m_interimPositionUpdateTimer.start(1000);
    if (isVisualPositionSendingEnabledForServer())
        m_visualPositionUpdateTimer.start(200);
}
```

> `$FP` 不是 fsdclient 层自动发的：由 swift 应用层在"载入飞行计划/登录完成"
> 后调用 `sendFlightPlan()`。`#SB PI` 仅在收到对方 `PIR` 请求时回。

### 2.2 本地 XFlySim-Client

```
webapp.py connect() → model.start_fsd(...)
   │  session.connect()
   │    ├─ TCP connect
   │    └─ 发送 #AP（协议版本 9，含呼号/CID/密码/评级/SimType/真名）
   ├─ session.start_reporting()   ← 模型层立即调用（不等 #DA）
   │    └─ 启动 0.5s 周期的 reporter 线程：
   │         ├─ 每 5s：@ 位置（需已有飞行数据 has_fix）
   │         └─ 每 60s：#TM...:SERVER:ping（保活；本服务器无 $PI 处理器）
   │
   └─ XConnect 插件 5Hz 飞行数据 → model._on_xconnect_flight_data
        └─ fsd.update_flight_state(lat,lon,alt,gs,pbh,corr,xpdr,squawk)
              （首次调用后 has_fix=True，位置 @ 才开始发）

webapp connect worker：登录后 sleep(1s) → 若有已保存飞行计划 → 自动发 $FP

收到 $PI → $PO；收到 $ZC → $ZR；收到 $SB PIR/FSIPIR → 回 PI/FSIPI
收到 $ER → 显示错误并断链；断开 → #DP + 200ms 后关 socket
```

## 3. 逐项上报说明

### 3.1 `#AP` 登录（连接后第一条）

```
#AP<呼号>:<CID>:<真名>:<密码>:<协议版本>:<SimType>:<评级>:0:<客户端名>:<实参>
```

- 本地 `packet.py::add_pilot`：`PROTOCOL_REVISION=9`、SimType 16=XP12、评级=3(IFR)
- **上报前提**：TCP 连通即可发，无需服务器握手（Legacy 模式）

### 3.2 `@` 位置更新（每 5s，核心上报）

```
@<应答机模式>:<呼号>:<应答机编码>:<PilotRating>:<纬度>:<经度>:<真高ft>:<地速kt>:<PBH>:<高度修正>
```

- swift `sendPilotDataUpdate()`：由定时器驱动，使用当前模拟器状态
- **本地条件**：`has_fix` 为真（收到过至少一帧飞行数据）才发；否则静默
- 数据来源：XConnect 插件 flight_data（5Hz）→ `update_flight_state`

### 3.3 中间/视觉位置（可选，按能力）

| 报文 | 说明 | 启用条件 |
|---|---|---|
| `#SB VI` | 1s 一次中间位置 | 服务器 CAPS 含 `INTERIMPOS`/`FASTPOS` |
| `^` | 200ms 视觉位置（13 字段含速度/角速度） | `VISUPDATE` 能力 + Velocity 协议(101) + 服务器 `$SF` 开关 |

- Legacy 私有网一般全部关闭（本地不发送，仅解析）

### 3.4 机模配置 `$CQ...:ACC`（swift，本地未实现）

**上报机制**（`sendIncrementalAircraftConfig()`，`fsdclient.cpp:1025`）：

```
┌─ 100ms 定时器（m_scheduledConfigUpdate）
│
├─ 1) 组装当前 CAircraftParts：灯光/起落架/襟翼/扰流板/发动机/地面标志
│        ← 由模拟器 layer 收集（X-Plane dataref → aircraftparts）
├─ 2) 与上次已发送版本 m_sentAircraftConfig 比较 → 无变化直接返回
├─ 3) 令牌桶 CTokenBucket 限速：capacity=10，每 5000ms 补 1 token
│        （约 1 条/5s，突发至多 10 条；无 token 则本次丢弃）
├─ 4) 计算增量 JSON：对比上次已发与当前配置，只取变化字段
├─ 5) JSON 打包 {"config": {...}}，非 ASCII 字符转 \uXXXX
└─ 6) $CQ<呼号>:@94836:ACC:<json>   ← 发往伪呼号（广播给范围内客户端）
```

**配置 JSON 字段**（swift `aircraftparts.h` / `aircraftlights.h` / `aircraftengine.h`）：

| 键 | 类型 | 说明 |
|---|---|---|
| `lights` | object | `strobe_on` / `landing_on` / `taxi_on` / `beacon_on` / `nav_on` / `logo_on` / `recognition_on` / `cabin_on` / `wing_on` |
| `gear_down` | bool | 起落架放下 |
| `flaps_pct` | int | 襟翼百分比 **0..100 整数**（`round(ratio*100)`，不是 0..1 比例）|
| `spoilers_out` | bool | 扰流板 |
| `engines` | **object** | 键=发动机编号（1 基字符串），值**只含** `{"on": bool}`。如 `{"1":{"on":false},"2":{"on":false}}` |
| `on_ground` | bool | 地面标志 |
| `is_full_data` | bool | `true`=全量 / `false`=增量 |

**全量请求/应答**（`fsdclient.cpp:1421-1450`）：

- 对端请求全量：`$CQ<对端>:<我>:ACC:{"request":"full"}` → 我回**全量**（含 `"is_full_data":true`），
  对不在范围内的对端也必须应答
- 增量广播只发给 `@94836`；接收侧仅处理"范围内 + 服务器开启 receiveAircraftParts"的包，
  请求包 `{"request":"full"}` 直接忽略

**开关与前置**：

- 服务器配置 `CFsdSetup::SendAircraftParts` / `ReceiveAircraftParts`
- CAPS 能力：`ACCONFIG=1`（`Capabilities::AircraftConfig`）
- 本地实现状态：**已实现（v7.1）**——
  - 插件采集：`parts` JSON 随 flight_data 5Hz 推送（灯光用开关布尔 dataref、
    起落架/襟翼/4 发 N1/地面标志；N1 NaN 消毒避免非法 JSON）
  - 客户端 `session.py`：`notify_aircraft_parts()` 规范化（浮点 1 位、去 NaN）+
    变化检测 + 令牌桶限速 + 增量，发 `$CQ<呼号>:@94836:ACC:{"config":{变化字段}}`
  - 收到 `$CQ<对方>:<我>:ACC:{"request":"full"}` → 回全量（`is_full_data:true`）
  - 接收远程配置 → `on_aircraft_config_received(sender, config)` 回调

### 3.5 保活与应答

- swift：回复服务器的 `$PI`（`$PO` 带回时间戳）；自身不发周期 ping
- 本地：60s 发一次 `#TM<呼号>:SERVER:ping`（其服务器无 `$PI`，用文本 ping 保持 NAT/会话）
- 本地 `$PI` 应答也实现（`$PO` 原样回时间戳）

## 4. 上报前置条件汇总

| 前提 | 影响 |
|---|---|
| TCP 已连接 | 才能发送任何报文（`FsdLink.connected`） |
| 登录成功/进入 Online 状态 | 本地 `start_reporting()` 启动定时上报线程 |
| 收到第一帧飞行数据（has_fix） | `@` 位置才开始发送（避免发无效 0 坐标） |
| 能力协商（INTERIMPOS/VISUPDATE/ACCONFIG） | 决定中间/视觉位置/机模配置是否上报 |
| 已载入飞行计划 | `$FP` 上报（本地 webapp 登录后 1s 自动补交已保存计划） |

## 5. 定时器参数（swift `fsdclient.h`）

| 参数 | 值 | 用途 |
|---|---|---|
| `c_updatePositionIntervalMsec` | 5000 | 位置 `@` |
| `c_updateInterimPositionIntervalMsec` | 1000 | 中间位置 |
| `c_updateVisualPositionIntervalMsec` | 200 | 视觉位置 |
| `c_sendFsdMsgIntervalMsec` | 10 | 发送队列批量出队（≤6 条/次） |
| `c_processingIntervalMsec` | 100 | 机模配置变化检查 |
| `PendingConnectionTimeoutMs` | 7500 | 登录挂起超时 |

本地常量：`POSITION_INTERVAL=5.0`、`KEEPALIVE_INTERVAL=60.0`（`session.py`）。

## 6. 完整报文示例

> 示例呼号 CCA1501、CID 1234567、真名 ZHANG SAN、密码 pass123、
> 机型 B738（例 6 为 A321）、位置上海浦东 31.19835, 121.32845、应答机 0450。
> 实际线上报文以服务器约定为准。

### 6.1 登录 `#AP`（连接后第一条）

```
#APCCA1501:1234567:ZHANG SAN:pass123:9:16:3:0:XConnect:1.0.0
    呼号     CID     真名      密码   协议 SimType 评级 客户端名 版本
                                          9     16(XP12) 3=IFR
```

### 6.2 位置更新 `@`（每 5s）

```
@N:CCA1501:0450:3:31.19835:121.32845:1800:452:523510912:155
 └应答机模式 └呼号 └squawk └评级└纬度    └经度     └真高ft└地速 └PBH   └修正ft
   N=工作      CCA1501  0450   3   31.19835 121.32845 1800  452   位域   155
```

### 6.3 中间位置 `#SB VI`（1s，能力开启时）

```
#SBCCA1501:ALL:VI:31.19835:121.32845:1800:452:523510912
```

### 6.4 视觉位置 `^`（200ms，Velocity 协议）

```
^CCA1501:31.1983510:121.3284530:1800.00:1500.00:523510912:44.2:-1.8:0.0:0.01:0.02:0.00:1.00
 呼号    lat        lon         altTrue altAGL   pbh     vx    vy   vz   pitchRate hdgRate bankRate noseGear
```

### 6.5 飞行计划 `$FP`（登录后/载入计划时）

```
$FPCCA1501:SERVER:I:B738:450:ZSPD:0000:0000:FL350:ZBAA:1:30:2:0:ZSSS:ONLINE:N0450F350 ZSPD SID ZSSB A593 VMB ZBAA
   呼号    目标  类型 机型  速度 起飞机场 计划起飞 实际起飞 巡航高度 目的地 时:分 时:分 备降 备注  航路
                 I   B738 450  ZSPD     0000     0000     FL350    ZBAA  1:30 2:0  ZSSS
```

### 6.6 机型信息 `#SB`（请求→应答）

```
请求: #SBCCA1501:CSN3456:PIR
应答: #SBCCA1501:CSN3456:PI:GEN:EQUIPMENT=A321:AIRLINE=CCA:LIVERY=New
FSInn 请求: #SBCCA1501:CSN3456:FSIPIR
FSInn 应答: #SBCCA1501:CSN3456:FSIPI:0:CCA:A321::::::CCA A321:XConnect
```

### 6.7 Ping / Pong 保活

```
$PISERVER:CCA1501:1728200000000      ← 服务器发（时间戳 ms）
$POCCA1501:SERVER:1728200000000      ← 客户端原样回
本地保活: #TMCCA1501:SERVER:ping     （每 60s）
```

### 6.8 认证 `$ZC` → `$ZR`

```
$ZCSERVER:CCA1501:76617473696d       ← 服务器挑战
$ZRCCA1501:SERVER:eaf9c2f5b1c1f7d8c7b1e2a3f4d5b6c7  ← MD5 challenge/response
```

### 6.9 机模配置 `$CQ:ACC`

```
请求全量:  $CQCCA1501:CSN3456:ACC:{"request":"full"}

全量应答:  $CQCCA1501:CSN3456:ACC:{"config":{"lights":{"strobe_on":true,"landing_on":false,
           "taxi_on":false,"beacon_on":true,"nav_on":true,"logo_on":false,
           "recognition_on":false,"cabin_on":false,"wing_on":false},"gear_down":true,
           "flaps_pct":0,"spoilers_out":false,"engines":{"1":{"on":true},"2":{"on":true}},"on_ground":true,"is_full_data":true}}

增量广播:  $CQCCA1501:@94836:ACC:{"config":{"flaps_pct":35,"lights":{"landing_on":true},
           "is_full_data":false}}
```

### 6.10 注销 `#DP`（断开时，发后等 200ms 关 socket）

```
#DPCCA1501:1234567
```

## 7. 参考

- pilotclient：`src/core/fsd/fsdclient.cpp`（`handleSocketConnected` / `startPositionTimers` / `sendPilotDataUpdate`）
- 协议字段详解：`xfly_client/fsd/FSD_PROTOCOL.md`
- 本地实现：`xfly_client/fsd/session.py`、`xfly_client/model.py`、`webapp.py::connect`
