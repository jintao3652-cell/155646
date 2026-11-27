# MConnect 协议（TCP / JSON 行）

**与 XConnect 协议完全兼容**——客户端不需要区分对面是 X-Plane 还是 MSFS，
只多一个可选字段 `sim` 用于展示。本文件只记录与 XConnect 的差异点，
其余字段语义见 `XConnect/PROTOCOL.md`。

- 传输：TCP，端口 **51002**（占用时自动换 51003 / 51004，实际端口打印在启动日志）
  - 51001 留给 X-Plane 的 XConnect 插件；两边同时开时不冲突
- 角色：**MConnect = TCP 服务端**（监听），客户端主动连接
- 绑定：仅 `127.0.0.1`

## 与 XConnect 的字段差异

### 插件 → 客户端

`connected` 多一个 `sim` 字段：

```json
{"type":"connected","version":1,"sim":"msfs"}
```

`flight_data` 多一个 `sim` 字段，值为 `msfs2020` / `msfs2024` / `msfsunknown`：

```json
{"type":"flight_data","sim":"msfs2024","aircraft_icao":"B738", ...}
```

其余字段名、单位、语义与 XConnect **完全一致**（`latitude` / `altitude_msl`
（ft）/ `groundspeed`（kt）/ `com1_freq`（MHz×1000）/ `parts` 等）。

MSFS 侧的取值说明：

| 字段 | MSFS 来源 | 备注 |
|---|---|---|
| `aircraft_icao` | `aircraft.cfg` 的 `icao_type_designator` 反查 | 失败时退 `ATC MODEL` |
| `registration` | `ATC ID` SimVar | 空则 `registration_src` 为 `""` |
| `altitude_msl` | `PLANE ALTITUDE` | 真高（非气压高度） |
| `altitude_indicated` | `INDICATED ALTITUDE` | 座舱高度表 |
| `altitude_agl` | `PLANE ALT ABOVE GROUND` | |
| `heading` | `PLANE HEADING DEGREES MAGNETIC` | 磁航向 |
| `true_heading` | `PLANE HEADING DEGREES TRUE` | |
| `vertical_speed` | `VERTICAL SPEED`（ft/min） | |
| `com*_freq` / `com*_standby` | `COM ACTIVE/STANDBY FREQUENCY:1..3`（MHz） | ×1000 后取整；无该台为 0 |
| `com_transmit` | `COM TRANSMIT:1..3` | `[com1,com2,com3]` 各台是否在发 |
| `pilot_xmit` | `PILOT TRANSMITTER TYPE` | **0=COM1 1=COM2 2=COM3，-1=未选中/未知** |
| `copilot_xmit` | `COPILOT TRANSMITTER TYPE` | 同上 |
| `com_receive_all` | `COM RECEIVE ALL` | 全收开关 |
| `audio_panel_on` | `ELECTRICAL MASTER BATTERY` | 断电时无线电实际不可用 |
| `transponder` | `TRANSPONDER CODE:1`（BCO16） | 逐位按八进制解出 4 位码 |
| `xpdr_mode` | `TRANSPONDER STATE:1` | 0=Off 1=Stby 2=Test 3=On 4=Alt 5=Ground |
| `gear_deploy` | `GEAR TOTAL PCT EXTENDED` | >0.5 视为放下 |
| `flaps_ratio` | `FLAPS HANDLE PERCENT` | 0..1 |
| `throttle_ratio` | `GENERAL ENG THROTTLE LEVER POSITION:1` | 0..1 |
| `flight_id` | `ATC FLIGHT NUMBER` | Mode-S 航班标识（AUX 遥测，失败时为空串） |
| `com1/2/3_power` | `ELECTRICAL MASTER BATTERY`（兜底） | MSFS 无单台 COM 电源 SimVar |
| `audio_sel_com1/2/3` | `COM RECEIVE:1..3` | 收听状态（AUX 遥测，失败时恒 0） |
| `audio_vol_com1/2` | 收听状态派生 | MSFS 无音量 SimVar：收听=1.0 否则 0.0 |
| `audio_sel/vol_*_copilot` | 同机长（单音频面板） | MSFS 音频面板不区分机长/副驾 |
| `tx_com` | `pilot_xmit + 1` | 1-based 发射台，`pilot_xmit=-1` 时为 0 |
| `xpdr_sys/pwr/tcas_*` | 固定 `-1` | X-Plane 机型自定义 dataref，MSFS 无对应物 |
| `tcas_filter/alert/advisory` | 固定 `0` | MSFS 2020 无标准 TCAS 输出 SimVar |
| `parts.lights.*` | `LIGHT LANDING/TAXI/NAV/BEACON/STROBE/LOGO/WING/RECOGNITION/CABIN` | Bool（后 4 项走 AUX 遥测，失败时恒 0） |
| `parts.spoilers_out` | `SPOILERS HANDLE POSITION` | >0.1 为伸出 |
| `parts.engines[].rpm_pct` | `TURB ENG N1:1..4` | 活塞机无 N1 → 0 |
| `on_ground` | `SIM ON GROUND` | |

