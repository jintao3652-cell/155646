# XConnect 协议（TCP / JSON 行）

- 传输：TCP，端口 **51001**，UTF-8，每条消息一行（以 `\n` 结尾）
- 角色：**插件 = TCP 服务端**（监听），客户端主动连接
- 全字段速查（含两桥差异、单位、代码位置）：`MC-XC字段手册.md`

## 插件 → 客户端

### connected（连接建立，插件就绪）

```json
{"type":"connected","protocol":1,"plugin":"XConnect","version":"1.0.0",
 "sim":"xplane",
 "capabilities":["csl","parts","controls","diagnostics",
                 "position_vi","position_visual","acc"]}
```

`hello` / `hello_ack`（握手）：客户端发
`{"type":"hello","protocol":1,"client":"XFlySim-Client","version":"1.0.0"}`，
插件回 `hello_ack`，字段与 `connected` 相同但**没有 `sim`**。
插件只按 `type` 分发，不读取 `hello` 的其它字段。

### csl_status（CSL 加载状态）

```json
{"type":"csl_status","loaded":["B738","A320"],"missing":["XXXX"]}
```

XC **没有** MC 的 `sim` / `active` / `pending` 字段。`loaded` 为已有实例的 ICAO，
`missing` 为尝试过但实例数为 0 的 ICAO。


### flight_data（本机飞行数据，5Hz）

```json
{"type":"flight_data","aircraft_icao":"B738",
 "aircraft_name":"Boeing 737-800","flight_id":"N844X",
 "registration":"B-1234","registration_src":"tailnum","engine_count":2,
 "latitude":23.1300,"longitude":113.2000,
 "altitude_msl":3010.0,"altitude_agl":2510.0,"elevation":10.0,
 "pitch":1.5,"bank":-2.0,"heading":92.0,"true_heading":92.0,
 "groundspeed":248.0,"indicated_airspeed":250.0,"vertical_speed":0.0,
 "com1_freq":122800,"com2_freq":118050,"com3_freq":0,
 "com1_standby":121500,"com2_standby":124850,"com3_standby":0,
 "avionics_on":1,
 "com1_power":1,"com2_power":1,"com3_power":0,
 "audio_sel_com1":1,"audio_sel_com2":0,
 "audio_sel_com3":0,"has_audio_sel_com3":1,
 "audio_vol_com1":0.85,"audio_vol_com2":0.60,
 "audio_sel_com1_copilot":1,"audio_sel_com2_copilot":0,
 "audio_vol_com1_copilot":0.80,"audio_vol_com2_copilot":0.60,
 "tx_com":1,"pilot_xmit":0,
 "transponder":1234,"xpdr_mode":3,"squark":"N",
 "tcas_filter":0,"tcas_alert":0,"tcas_advisory":0,
 "xpdr_sys":-1,"xpdr_pwr":-1,"xpdr_tcas_mode":-1,"xpdr_tcas_alt":-1,
 "gear_deploy":0.0,"flaps_ratio":0.0,"throttle_ratio":0.3,
 "parts":{"lights":{"strobe_on":0,"landing_on":1,"taxi_on":0,
                    "beacon_on":1,"nav_on":1,"logo_on":0,
                    "wing_on":0,"recognition_on":1,"cabin_on":1},
          "gear_down":1,"flaps_pct":15,"spoilers_out":0,
          "engines":{"1":{"on":true},"2":{"on":true}},
          "on_ground":1}}
```

