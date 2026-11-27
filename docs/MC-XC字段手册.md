# MC / XC 字段手册

> 本文以**代码为准**，覆盖 MConnect（MC，MSFS 桥）与 XConnect（XC，X-Plane 桥）
> 之间协议的全部字段、单位、取值、来源，以及配置/命令行与关键 C 结构体。
> 括号里的 `文件:行号` 是核实位置；代码变更后请同步更新本文。
>
> - 桥协议概览：`BRIDGE_PROTOCOL.md`
> - MC 差异与机模控制细节：`MConnect-PROTOCOL.md`
> - XC 差异与 dataref 细节：`XConnect-PROTOCOL.md`、`XConnect-DATAREFS.md`

## 0. 术语与约定

| 缩写 | 含义 |
|---|---|
| MC | MConnect，MSFS 侧 TCP 服务端，端口 51002（占用时 51003/51004） |
| XC | XConnect，X-Plane 侧 TCP 服务端，端口 51001（固定） |
| 桥协议 | 客户端与插件之间的换行分隔 JSON（TCP，UTF-8） |
| `flight_data` | 插件 → 客户端的本机飞行数据，5 Hz |
| `parts` | 机模部件（灯/起落架/襟翼/扰流板/发动机） |
| ACC | FSD 的 Aircraft Configuration 报文，客户端由此得到远端 `parts` |
| InputEvent | MSFS 机模自定义事件（非标准 SimEvent） |
| WASM | MobiFlight WASM 模块提供的 RPN 执行通道（仅 MC） |

**单位速记**：距离高度 ft（英尺）、经纬度度、速度 kt（节）、垂直速度 ft/min、
频率 MHz×1000、角度度、`parts.flaps_pct` 为 0..100 整数、其余比例多为 0..1。
例外在 §2.1.4 单列，那里是跨桥最容易踩坑的地方。

---

## 1. 握手与连接

### 1.1 `connected`（插件 → 客户端，建连瞬间主动发）

| 字段 | 类型 | MC | XC | 含义 |
|---|---|---|---|---|
| `type` | str | `"connected"` | 同 | 固定 |
| `protocol` | int | `1` | `1` | 桥协议主版本 |
| `plugin` | str | `"MConnect"` | `"XConnect"` | 插件名 |
| `version` | str | `"1.0.0"` | `"1.0.0"` | 插件 SemVer（XC 用 `XCONNECT_VERSION` 宏） |
| `sim` | str | `"msfs"` | `"xplane"` | 模拟器族 |
| `capabilities` | str[] | 见下表 | 见下表 | 能力位清单 |

位置：MC `MConnect/src/mconnect.c:455-459`；XC `XConnect/src/xconnect.c:583-586`。
客户端消费：`xfly_client/xconnect/client.py:403-439`。

### 1.2 `hello`（客户端 → 插件）

```json
{"type":"hello","protocol":1,"client":"XFlySim-Client","version":"1.0.0"}
```

| 字段 | 含义 |
|---|---|
| `protocol` | 客户端期望的桥协议版本 |
| `client` | 客户端名 |
| `version` | 客户端 SemVer |

**插件只匹配 `type`，不读取另外三个字段**（MC `mconnect.c:338-350`；XC `xconnect.c:360-372`）。
位置：`xfly_client/xconnect/client.py:362-365`。

### 1.3 `hello_ack`（插件 → 客户端）

字段与 `connected` 同名同值，**唯独没有 `sim`**。位置：MC `mconnect.c:343-347`；
XC `xconnect.c:365-369`。

### 1.4 capabilities 对照（两桥互不 superset）

| capability | MC | XC | 含义 |
|---|:---:|:---:|---|
| `csl` | ✅ | ✅ | 能渲染在线飞机 |
| `parts` | ✅ | ✅ | 能上报/接收机模部件 |
| `controls` | ✅ | ✅ | 支持 RADIO/XPDR 写入 |
| `diagnostics` | ✅ | ✅ | 支持诊断请求 |
| `wasm` | ✅ | ❌ | 支持 RPN / WASM 通道 |
| `position_vi` | ❌ | ✅ | FSD 侧可发送 VI 位置包 |
| `position_visual` | ❌ | ✅ | FSD 侧可发送 `^` 视觉位置包 |
| `acc` | ❌ | ✅ | 支持 ACC 机模配置 |

定义：MC `mconnect.c:458-459`；XC `XConnect/src/xconnect.c:20-22`。
注意 `wasm` 与 `acc`/`position_*` 互斥，**不要用一套 capabilities 描述两桥**。

### 1.5 兼容规则与行长度