`csl_status` 语义改为「已成功注入的机型 / 机型目录里找不到的机型」，并多两个计数：

```json
{"type":"csl_status","sim":"msfs","active":5,"pending":1,
 "loaded":["B738","A20N"],"missing":["XXXX"]}
```

### 客户端 → 插件

`Send:OPD:FSD9` 字段与 XConnect **完全相同**（`callsign` / `latitude` /
`longitude` / `altitude` / `pitch` / `bank` / `heading` / `ground_speed` /
`aircraft` / `airline` / `registration` / `on_ground` / `altitude_agl`）。

`Send:ATC` 接收但只记录数量——MSFS 侧没有可注入的管制模型。

### aircraft_caps（机型能力，插件 → 客户端，按实际机型推送）

这是"按实际机型给选项"的核心报文。**机型切换或能力集变化时自动推送**，
客户端据此只显示这架飞机真能操作的项，不给假选项。

```json
{"type":"aircraft_caps","icao":"A20N","title":"Fenix A320",
 "standard":{"com":true,"xpdr":true},
 "inputev":{"total":10,"com_freq":2,"com_select":1,
            "xpndr_code":2,"xpndr_mode":1,"xpndr_ident":1},
 "wasm":{"state":"ready","version":"0.6.0"},
 "events":[
   {"cat":"xpndr_code","name":"XPDR_CODE_SET","hash":3735928559},
   {"cat":"xpndr_code","name":"XPDR_CODE_KNOB1","hash":3405691582},
   {"cat":"com_freq","name":"COM1_SWAP_Push","hash":123456}]}
```

| 字段 | 含义 |
|---|---|
| `icao` / `title` | 当前机型（`title` 是容器标题，含机模名） |
| `standard` | 标准 SimEvent 是否可用（兜底，几乎总是 true） |
| `inputev.total` | 枚举到的自定义 InputEvent 总数（0 = 机模没暴露） |
| `inputev.<cat>` | 各语义分类下的事件数 |
| `wasm.state` | WASM 桥状态：`off`/`registering`/`ready`/`failed` |
| `events[]` | **真实事件名 + hash**（每类最多 8 条），客户端直接拿来渲染选项 |

#### 为什么按语义分类而不是给固定列表

每个机模给按钮起的名字都不一样：

| 机模 | 应答机模式 | COM 频率交换 |
|---|---|---|
| Fenix A320 | `XPDR_STBY_Push` | `COM1_SWAP_Push` |
| PMDG 737 | `EVT_XPNDR_MODE` | `EVT_COM1_TRANSFER` |
| Asobo 默认机 | `TRANSPONDER MODE SELECT` | `COM1 STANDBY SWAP` |

写死候选表必然漏机型。MConnect 从**当前机模实际枚举到的事件**里按语义
归类（`inputev_classify`），把真实名字报给客户端，客户端不需要猜。

分类优先级（容易错的地方）：
`XPDR_IDENT` 必须先判 IDENT，否则会被 `XPDR` 泛匹配吃成 MODE；
`XPDR_MODE` 必须先判 MODE，否则会被当成 CODE。

### Send:INPUTEV（按名字触发机模自定义事件）

```json
{"type":"Send:INPUTEV","name":"XPDR_CODE_SET","value":7000}
```

`name` 来自 `aircraft_caps.events[]`，原样回传即可。名字不在当前机型事件表里
会被拒绝并记日志（不会静默失败）。

### Send:RPN（执行任意 RPN，需 WASM 桥）