- 频率字段单位：`MHz*1000`（`122800` = 122.800 MHz）

  > ⚠️ **X-Plane 侧有四种频率单位，且不能靠数值大小区分**。实测（A330，座舱 121.800）：
  >
  > | dataref | 实测值 | 单位 |
  > |---|---|---|
  > | `com1_frequency_hz_833` | **121800** | kHz（8.33kHz 间隔）← 插件默认读这个 |
  > | `com1_frequency_hz` | 12180 | **10kHz** |
  > | `com1_frequency_Mhz` | 121 | MHz 整数部分（丢小数，别用） |
  > | 部分第三方机模 | 121.800 | MHz 浮点 |
  >
  > kHz 的 `121800` 与 10kHz 的 `12180` 都落在 `[1e4,1e7)`，**旧的按量级判断会把
  > 10kHz 值少乘 10**（121.800 显示成 12.180）。现已改为「试每种单位，取第一个
  > 落进 COM 波段 118.000~136.975 的结果」，四种单位都能正确归一化。
  > 往 `radio.json` 填机模私有 dataref 时，务必确认它用的是哪种单位。
- **机型辨识字段**（v16 起）：
  | 字段 | dataref | 说明 |
  |---|---|---|
  | `aircraft_name` | `sim/aircraft/view/acf_ui_name` | 机型 UI 名。**同一 ICAO 可能有三份不同机模**（Laminar/ToLiss/Fenix），只有 UI 名能区分，`radio.json` 的 `ui` 键就匹配它 |
  | `flight_id` | `sim/cockpit2/radios/actuators/flight_id` | Mode-S 播发的 Flight ID（`byte[8]`，≤7 字符，如 `N844X`/`KLM511`）。**不是 `.acf` 文件名，也不是 hexcode** |
- **音频面板字段**（v15 起，对齐 XVoice 的采集范围）：
  | 字段 | dataref | 说明 |
  |---|---|---|
  | `avionics_on` | `sim/cockpit/electrical/avionics_on` | 航电总电门；**断电时频率数值仍在但不可用** |
  | `com{1,2,3}_power` | `sim/cockpit2/radios/actuators/comN_power` | 单个 COM 电源开关。比 `avionics_on` 精确（可单独关某个 COM）；dataref 缺失时插件用 `avionics_on` 兜底 |
  | `audio_sel_com1/2/3` | `audio_selection_com{1,2,3}` | 音频面板是否选中收听该 COM |
  | `has_audio_sel_com3` | 同上（存在性） | 该机模是否**提供** COM3 收听 dataref。`0` 表示双台机型无此概念，前端不应把 COM3 画成"未选中" |
  | `audio_vol_com1/2` | `audio_volume_com{1,2}` | 收听音量 `[0..1]` |
  | `audio_sel_com1/2_copilot` | `audio_selection_com{1,2}_copilot` | 副驾驶音频面板收听状态（双人机组/前后座） |
  | `audio_vol_com1/2_copilot` | `audio_volume_com{1,2}_copilot` | 副驾驶收听音量 |
  | `tx_com` | `audio_com_selection` | 当前发话通道：`0`=无 `1`=COM1 `2`=COM2 `3`=COM3 |
  | `pilot_xmit` | 同上（0-based） | 与 MSFS 侧字段对齐：`0`=COM1 `1`=COM2 `2`=COM3，`-1`=未选中 |

  前端用 `avionics_on && audio_sel_comN` 决定 RMP 里该行频率是否"灰掉"——
  这是 XVoice 的做法：**频率有值 ≠ 可用**，只有通电且选中收听才是真正在守听的频道。
- 高度/地速单位：`altitude_msl` / `altitude_agl` / `altitude_indicated` 为 ft，
  `groundspeed` 为 kt，**但 `elevation` 是米原值**（`position/elevation` 未换算），
  **`indicated_airspeed` 也是 m/s 原值**——这两项与 MConnect 不同，跨桥混用需换算。
  `gear_deploy` 在 XC 是连续 0..1（MC 是二值 0/1）。
- `xpdr_sys` / `xpdr_pwr` / `xpdr_tcas_mode` / `xpdr_tcas_alt`：机型自定义枚举，
  `-1` 表示该机型没有对应 dataref（MC 恒为 `-1`）。