- 旧插件只发 `{"type":"connected","version":1}`：客户端按 legacy 处理（无 capabilities）。
- 未知字段/未知 `type` 一律忽略；协议主版本不一致只告警，不主动断开。
- 插件侧单行上限 256 KB（MC `mconnect.c:462`、XC `xconnect.c:590`）；
  客户端 `RX_LINE_LIMIT=64KB` 常量**未被使用**，读循环按 `\n` 无限累积
  （`client.py:34,375-391`）——这是实现现状，不要据旧文档以为两边都有硬限制。

---

## 2. 插件 → 客户端消息

### 2.1 `flight_data`（5 Hz）

构造位置：MC `MConnect/src/datadefs.c:1010-1208`（格式串 `1107-1164`）；
XC `XConnect/src/datarefs.c:1166-1289`（格式串 `1217-1253`）。

#### 2.1.1 两桥都发

| 字段 | 单位/取值 | MC 来源 | XC 来源 |
|---|---|---|---|
| `type` | `"flight_data"` | 固定 | 固定 |
| `aircraft_icao` | ICAO 码，未知 `ZZZZ` | `catalog` 反查 `icao_type` | `sim/aircraft/view/acf_ICAO` |
| `aircraft_name` | 机型 UI 名 | `TITLE` | `acf_ui_name` |
| `flight_id` | Mode-S Flight ID ≤7 字符 | `ATC FLIGHT NUMBER`（AUX） | `sim/cockpit2/radios/actuators/flight_id` |
| `registration` | 尾号 | `ATC ID` | 涂装目录名优先，退 `acf_tailnum` |
| `registration_src` | MC `"atc_id"`/`""`；XC `"livery"`/`"tailnum"`/`""` | 同上 | 同上 |
| `engine_count` | 台数，`0`=未知 | 本地结构 | 本地结构 |
| `latitude` / `longitude` | 度（%.6f） | `PLANE LATITUDE/LONGITUDE` | `sim/flightmodel/position/latitude/longitude` |
| `altitude_msl` | ft | `PLANE ALTITUDE` | `position/elevation[m]×3.28084` |
| `altitude_agl` | ft | `PLANE ALT ABOVE GROUND` | `position/y_agl[m]×3.28084` |
| `elevation` | **见 §2.1.4** | ft = `MSL−AGL` | 米（`position/elevation` 原值） |
| `altitude_indicated` | ft | `INDICATED ALTITUDE` | `altitude_ft_pilot` |
| `pitch` / `bank` / `heading` / `true_heading` | 度；`heading` 为磁航向 | 对应 SimVar | 对应 dataref |
| `groundspeed` | kt | `VELOCITY GROUNDSPEED` | m/s×1.943844 |
| `indicated_airspeed` | **见 §2.1.4** | kt | m/s（原值） |
| `vertical_speed` | ft/min | `VERTICAL SPEED`[ft/s]×60 | `vh_ind_fpm` |
| `com1/2/3_freq`、`com1/2/3_standby` | MHz×1000；无该台 `0` | BCD16 解码 | `_hz_833` dataref 归一化 |
| `avionics_on` | 航电总电门 | `ELECTRICAL MASTER BATTERY` | `sim/cockpit/electrical/avionics_on` |
| `com1/2/3_power` | 单台 COM 电源 | 与 `audio_panel_on` 同源兜底 | `comN_power`，缺失用 `avionics_on` 兜底 |
| `audio_sel_com1/2/3` | 收听选中 0/1 | `COM RECEIVE:n`（AUX） | `audio_selection_comN` |
| `has_audio_sel_com3` | 是否有 COM3 收听 | **恒 `1`** | 动态探测 dataref |
| `audio_vol_com1/2` | 0..1 | 收听=1.0 否则 0.0（无音量 SimVar） | dataref 原值 |
| `audio_sel_com1/2_copilot`、`audio_vol_com1/2_copilot` | 副驾音频面板 | 与机长同源 | 独立 dataref |
| `tx_com` | **1-based** 发射台 1/2/3，`0`=未选 | `pilot_xmit+1` | `audio_com_selection` 映射（6/0→1，7/1→2，2→3） |
| `pilot_xmit` | **0-based** 0/1/2，`-1`=未选 | `PILOT TRANSMITTER TYPE` | `tx_com>0 ? tx_com-1 : -1` |
| `transponder` | 4 位码（十进制写法，逐位 0..7） | `TRANSPONDER CODE:1` BCO16 解码 | `transponder_code` |
| `xpdr_mode` | **见 §2.1.4** | `TRANSPONDER STATE:1` 序列 | XP12 `transponder_mode` 0..7 |
| `squark` | `"S"`=待机 / `"N"`=发射 | 由 `xpdr_state` 与码派生 | `mode<=1 ? "S" : "N"` |
| `tcas_filter` / `tcas_alert` / `tcas_advisory` | 过滤 0=ALL..4=URSTR；告警 0/1；advisory 0/1/2 | **恒 `0`** | 真实 dataref |
| `xpdr_sys` / `xpdr_pwr` / `xpdr_tcas_mode` / `xpdr_tcas_alt` | 机型自定义枚举，`-1`=无此字段 | **恒 `-1`** | `AirbusFBW/*` 等，缺失 `-1` |
| `gear_deploy` | **见 §2.1.4** | 二值：`gear_ratio>0.5` | 连续 0..1 |
| `flaps_ratio` | 0..1 实际展开比例 | `FLAPS HANDLE PERCENT` | `flap_system_deploy_ratio` |
| `throttle_ratio` | 0..1（1 号发动机） | `GENERAL ENG THROTTLE LEVER POSITION:1` | `throttle` dataref |
| `parts` | 见 §2.1.3 | — | — |