```json
{"type":"Send:RPN","rpn":"5 (>L:MY_VAR)"}
{"type":"Send:RPN","rpn":"1 (>B:MY_BUTTON_Push)"}
```

仅在 `aircraft_caps.wasm.state == "ready"` 时可用（需装 MobiFlight WASM
模块）。这是最强通道，能改 LVar、触发 B 事件。

### Send:RADIO（设置无线电频率）

```json
{"type":"Send:RADIO","com":1,"active":true,"freq":122800}
{"type":"Send:RADIO","com":2,"active":false,"freq":121500}
{"type":"Send:RADIO","com":1,"swap":true}
{"type":"Send:RADIO","com":1,"flip":true}      // swap 别名（XConnect 字段互通）
{"type":"Send:RADIO","com":2,"xmit":true}
{"type":"Send:RADIO","com":1,"tx":true}        // 切发话通道（XConnect 字段互通）
{"type":"Send:RADIO","com":2,"monitor":true}   // 收听开关 toggle（XConnect 字段互通）
{"type":"Send:RADIO","recv_all":true}
```

| 字段 | 含义 |
|---|---|
| `com` | 1..3，越界拒收 |
| `active` | `true`=主频，`false`=备频；缺省按主频 |
| `freq` | **MHz×1000**（`122800` = 122.800 MHz），与 `flight_data` 的 `com*_freq` 同单位，可直接回填 |
| `swap` / `flip` | 为真时主备互换，忽略 `freq` |
| `tx` / `xmit` | 为真时把该 COM 选为**发射台**（对应读侧 `pilot_xmit`）；`false` 无意义，忽略 |
| `monitor` | 为真时 toggle 该 COM 收听（对应读侧 `audio_sel_comN`，XConnect 互通字段） |
| `recv_all` | 全收开关，不涉及 `com` |
| `power` | MSFS 无单台 COM 电源对应物，**忽略**（记日志） |

底层用 SimConnect 事件（频率是**只读** SimVar，不能直接写）：

| 目标 | 事件 | 参数 |
|---|---|---|
| COM1 主频 | `COM_RADIO_SET_HZ` | Hz |
| COM1 备频 | `COM_STBY_RADIO_SET_HZ` | Hz |
| COM2 / COM3 | `COM2_RADIO_SET_HZ` / `COM3_RADIO_SET_HZ`（及 `_STBY_`） | Hz |
| 主备互换 | `COM1/2/3_RADIO_SWAP` | — |
| 选定发射台 | `COM1/2/3_TRANSMIT_SELECT` | 1 |
| 收听 toggle | `COM1/2/3_RECEIVE_SELECT` | — |
| 全收 | `COM_RECEIVE_ALL_SET` | 1/0 |

### 呼号标签

- 每个 AI 对象创建后把 `ATC ID` 写成远端**呼号**（内置 ATC 语音会正确称呼；
  原生 Traffic Nameplates 对注入 AI 永不渲染，见下）。
- **原生铭牌不可行（模拟器硬限制）**：MSFS 20/24 只对模拟器自建 AI/多人
  飞机渲染铭牌，SimConnect 注入的 AI 一律没有；`TOGGLE_AIRCRAFT_LABELS`
  事件已被官方废弃（任何版本均无效果，无替代）。
- **自绘 overlay（现行方案）**：Community 覆盖包 `xflysim-labels`（JS，
  注入飞行标记层）每 500ms 轮询 MConnect 的 HTTP roster 快照，把注入 AI
  投影为世界锚定呼号标签（驾驶舱视角，样式与 X-Plane 自绘一致）。
- 快照端点：`GET http://127.0.0.1:51002/roster`（备用端口 51003/51004，
  同端口协议嗅探，桥接客户端不受影响），带 `Access-Control-Allow-Origin: *`。
  响应示例：

```json
{"type":"roster_snapshot","labels_on":1,"sim":"msfs","players":[
  {"callsign":"CCA1234","icao":"B738","lat":31.14,"lon":121.80,
   "alt":1500.0,"heading":90.0,"gs":250.0}],"count":1}
```

- 显隐开关：`--labels`（启动默认）或 `{"type":"Send:LABELS"}`（运行时翻转）
  → 快照里的 `labels_on`，overlay 轮询后立即显隐，无需重启。