- `registration`：本机尾号。**涂装目录名优先于机模尾号**（v19 起）：
  1. `sim/aircraft/view/acf_livery_path` 涂装目录名里的尾号，如
     `.../liveries/Lufthansa D-AIKI` → `D-AIKI`
  2. `sim/aircraft/view/acf_tailnum` 机模自带尾号（兜底）

  > ⚠️ **为什么涂装优先**：`acf_tailnum` 是 `.acf` 里的**出厂值，不随涂装变化**。
  > 实测 X-Plane 自带 A330 的 `P acf/_tailnum` 恒为 `D-AIKN`，但选 Lufthansa
  > 涂装时真实尾号是 `D-AIKI`（目录 `liveries/Lufthansa D-AIKI`，目录内没有
  > 配置文件，尾号只存在于目录名）。**选涂装就是在选具体飞机**，所以目录名更准。
  > 若优先用 `acf_tailnum`，换任何涂装都会报出厂的那一架。
  >
  > `acf_livery_path` 官方标注 *"slow dataref, don't read a lot!"*，插件按 2 秒
  > 节流读取（涂装只可能在加载/切换机模时变化），不是 5Hz。
- `registration_src`：尾号来源 `livery`（涂装名）/ `tailnum`（机模尾号）/ `""`
  （都没有）。客户端按「手动填写 → 模拟器 → 同机型上次值 → 空」定最终值
- `engine_count`：真实发动机数量，读 `sim/aircraft/engine/acf_num_engines`
  （A333=2、B744=4、C172=1；0 = 未识别）
- `flaps_ratio`：0.0~1.0 的**实际展开**比例（dataref
  `sim/cockpit2/controls/flap_system_deploy_ratio`）。**不是手柄位置** ——
  手柄拨到 FULL 时目标是 1.0，但襟翼要几秒才真正放到 1.0；报目标值会让对端
  CSL 在你还没放完时就把襟翼画到全开。官方另两个同名字段里，
  `flap_ratio` 与 `flap_handle_deploy_ratio` 都已标"REPLACED"废弃，
  `flap_handle_request_ratio` 是手柄目标位置（不是我们要的）。
- `parts.flaps_pct`：**0..100 整数百分比**，公式 `round(ratio * 100)`
  （对齐 swift `simulatorxplane.cpp` 里 `qRound(flapsDeployRatio * 100.0)`
  传给 `CAircraftParts` 的第 3 参）。**不能发 0..1 比例** —— 服务器站点
  JSON 的 `flapsPct` 与 swift 的 `getFlapsPercent()` 都是整数百分比，
  发比例会让对端把 0.5 当成 "0%"。
  实现上先用区间判断滤掉 NaN（`(int)(NaN)` 是未定义行为，MSVC 给
  INT_MIN，会让 JSON 出现 -2147483648 导致服务器丢弃整个 ACC 包 ——
  这个坑真踩过），再夹到 0..100。
- `parts.engines`：**JSON 对象**，键为发动机编号（1 基），值**只含 `{on}`**。
  按 `engine_count` 生成长度，不再固定 4 组（双发机以前会多报两台永远关车的
  "幽灵引擎"，对端据此判机型会错）。

  > ⚠️ **是对象不是数组**。swift 的 `CAircraftEngineList::toJson()` 返回的是
  > `QJsonObject`，实现为 `map.insert(QString::number(e.getNumber()), e.toJson())`，
  > 即 `{"1":{"on":false},"2":{"on":false}}`。
  > 曾经发成数组 `[{"on":..},..]`，类型不匹配 → 服务器解析失败，站点 JSON 里
  > `aircraft.engines` 恒为 `null`。

  > `on` 的判定优先用 **`sim/flightmodel/engine/ENGN_running`**
  > （X-Plane 官方语义："Engine on and using fuel"，int[16]，下标 0 = 1 号引擎），
  > 这比"N1 > 5%"可靠：起动过程中 N1 已经上来但引擎尚未稳定运转，靠阈值会误报。
  > 没有该 dataref 的极老版本才退回 N1 阈值。

  > ⚠️ **值里只能有 `on`，不要加 `rpm_pct`**。虽然 swift 的 `CAircraftEngine`
  > 定义里 `rpmPct` 没有 `DisabledForJson`（理论上会序列化），但 swift 客户端
  > 实际上报与服务器回显都只有 `{"on":false}`。实测带 `rpm_pct` 时服务器返回
  > `"engines": {}`（空对象，key 全丢）；去掉后才有内容。
  > 结论：**服务器只认它认识的键，多余键会导致该台引擎被丢弃**。