#### 2.1.2 仅 MC 发

| 字段 | 含义/取值 | 位置 |
|---|---|---|
| `sim` | `"msfs2020"` / `"msfs2024"` / `"msfsunknown"`（`"msfs%s"` + `sc_sim_name()`） | `datadefs.c:1109,1165` |
| `com_transmit` | `[com1,com2,com3]` 各台是否在发 0/1 | `datadefs.c:1129,1187` |
| `copilot_xmit` | `COPILOT TRANSMITTER TYPE`，0/1/2，`-1`=未选 | `datadefs.c:1130,1189` |
| `com_receive_all` | 全收开关 | `datadefs.c:1149,1197` |
| `audio_panel_on` | 实为 `ELECTRICAL MASTER BATTERY`（音频面板供电） | `datadefs.c:1149,1190` |

XC 不发这些字段；客户端 `FlightData` 里声明了，XC 连接时保持默认值（`client.py:113-115`）。
`sim` 客户端**不写入 `FlightData`**，只在握手时读（`client.py:408,453`）。

#### 2.1.3 `flight_data.parts`（插件上报方向）

位置：MC `datadefs.c:1027-1048,1159-1164`；XC `datarefs.c:1198-1214,1247-1253`。

| 字段 | 取值 | 说明 |
|---|---|---|
| `lights.strobe_on` / `landing_on` / `taxi_on` / `beacon_on` / `nav_on` / `logo_on` / `wing_on` / `recognition_on` / `cabin_on` | 0/1 | 9 种灯，全部都发 |
| `gear_down` | 0/1 | MC 由 `gear_ratio>0.5` 派生；XC 直接读 |
| `flaps_pct` | **0..100 整数** | `round(ratio*100)` |
| `spoilers_out` | 0/1 | MC 由 `ratio>0.1` 派生 |
| `engines` | **JSON 对象** `{"1":{"on":true},"2":{"on":false}}` | 键 1-based；值**只有 `on`** |
| `on_ground` | 0/1 | 地面标志 |

⚠️ **`engines` 没有 `rpm_pct`，也不是数组**。曾发成 `[{"on":..,"rpm_pct":..}]`
导致服务器类型不匹配、`aircraft.engines` 恒为 null；多余键也会让该台引擎解析成 `{}`。
两桥都已修正（`datadefs.c:1024-1048`、`datarefs.c:1198-1214`）。
`parts.throttle_ratio` **不存在**，油门只在顶层 `throttle_ratio`。

#### 2.1.4 单位与语义陷阱（务必按桥区分）

| 字段 | MC | XC | 后果 |
|---|---|---|---|
| `elevation` | ft | **米** | 直接混用会让地面高度差 3.28 倍 |
| `indicated_airspeed` | kt | **m/s** | 空速显示/上报错 1.94 倍 |
| `gear_deploy` | 二值 0/1 | 连续 0..1 | UI 若按连续量画 MC 会只有两端 |
| `xpdr_mode` | 默认机路径 0=Off 1=Stby **2=Test 3=On 4=Alt** 5=Ground；PMDG/WASM 路径 2=On 3=Alt **4=Test** | XP12 原生 0=Off 1=Stby 2=On 3=Alt **4=Test** 5=Ground 6=TA 7=TA/RA | 同一字段两套顺序，客户端必须先确认 `sim` 再解释 |
| `has_audio_sel_com3` | 恒 1 | 动态 | MC 的 1 不代表真有该 dataref |

MC 内部两套应答机顺序的位置：`datadefs.c:706`（默认 SimVar）、`datadefs.c:952-979`（PMDG/WASM）。

### 2.2 `csl_status`

| 字段 | MC | XC | 含义 |
|---|---|---|---|
| `type` | `"csl_status"` | 同 | 固定 |
| `sim` | `"msfs"` | **无** | 插件族 |
| `active` / `pending` | 有 | **无** | 已激活 AI 数 / 待创建数 |
| `loaded[]` | 已成功注入的 ICAO | 有实例的 ICAO | 已加载机型列表 |
| `missing[]` | `state==AI_STATE_FAILED` 的 ICAO | `tried && inst_count==0` 的 ICAO | 找不到模型的机型 |