有效波段校验：`118000 ≤ freq ≤ 137000`（MHz×1000），越界直接拒收并记日志
（客户端可能把 NAV 频率误发到 COM 通道）。

### Send:TRAFFICLIB（切换 AI 模型库偏好）

```json
{"type":"Send:TRAFFICLIB","lib":"fsltl"}
{"type":"Send:TRAFFICLIB","lib":"aig"}
{"type":"Send:TRAFFICLIB","lib":"auto"}
```

`lib` 也接受数字 `0=auto / 1=fsltl / 2=aig`。只改变匹配偏好，不重新扫描
目录；已创建的 AI 在下一帧按 title/livery 变化自动重建。仅 MConnect 支持，
X-Plane 固定使用 X-CSL，不识别该命令。

### Send:XPDR（设置应答机）

```json
{"type":"Send:XPDR","code":"1234"}
{"type":"Send:XPDR","code":1234}
{"type":"Send:XPDR","state":4}
{"type":"Send:XPDR","ident":true}
```

| 字段 | 含义 |
|---|---|
| `code` | 4 位码，**每位必须 0..7**（八进制语义）。字符串或数字都接受，`"8899"` 这类含 8/9 的拒收 |
| `state` | 应答机模式 `0=Off 1=Stby 2=Test 3=On 4=Alt 5=Ground`，越界夹到 0..5 |
| `ident` | 触发/关闭 IDENT（模拟器会在 18 秒后自动关） |

机制差异：

| 目标 | 方式 |
|---|---|
| 应答机码 | 事件 `XPNDR_SET`，参数 = **BCD 编码**（`1234` → `0x1234`） |
| 应答机模式 | 写可写 SimVar `TRANSPONDER STATE`（`SetDataOnSimObject`） |
| IDENT | 事件 `XPNDR_IDENT_SET`，参数 1/0 |

### 写入语义与注意

- **最新有效**：命令进 64 条队列，满了丢最旧的（不是丢最新的）
- **不保证成功**：机模可能自己又改回应答机码/频率（如 CDU 逻辑），
  也可能完全不实现该事件。客户端**必须以回读的 `flight_data` 为准刷新界面**，
  不要假定写入生效
- **异步**：命令由网络线程入队、主线程执行（SimConnect 非线程安全），
  存在一帧（~10ms）延迟
- 回读一致性：写完 5Hz 的 `flight_data` 里就能看到新值，可作为确认依据

## 对象生命周期

```
客户端连接
  ↓
收到 Send:OPD:FSD9 → 建 Player 槽位、位置入环形缓冲
  ↓
主循环 catalog_match(icao, airline, reg)
  ↓                    ↓
找到可创建标题        找不到
  ↓                    ↓
_EX1/基础版 AICreate   退避重试（5s→60s）
  ↓
模拟器回 ASSIGNED_OBJECT_ID → state=ACTIVE
  ↓
每帧 SetDataOnSimObject（位置+姿态+速度）
  ↓
客户端断开 / 30s 超时 → AIRemoveObject
```

同一时刻仅接受一个客户端连接（新连接替换旧连接）。

## 排查

`mconnect.log`（exe 同目录）记录全过程。关键前缀：

| 前缀 | 含义 |
|---|---|
| `sc:` | SimConnect 装载/连接/版本探测/异常码 |
| `catalog:` | 机型目录扫描进度与统计 |
| `ai:` | AI 对象创建/ID 分配/换型/写入失败 |
| `net:` | 监听端口、客户端连接、玩家批次 |
| `datadefs:` | 数据定义注册、首帧数据 |
| `loop alive:` | 每 10s 心跳（确认主循环活着） |

常见异常码（`sc: EXCEPTION`）：

| 码 | 含义 | 处理 |
|---|---|---|
| `0x02` SIZE_MISMATCH | 数据定义与结构体字节数不符 | 检查 `datadefs.c` 的 `add()` 调用顺序与 `FlightPkt` / `AiPoseRaw` 字段 |
| `0x07` NAME_UNRECOGNIZED | SimVar/事件名不被识别 | 通常是非 Windows 版或极老版本 |
| `0x16` CREATE_OBJECT_FAILED | 容器标题不存在 | 用 `--scan-only` 确认标题拼写 |