- **写入方向只消费 6 种灯**：客户端 `data[].parts.lights` 里的
  `wing_on` / `recognition_on` / `cabin_on` 会被插件忽略，只有
  `beacon/landing/taxi/nav/strobe/logo` 生效（`xconnect.c:226-238`）。
  上报方向（`flight_data.parts`）9 种灯全发。
- `parts` 里**没有 `throttle_ratio`**，油门只在顶层 `throttle_ratio`。
- `squark`：`"S"` 应答机开启、`"N"` 应答机关闭
- `xpdr_mode`：XP12 应答机模式枚举 `0=OFF 1=STBY 2=ON 3=ALT 4=TEST 5=GND(ModeS) 6=TA 7=TA/RA`
- `tcas_filter`：TCAS 高度过滤 `0=ALL 1=THRT 2=ABV 3=BLW 4=URSTR`
- `tcas_alert`：TCAS 告警 0/1；`tcas_advisory`：`0=None 1=Traffic 2=Resolution`
- `com3_*` / `com*_standby`：机型未提供该台时数值为 `0`（前端显示 `———`）

## 客户端 → 插件

### Send:OPD:FSD9（在线玩家列表 → CSL 渲染）

```json
{"type":"Send:OPD:FSD9","mycid":"1001",
 "data":[{"callsign":"CCA999","latitude":23.1,"longitude":113.0,
          "altitude":3000,"pitch":1.0,"bank":-5.0,"heading":90.0,
          "ground_speed":250,"aircraft":"B738","aircraft_family":"",
          "airline":"CCA","registration":"B-1234",
          "on_ground":false,"altitude_agl":-1}]}
```

- `callsign`：玩家呼号（主键）
- `latitude/longitude`：WGS84 度
- `altitude`：真高 ft（MSL）
- `pitch/bank/heading`：度
- `ground_speed`：kt
- `aircraft`：机型 ICAO，用于查找 `CSL/{ICAO}/xsb_aircraft.txt`。库里没有该
  型号时按内置别名表回退同族模型（`FA8X`/`GLF6`→`F900`、`B38M`→`B738`、
  `E75L`→`E175` …），再回退 `CSL/ZZZZ` 白机；空串按 `ZZZZ` 处理。
  **空串 = 未识别**（客户端不再猜默认机型）→ 插件回退通用 `CSL/ZZZZ`
- `airline`：涂装/航司码（PI 的 `AIRLINE`/`LIVERY`，或对方 `#SB PI` 上报值，
  或呼号前三位），对应 xsb 中 `AIRLINE`/`LIVERY` 的变体码
- `registration`：飞机注册号/尾号（`B-1234`、`N829PL`；空串 = 未知）。来源：
  对端 `#SB PI` 的 `LIVERY` 字段（本客户端把尾号放在这里，才能被 swift 中继
  转发给第三方）或 `REG=` 扩展字段；swift 原生只发涂装/航司码，因此对端为
  swift 时通常为空
- `on_ground`：遥测 onGround（PBH bit1 / `#SB VI`）。为真且速度 < 100kt 时，
  插件忽略高度字段的偏差，直接把模型落到地形高度
- `altitude_agl`：遥测离地高度 ft（`^`/`#SL`/`#ST` 包携带），`<0` 表示未知；
  AGL ≈ 0 也按"在地面"处理
- 未在列表中或超过 30s 未更新的玩家，插件移除其模型