位置：MC `mconnect.c:544-554`；XC `xconnect.c:661-670`。客户端只把整个对象透传给
`on_csl_status`（`client.py:441-443`），不解析子字段。

### 2.3 `aircraft_caps`（**仅 MC**，机型能力）

构造：`MConnect/src/aircraftcaps.c:86-151`；分类：`inputev.c:184-239`。
机型切换或能力集变化时自动推送，客户端据此只显示真能操作的项。

```json
{"type":"aircraft_caps","icao":"A20N","title":"Fenix A320",
 "profile":"FBW_A320","profile_caps":7,
 "standard":{"com":true,"xpdr":true},
 "inputev":{"total":10,"com_freq":2,"com_select":1,
            "xpndr_code":2,"xpndr_mode":1,"xpndr_ident":1},
 "wasm":{"state":"ready","version":"0.6.0"},
 "events":[{"cat":"xpndr_ident","name":"XPDR_IDENT_Push","hash":123}]}
```

| 字段 | 含义 |
|---|---|
| `icao` / `title` | 当前机型 ICAO 与容器标题（含机模名） |
| `profile` | 命中的机型档案 ID，无档案 `"none"`（`aircraftcaps.c:128,138`） |
| `profile_caps` | 档案能力位（uint）：`0x01` xpndr_code / `0x02` xpndr_mode / `0x04` com_set / `0x08` com_swap / `0x10` com_xmit（`aircraftdb.h:53-57`） |
| `standard` | 标准 SimEvent 兜底 `{com:true,xpdr:true}` |
| `inputev.total` | 枚举到的 InputEvent 总数，`0`=机模没暴露 |
| `inputev.com_freq` / `com_select` / `xpndr_code` / `xpndr_mode` / `xpndr_ident` | 各语义分类的事件数 |
| `wasm.state` | `"off"` / `"registering"` / `"ready"` / `"failed"` |
| `wasm.version` | WASM 桥版本串 |
| `events[]` | `{cat,name,hash}`：真实事件名 + 32 位 hash，每类最多 8 条 |

客户端消费：`client.py:222-285`（`control_options()` 读 `events[].cat/name`，
`supports()` 依次读 `profile_caps` → `inputev.<cat>` → `standard`，
`profile_id()` 读 `profile`，`control_backend()` 读 `wasm.state`）。

`events[].cat` 全集（`inputev.c:228-239`）：

| `cat` | 判定关键字（忽略大小写） |
|---|---|
| `xpndr_ident` | `ident` / `id_button` / `idbtn`（**优先级最高**） |
| `xpndr_mode` | `mode` / `_alt` / `standby` / `_stby` / `_on` / `_off` / `test` / `tara` / `_ta`，xpndr 兜底也归此类 |
| `xpndr_code` | `code` / `digit` / `knob` / `set` / `1`..`4` |
| `com_freq` | `com1/2/3` / `com_` / `radio` / `vhf` / `freq`，且不含 select 类关键字 |
| `com_select` | 上述基础上含 `transmit`/`xmit`/`select`/`receive`/`recv`/`audio`/`listening` |
| `autopilot` | `ap_` / `autopilot` / `fcc_` / `athr`（预留，**不在上报分类表里**） |
| `none` | 未分类 |

分类顺序很重要：`XPDR_IDENT` 必须先判，否则被 `XPDR` 泛匹配吃成 MODE；
`XPDR_MODE` 必须先判，否则被当成 CODE。

### 2.4 `roster_snapshot`（**HTTP，不是 TCP 桥**，仅 MC）

`GET http://127.0.0.1:51002/roster`（备用端口 51003/51004，同端口协议嗅探），
带 `Access-Control-Allow-Origin: *`。构造：`mconnect.c:371-420`。

| 字段 | 含义 |
|---|---|
| `type` | `"roster_snapshot"` |
| `labels_on` | 呼号 overlay 显隐（0/1），由 `--labels` / `Send:LABELS` 翻转 |
| `sim` | `"msfs"` |
| `players[]` | `callsign` / `icao` / `lat` / `lon` / `alt` / `heading` / `gs` |
| `count` | 玩家数 |

⚠️ `players[].alt` 是**米**（`PositionFix.alt`，`players.h:46`），不是桥协议 `flight_data` 的 ft。
JS overlay（`XFlyLabels`）每 500 ms 轮询此端点绘制贴机身呼号。

---

## 3. 客户端 → 插件命令

### 3.1 `Send:OPD:FSD9`（在线玩家列表）

| 字段 | 单位/取值 | 说明 |
|---|---|---|
| `type` | `"Send:OPD:FSD9"` | 固定 |
| `mycid` | 客户端 CID | 客户端发送、插件**从不读取**（`mconnect.c:228`、XC `xconnect.c:148` 起均无 `json_get("mycid")`） |
| `data[]` | 玩家数组 | 见下 |

`data[]` 元素（`PlayerEntry`，`client.py:145-162`）：

| 字段 | 单位/取值 | 用途 |
|---|---|---|
| `callsign` | 呼号 | 主键 |
| `aircraft` | ICAO，空→`ZZZZ` | 机型匹配 |
| `aircraft_family` | 机型族 | 别名回退 |
| `airline` | 航司/涂装码 | 涂装匹配 |
| `registration` | 注册号 | 最高优先级匹配 |
| `latitude` / `longitude` | 度 | 位置 |
| `altitude` | ft MSL | 位置 |
| `altitude_agl` | ft，`<0`=未知 | 位置 |
| `pitch` / `bank` / `heading` | 度 | 姿态 |
| `ground_speed` | kt | 速度 |
| `on_ground` | bool/0-1 | 地面标志 |
| `parts` | ACC 合并后的部件 | 见 §4.2 |

### 3.2 `Send:ATC`

`{"type":"Send:ATC","data":[{"callsign","frequency","type","latitude","longitude","rating"}]}`。
MC 只记录数组长度（`mconnect.c:332-337`），XC 只存数量到 `g_atc_count`
（`xconnect.c:344-350`），**都不读子字段**——MSFS/X-CSL 没有可注入的管制模型。

### 3.3 `Send:RADIO`

| 字段 | 含义 | MC | XC |
|---|---|---|---|
| `com` | 1..3，越界拒收 | `control.c:837` | `xconnect.c:424` |
| `active` | `true`=主频、`false`=备频，缺省主频 | ✅ | ✅ |
| `freq` | MHz×1000；MC 合法 `118000..137000`，XC `118000..136975` | `control.c:894` | `xconnect.c:480` |
| `swap` / `flip` | 主备互换，为真时忽略 `freq` | ✅ | ✅ |
| `tx` / `xmit` | 选该台为发射台；仅非零触发 | ✅ | ✅ |
| `monitor` | 收听 toggle；仅非零触发 | ✅ | ✅ |
| `recv_all` | 全收开关，不需要 `com` | ✅ | ✅ |
| `power` | 单台电源 | **忽略并记日志** | `1/0` 或 bool → `power_comN_on/off` |

客户端只有 `send_radio()` / `send_radio_swap()` / `send_radio_xmit()` / `send_recv_all()`
（`client.py:500-520`），`monitor` / `power` 目前无专用发送函数。
频率是**只读 SimVar**，底层必须走 SimConnect 事件（`COM_RADIO_SET_HZ` 等，参数 Hz）。

### 3.4 `Send:XPDR`

| 字段 | 含义 |
|---|---|
| `code` | 4 位码，每位 0..7（八进制语义）；字符串或数字均可，`"8899"` 拒收 |
| `state` | **写入侧规范枚举**：`0=Off 1=Stby 2=On 3=Alt 4=Test 5=Ground`；MC 接受 6/7 并映射到 3，`>7`→5，写前再 remap 到 MSFS 顺序；XC `0..7` 直通，`>7` 夹到 7 |
| `ident` | 触发 IDENT；MC `false` 也入队（on=0），XC `false` 忽略并记日志 |

位置：MC `control.c:924-977`（remap `67-79,687`）；XC `xconnect.c:508-554`。
MSFS 侧模式写可写 SimVar `TRANSPONDER STATE`，码走 `XPNDR_SET`（参数 BCD，如 `0x1234`），
IDENT 走 `XPNDR_IDENT_SET`。

### 3.5 `Send:TRAFFICLIB`（仅 MC）

`{"type":"Send:TRAFFICLIB","lib":"fsltl"|"aig"|"auto"}`，也接受数字
`0=auto / 1=fsltl / 2=aig`（其他→auto）。只改匹配偏好，不重新扫描目录；
已创建的 AI 下一帧按 title/livery 变化自动重建（`mconnect.c:315-329`）。
XC 无此 handler，固定 X-CSL。

### 3.6 `Send:INPUTEV`（仅 MC）

`{"type":"Send:INPUTEV","name":"XPDR_CODE_SET","value":7000}`。

| 字段 | 含义 |
|---|---|
| `name` | 来自 `aircraft_caps.events[].name`，大小写不敏感查找；不存在则拒收并记日志 |
| `value` | double，实际 `(int)(v+0.5)` 后作为事件参数（`control.c:1010`） |

### 3.7 `Send:RPN`（仅 MC）