> **复合模型与地面吸附**：X-CSL 的一架飞机由多个 OBJ 组成（机身涂装 + 发动机/
> 风扇等），插件解析 `CSL/{ICAO}/xsb_aircraft.txt` 的 `OBJ8_AIRCRAFT` 块逐个实例化。
> **变体匹配四级规则**（日志里以 `match=REG/AIRLINE/TYPE/FALLBACK` 标出）：
> 1. `REG` —— 注册号一致：变体名、OBJ 文件名（`B738_CCA_B-1234.obj`）或
>    `AIRLINE`/`LIVERY` 行尾注记（`LIVERY B738 CCA B-1234`）含同一尾号
>    （忽略大小写与 `-`）→ 同一架飞机的专属涂装/模型；
> 2. `AIRLINE` —— 同航司 + 同机型：变体码精确匹配，再"同航司带后缀"
>    （`AAL`→`AALWL`）；同一航司有多个变体时**优先不绑定具体尾号的那个**；
> 3. `TYPE` —— 同机型任意涂装：`ZZZ`（通用白机）→ `ANY`/`GEN` →
>    无涂装基础变体 → 包内 `CSL/{ICAO}/{ICAO}.obj` → 首个变体；
> 4. `FALLBACK` —— 机型别名同族模型（`FA8X`→`F900`）→ `CSL/ZZZZ` 白机。
> 不会"无脑取第一个变体"给不存在的航司挂别家涂装。
>
> 高度方面：遥测 `altitude` 是飞机参考点（重心）的高度，而 CSL 模型原点在轮胎
> 接地点；部分客户端上报的还是气压/指示高度基准（QNH≠1013 时可差数百英尺）。
> 因此渲染时：**onGround 为真且速度 <100kt → 直接落到地形高度**；否则近地
> （AGL ≤ 8m）同样贴地（等价 XPMP2 `clampToGround`），消除整机悬空/陷入地面。

> **平滑渲染**：列表中每架飞机的位置按**接收时刻**记入 6 帧环形缓冲；每个
> 渲染帧用 `now - 渲染滞后` 在历史中找包夹该时刻的相邻两帧线性插值（航向取
> 最短弧 ±180° 回绕）。渲染滞后**自适应 ≥ 帧到达间隔**（常规 6s，快速更新
> 1.5s，上限 8s；等价 Swift 的 6000/1500ms 时间偏移），因此渲染时刻总是落在
> 已知两帧之间——**纯插值、永不外推、永不回跳**。仅当下一帧迟到（超过
> 期望间隔）时才沿最近两帧短时外推（≤3s），数据停滞则停在最新帧。首帧与
> >60km 的遥传跳变（重连/重生）直接吸附。因此 **5s/3s/1s 位置更新都能得到
> 连续运动**，且不受主循环帧率影响。

### Send:ATC（管制列表，用于可选标注）

```json
{"type":"Send:ATC",
 "data":[{"callsign":"ZGGG_TWR","frequency":"118.800","type":"TWR",
          "latitude":23.43,"longitude":113.20,"rating":2}]}
```

插件只取 `data` 的**数量**存 `g_atc_count`（无管制模型可注入），不读任何子字段。

### Send:RADIO（设置无线电频率 / 触发音频面板动作）

```json
{"type":"Send:RADIO","com":1,"active":true,"freq":122800}
{"type":"Send:RADIO","com":2,"active":false,"freq":121500}
{"type":"Send:RADIO","com":1,"swap":true}       // 主备互换（命令路径）
{"type":"Send:RADIO","com":1,"flip":true}       // 同 swap
{"type":"Send:RADIO","com":1,"tx":true}         // 切发话通道
{"type":"Send:RADIO","com":1,"xmit":true}       // 同 tx（MConnect 字段互通）
{"type":"Send:RADIO","com":2,"monitor":true}    // 收听开关 toggle
{"type":"Send:RADIO","recv_all":true}           // 全收开关（MConnect 字段互通）
{"type":"Send:RADIO","com":1,"power":0}         // COM 电源 off（1=on）
```

| 字段 | 含义 |
|---|---|
| `com` | 1..3，越界拒收 |
| `active` | `true`=主频，`false`=备频；缺省按主频 |
| `freq` | **MHz×1000**（`122800` = 122.800 MHz），与 `flight_data.com*_freq` 同单位 |
| `swap` / `flip` | 主备互换，走**命令** `sim/radios/comN_standy_flip`，忽略 `freq` |
| `tx` / `xmit` | 切发话通道，走命令 `sim/audio_panel/transmit_audio_comN` |
| `monitor` | 收听开关（toggle），走命令 `sim/audio_panel/monitor_audio_comN` |
| `recv_all` | 全收开关：按 `audio_selection_com1..3` 当前读数对不符的台触发 monitor toggle（X-Plane 无"全收"命令），`true`=全收 `false`=全关 |
| `power` | COM 电源，`1`/`0` → `sim/radios/power_comN_on/off` |

**两种反写通道，各有适用场景**（照 MobiFlight 的 DataRef vs Command 双通道设计）：

| 动作 | 通道 | 为什么 |
|---|---|---|
| 设频率 | 直写 dataref | 频率是"数值"，写 dataref 精确可控 |
| 主备互换 | **命令** | 机模内部监听命令并同步面板；直接改两个 dataref 会让面板显示与实际值脱节（ToLiss/Fenix 尤其明显） |
| 发话/收听/电源 | **命令** | 同上，这些是"开关动作"而非"数值赋值" |

设频率的有效波段 `118000 ≤ freq ≤ 136975`，越界拒收。
（上界是 **136.975** 不是 137.000 —— 137.000 是波段预留边界，不是可调频率；
8.33kHz 间隔下最高合法值是 136.975。）
命令按名称懒解析并缓存（`XPLMFindCommand`），找不到会记
`control: command not found: <name>`。
注意 X-Plane 官方把 standby 拼成了 **`standy`**（`com1_standy_flip`），不是笔误。

### Send:XPDR（设置应答机）

```json
{"type":"Send:XPDR","code":"1234"}
{"type":"Send:XPDR","code":1234}
{"type":"Send:XPDR","state":3}
```

| 字段 | 含义 |
|---|---|
| `code` | 4 位码，**每位必须 0..7**。字符串或数字都接受 |
| `state` | 模式 `0=Off 1=Stby 2=On(A) 3=Alt(C) 4=Test 5=GND 6=TA only 7=TA/RA`，越界夹到 0..7 |
| `ident` | 触发 IDENT（瞬时脉冲，`false` 无法取消，只记日志忽略）|

X-Plane 侧的两种通道：

| 动作 | 通道 | 说明 |
|---|---|---|
| 应答机码 | 直写 dataref | `sim/cockpit2/radios/actuators/transponder_code` |
| 应答机模式 | **命令** | `sim/transponder/transponder_{off,standby,on,alt,test,ground,ta_only,ta_ra}`，机模面板会同步；命令不存在时退回写 dataref |
| IDENT | **命令** | `sim/transponder/transponder_ident`。`transponder_id` 是**只读** dataref，只能靠命令触发 |

> **两平台差异（对客户端透明）**：X-Plane 的频率/应答机 dataref 可写，直接
> 改数据；MSFS 的同名 SimVar 是**只读**，MConnect 走 SimConnect 事件。协议
> 报文格式完全一致，客户端不需要区分。
>
> MSFS 的 `TRANSPONDER STATE` 枚举顺序与本文不同（`2=Test 3=On 4=Alt`），
> MConnect 内部会重映射——不重映射会让"设为 ON"打到 Test 位，飞行中应答机
> 不响应询问，管制看不到该航班。

> **写入语义**：命令由网络线程入队（64 条，满则丢最旧）、主线程执行，
> 存在一帧延迟；且**不保证生效**（机模可能自己覆盖，或不实现该事件）。
> 客户端必须以回读的 `flight_data` 为准刷新界面。

### Send:SCAN（重扫 dataref，诊断用）

```json
{"type":"Send:SCAN"}
```