`{"type":"Send:RPN","rpn":"5 (>L:MY_VAR)"}`。
`rpn` 字符串上限 **160 字节**（`control.c:1018-1026`），仅在
`aircraft_caps.wasm.state == "ready"` 时可用（需装 MobiFlight WASM 模块）。

### 3.8 `Send:LABELS`（仅 MC）

无字段。翻转 JS overlay 的 `labels_on`（`control.c:1028-1038`、`mconnect.c:382-384`）。
注意：客户端函数注释仍写"翻转 MSFS 内置标签"，那是旧语义，实际已改为 overlay。

### 3.9 `Send:SCAN`（仅 XC）

无字段。由 `tools/dref_find.py` 发送，置 `g_scan_request=1`（`xconnect.c:355-359`），
主线程枚举 dataref 写入 `xconnect.log` 的 `dref:` 行。

### 3.10 写入语义（两桥共有）

- **最新有效**：命令进 64 条队列，满了丢最旧而非最新。
- **异步**：网络线程入队、主线程执行（SimConnect/XPLM 非线程安全），约一帧延迟。
- **不保证成功**：机模可能自己改回值或根本不实现事件；**客户端必须以回读的
  `flight_data` 为准刷新界面**，写完约 5 Hz 周期内可看到新值。

---

## 4. `parts` 双向差异（最容易混淆）

### 4.1 插件上报：`flight_data.parts`（§2.1.3）

9 种灯全发，`engines` 只含 `on`，`flaps_pct` 是 0..100 整数，没有 `throttle_ratio`。

### 4.2 客户端下发：`data[].parts`（来自 FSD ACC 合并）

构造位置 `xfly_client/model.py:600-651`。字段：

| 字段 | 含义 |
|---|---|
| `lights` | canonical 9 键（同 §2.1.3） |
| `gear_down` / `flaps_pct` / `spoilers_out` / `throttle_ratio` / `on_ground` | 部件状态 |
| `engines` | 1-based 对象，只含 `on` |
| `engine_count` | 台数 |
| `engine_type` | `J`/`T`/`P`/`R`/`?` |
| `gear_type` | `fixed` / `retractable` / `unknown` |
| `parts_source` | `fsd_parts` / `guessed` / `not_set` |

`is_full_data` 在合并时被剥离（`model.py:92-99`）。
**插件只消费前 6 种灯**（`beacon/landing/taxi/nav/strobe/logo`），
`wing_on` / `recognition_on` / `cabin_on` 在写入方向被丢弃
（MC `mconnect.c:150-198`；XC `xconnect.c:218-307`）。

客户端解析契约（`xfly_client/aircraft_profile.py:30-49,183-397`）：
`LIGHT_KEYS` 9 个，服务器只接受前 6 个 `SERVER_LIGHT_KEYS`；有别名表；
`throttle_ratio` 量化到 5%；`get_adjusted_lights()` 用任一引擎在转推导
`recognition_on` / `cabin_on`。

### 4.3 `lights` 位掩码（C 内部）

两桥一致：bit0=beacon、bit1=landing、bit2=taxi、bit3=nav、bit4=strobe、bit5=logo
（MC `players.h:105`；XC `players.h:78-79`）。

---

## 5. 配置、命令行与环境变量

### 5.1 MC 命令行（`MConnect/src/mconnect.c:806-907`）

| 参数 | 含义 | 备注 |
|---|---|---|
| `--port <n>` | **解析了但完全忽略** | 端口固定 51002→51003→51004；`mconnect.c:907 (void)port_arg` |
| `--scan <dir>` | 只扫指定目录作为机型目录 | |
| `--simconnect <p>` | SimConnect.dll 路径或所在目录 | 设 `MCONNECT_SIMCONNECT` 环境变量 |
| `--verbose` / `-v` | 详细日志 | |
| `--scan-only` | 只扫机型目录，不连模拟器不监听 | 验证 FSLTL/AIG 读取用 |
| `--dump-input` | 连上后把 InputEvent 表写日志 | |
| `--dump-wasm` | 检查 MobiFlight WASM 桥 | |
| `--dump-profiles` | 打印机型档案表，不连模拟器 | |
| `--parts-probe` | 真机探针：注入临时 AI 测部件可写性 | |
| `--traffic-lib <auto\|fsltl\|aig>` | AI 模型库偏好 | 对应 `Send:TRAFFICLIB` |
| `--labels` | 启动把 overlay 开关设为 1（toggle 语义） | 不是 MSFS 内置铭牌 |
| `--help` / `-h` | 帮助 | GUI 构建下提示用 `MConnectConsole.exe` |

### 5.2 XC 配置（无命令行参数）

XC 是 XPLM 插件，入口 `XPluginStart()`（`XConnect/src/xconnect.c:775-864`），
**没有 `main()`、没有 `--*` 参数**：