让插件在下一帧重新枚举全部已注册 dataref，并把名字、**当前值**、是否可写写进
`xconnect.log` 的 `dref:` 行。换机模后查私有 dataref 不必重启 X-Plane。

配套工具：

```bash
python tools/dref_find.py                    # 触发重扫并列出全部匹配
python tools/dref_find.py AirbusFBW          # 只看该前缀
python tools/dref_find.py com VHF audio      # 多关键字（名字或值命中）
python tools/dref_find.py --no-scan          # 只读日志已有结果
```

> **为什么不用 `Resources/plugins/DataRefs.txt` 查机模私有 dataref**：
> 实测该文件仅含 Laminar 原生条目（`AirbusFBW`、`laminar/B738` 匹配数均为 0），
> 且是某次运行的静态导出快照。第三方机模的私有 dataref 在加载 `.acf` 时才注册
> 到 XPLM，**只有运行时枚举才看得到**。

### Send:EFB:Weather / Send:EFB:Route（预留，插件暂不处理）

## 会话管理

- 客户端断开：插件清空全部 CSL 实例与管制数据
- 同一时刻仅接受一个客户端连接（新连接替换旧连接）
- 插件不重连（非阻塞；X-Plane 重启后客户端需重连）
## 机型适配表 `radio.json`

放在 `Resources/plugins/XConnect/radio.json`，用于给**使用私有 dataref** 的机模
（ToLiss / FlightFactor / Fenix 等）覆盖通用 X-Plane dataref。空表或不放文件都正常，
此时全部走通用 dataref，行为与没有本功能时完全一致。

```json
{
  "aircraft": [
    { "icao": "A321", "ui": "ToLiss", "reg": "", "name": "可选备注",
      "power":        "<覆盖 avionics_on 的 dataref>",
      "com": [ { "active": "...", "standby": "..." },   // COM1
               { "active": "...", "standby": "..." },   // COM2
               { "active": "...", "standby": "..." } ], // COM3
      "audio": [ { "select": "...", "volume": "..." },  // COM1
                 { "select": "...", "volume": "..." } ],
      "audio_com_sel": "..." }
  ]
}
```

匹配优先级（高 → 低）：**`icao`+`ui`+`reg` > `icao`+`ui` > `icao`+`reg` > 仅 `icao`**

- `icao` 必填，对应 `sim/aircraft/view/acf_ICAO`（大小写不敏感）
- `ui` 可选，`acf_ui_name` 的**子串**匹配（大小写不敏感）。
  **同一 ICAO 下可能有三份完全不同的机模**（Laminar A320 / ToLiss A320 / FenixA320），
  它们的无线电 dataref 各不相同——只看 ICAO 无法区分，必须靠 `ui`。
  例如 `"ui": "ToLiss"` 可匹配 `"ToLiss A321"`。
- `reg` 可选，尾号精确匹配
- 未命中 → 全部通用 dataref
- 每个字段都可留空；写了但 `XPLMFindDataRef` 失败会自动回退通用值并写日志

只覆盖「读」。**「写」不走覆盖**：自定义 dataref 未必可写，贸然 `XPLMSetData`
可能污染机模内部状态。反写（频率/应答机）始终用通用 dataref。

> 为什么不只靠 ICAO：`acf_ICAO` 只是机型码。查 MobiFlight 社区配置可见，
> ToLiss 系全部用 `AirbusFBW/*` 私有前缀，而 Laminar 同型机用标准
> `sim/cockpit2/radios/*`。两者 ICAO 相同、dataref 完全不同，`ui` 是唯一的区分手段。

启动日志会打印 `radio.json: loaded N aircraft override(s)`，以及每次换机型时的
`radio.json: matched ...` 或 `radio.json: no match for ...`，便于确认是否命中。

> 设计参考 XVoice 3.8.17 的 `simaircrafttable.bytes`（Luban 二进制表），改成外部
> JSON 是为了让用户自己加机型不必重编译插件。详见仓库根目录
> `XVoice频率与Squawk获取机制分析.md`。