| 配置项 | 值/位置 |
|---|---|
| 监听端口 | 固定 `51001`（`XConnect/src/xconnect.h:39`） |
| 玩家上限 | `PLAYER_MAX=256` |
| 机型适配表 | `Resources/plugins/XConnect/radio.json`（`xconnect.c:810-819`） |
| CSL 根 | `Resources/plugins/XConnect/CSL`（`xconnect.c:822-824`） |
| 日志 | `Resources/plugins/XConnect/xconnect.log` |
| 呼号标签 | 注册即绘制，无开关（`labels.c:186-190`） |

### 5.3 客户端配置字段（`xfly_client/config.py:28-74`）

| 字段 | 默认 | 含义 |
|---|---|---|
| `callsign` / `cid` / `password` / `real_name` | 空 | FSD 登录信息（诊断导出自动脱敏） |
| `aircraft_icao` | `"A320"` | 上报机型 |
| `airline_icao` / `registration` | 空 | 航司/尾号，尾号手填优先 |
| `registration_cache` | `None` | ICAO → 尾号缓存（同机型复用） |
| `remember_password` | `True` | 是否记住密码 |
| `debug_mode` | `False` | 显示原始收发报文 |
| `language` | `"en"` | 界面语言（zh-CN/zh-TW/en） |
| `flight_plan` | `None` | 飞行计划 |
| `simbrief_user` | 空 | SimBrief 用户名/ID |
| `server_host` / `server_port` | `ol.xflysim.com` / `6809` | FSD 服务器 |
| `xconnect_host` / `xconnect_port` | `127.0.0.1` / `51001` | XC 桥 |
| `mconnect_port` | `51002` | MC 桥，须与 XC 错开；占用退 51003/51004 |
| `auto_start_mconnect` | `True` | 启动客户端时拉起 MConnect.exe |
| `mconnect_stop_on_exit` | `False` | 退出时是否终止 MC（默认保留 SimConnect 会话） |
| `msfs_toggle_labels` | `False` | 启动时翻转 MC 呼号 overlay（toggle 语义） |
| `msfs_traffic_lib` | `"auto"` | MSFS AI 库偏好；仅影响 MC，XC 固定 CSL |

配置存 `%APPDATA%\XFlySim-Client\config.bin`（机器绑定混淆），详见 `config.py` 顶部说明。

### 5.4 环境变量

| 变量 | 用途 |
|---|---|
| `MCONNECT_SIMCONNECT` | 指定 SimConnect.dll（由 `--simconnect` 设置） |
| `XFLYSIM_LOG_DIR` | 覆盖客户端日志目录（默认程序目录 `logs/`） |
| `XFLYSIM_CSL_ROOT` / `XFLYSIM_FSLTL_ROOT` / `XFLYSIM_AIG_ROOT` | 覆盖资源根目录 |
| `MSFS_COMMUNITY` / `XPLANE_ROOT` | 资源路径自动发现用 |

---

## 6. 关键 C 结构体字段

### 6.1 MC `FlightRaw`（本机快照，`MConnect/src/datadefs.h:67-123`）

- 位置姿态：`lat_deg, lon_deg, alt_msl_ft, alt_agl_ft, alt_indicated_ft, pitch_deg,
  bank_deg, heading_mag_deg, heading_true_deg, gs_kt, ias_kt, vs_fpm`
- 无线电：`com1/2/3_mhz1000, com1/2/3_stby_mhz1000, com_transmit[3], com_status[3],
  pilot_xmit_type, copilot_xmit_type, com_receive_all, audio_panel_on, aux_valid,
  com_recv[3], flight_id[32]`
- 机模：`title[256], atc_id[32], atc_airline[32], atc_model[32], atc_type[16],
  icao_type[16], num_engines, n1_pct[4], eng_on[4]`
- 部件：`light_logo/wing/recog/cabin, light_landing/taxi/nav/beacon/strobe,
  transponder, xpdr_state, on_ground, gear_ratio, flaps_ratio, throttle_ratio,
  spoilers_ratio, parking_brake`

### 6.2 MC AI 写入结构（`datadefs.h:37-64`）

| 结构 | 字段 | 约束 |
|---|---|---|
| `AiPoseRaw` | `latitude, longitude, altitude, pitch, bank, heading, on_ground, velocity_x/y/z, vel_body_x/y/z`（全 double） | 顺序必须与 `ai_define_position()` 一致 |
| `AiPartsRaw` | `gear_handle, flaps_left, flaps_right, spoilers, throttle[4]` | 与 AI 数据定义顺序一致 |

### 6.3 `Player`（MC `players.h:62-117` / XC `players.h:44-98`）

共有：`callsign[24], icao[8], airline[8], reg[16], family[64], model_icao[8],
model_airline[8], fixes[6], fix_count, fix_head, avg_interval, cur_lat/lon/alt,
cur_heading/pitch/roll, have_first_fix, last_seen, last_snap_log, has_parts,
has_gear, gear_down, flaps_pct, spoilers_out, throttle, has_throttle, lights,
engines_on, engine_count, engine_type[4], gear_type[12], parts_source[16], active`

MC 独有：`model_title[112], model_livery[64], last_write, last_parts_log, state,
object_id, create_request_id, create_attempts, retry_at, write_fail_logged,
lights_sent, parts_write_failed, catalog_wait_logged, atcid_written`

XC 独有：`model_reg[16], last_probe_t, probe_msl, probe_ok, tire_angle, last_anim_t,
model`（CslNode）

### 6.4 `PositionFix`（两桥同构）

`lat, lon, alt[m], heading, pitch, roll, gs[kt], agl[m], on_ground, ts`
（MC `players.h:44-52`；XC `players.h:34-42`）。
注意这里的 `alt` 是**米**，与桥协议 `flight_data.altitude_msl`（ft）不同。

### 6.5 命令结构

| 结构 | 字段 | 位置 |
|---|---|---|
| MC `Command` | `kind, com_index, active, hz, code, state, on, hash(DWORD)` | `MConnect/src/control.h:53-77` |
| XC `XCommand` | `kind, com_index, active, hz, code, mode`（0..7） | `XConnect/src/datarefs.h:122-151` |
| `InputEvent` | `name[64], hash:DWORD, type:DWORD` | `MConnect/src/inputev.h:35-59` |
| `Json` | `next, key, type(NULL/BOOL/NUM/STR/ARR/OBJ), num, bval, sval, child` | `MConnect/src/json.h:8-25`（XC 同构） |

XC `DrIndex` 是 dataref 索引枚举（`DR_LAT..DR_COUNT`，含 `DR_AVIONICS_ON`、
`COM*_POWER`、`AUD_SEL*`、`AUD_VOL*`、`AUD_COM_SEL`、`DR_AC_ICAO`、`DR_FLIGHT_ID`、
`DR_LIVERY_PATH`、`DR_NUM_ENGINES`、`DR_ALT_IND`、`DR_LT_*_BRI` 等），
见 `XConnect/src/datarefs.h:8-78`，dataref 清单另见 `XConnect-DATAREFS.md`。

---

## 7. 差异速查与常见坑

| # | 坑 | 处理 |
|---|---|---|
| 1 | `xpdr_mode` 在 MC 内部有两套顺序，XC 又是第三套 | 先读 `connected.sim`，再按 §2.1.4 解释；写入统一用 §3.4 的规范枚举 |
| 2 | `parts.engines` 不是数组、无 `rpm_pct` | 按 1-based 对象解析，只读 `on` |
| 3 | `aircraft_caps.profile` / `profile_caps` 必发 | 客户端 `supports()` 依赖它们，不要当作可选字段 |
| 4 | MC `--port` 是假参数 | 端口固定 51002/3/4 退避 |
| 5 | 两桥 capabilities 不同 | `wasm` 仅 MC；`acc`/`position_*` 仅 XC |
| 6 | `hello_ack` 无 `sim` | 需要模拟器族时缓存 `connected` 的值 |
| 7 | `elevation` / `indicated_airspeed` 单位跨桥不同 | 见 §2.1.4 |
| 8 | `roster_snapshot.players[].alt` 是米 | overlay 与桥数据混用时注意换算 |
| 9 | 写入方向只消费 6 种灯 | 不要指望下发 `wing/recognition/cabin` 生效 |
| 10 | 桥写入不保证成功 | 以 5 Hz 回读 `flight_data` 为准 |

---

## 8. 代码位置索引

| 主题 | 权威位置 |
|---|---|
| MC `flight_data` 构造 | `MConnect/src/datadefs.c:1010-1208` |
| XC `flight_data` 构造 | `XConnect/src/datarefs.c:1166-1289` |
| MC 握手/分发/roster | `MConnect/src/mconnect.c:299-420,455-459,544-554` |
| XC 握手/分发 | `XConnect/src/xconnect.c:130-372,583-586,661-670` |
| 客户端解析/发送 | `xfly_client/xconnect/client.py:58-132,393-573` |
| MC 命令解析 | `MConnect/src/control.c:789-1069` |
| XC 命令解析 | `XConnect/src/xconnect.c:388-557` |
| caps 构造/分类 | `MConnect/src/aircraftcaps.c:86-151`、`inputev.c:184-239` |
| parts 消费 | `MConnect/src/mconnect.c:150-198`、`XConnect/src/xconnect.c:214-307`、`xfly_client/aircraft_profile.py:30-397` |
| 客户端配置 | `xfly_client/config.py:28-74` |
| 资源层 | `docs/资源层更新设计.md`、`xfly_client/resources.py` |
