# X-Plane 数据采集清单与模板（XConnect 插件）

> 插件通过 XPLM 读取 X-Plane dataref，以 5Hz 通过 TCP 推送给客户端。
> 本文档整理**可获取的信息**、提供**MD 模板**与**报文示例**。

---

## 1. 复制模板（新增字段时使用）

### 1.1 采集项表格模板

```markdown
| # | 数据项 | dataref 路径 | 类型 | 单位/范围 | 换算 | 示例值 | 当前 |
|---|--------|-------------|------|-----------|------|--------|------|
| 1 | 经度 | sim/flightmodel/position/longitude | float | 度 [-180,180] | — | 113.30785 | ✅ |
| 2 | 真空速 | sim/flightmodel/position/true_airspeed | float[2] | m/s | ×1.94384 → kt | 122.0 | ○ |
```

字段说明：

| 列 | 含义 |
|----|------|
| **类型** | `float` / `float[]` / `int` / `int[]` / `byte[]`(string) |
| **单位/范围** | 原始单位与有效区间（决定换算与限幅） |
| **换算** | 转为航空单位（ft/kt/MHz）的公式 |
| **示例值** | 真实场景采样值（可复制的示例报文） |
| **当前** | ✅ 插件已采集并上报 / ○ 未采集（可扩展） |

### 1.2 报文结构模板

```markdown
- `type`: 固定 `flight_data`
- `字段名`: 类型与含义
- 单位约定：高度 ft / 地速 kt / 频率 `MHz*1000` / 角度 deg
```

---

## 2. 当前已上报字段（`flight_data`，20 项）

实测示例（XP12 地面停机，ZSPD；修复前插件输出见括号注释）：

```json
{"type":"flight_data","aircraft_icao":"A20N","latitude":31.198351,"longitude":121.328453,
 "altitude_msl":18.5,"altitude_agl":0.1,"elevation":5.6,"pitch":-0.21,"bank":0.01,
 "heading":294.46,"true_heading":287.98,"groundspeed":0.0,"indicated_airspeed":14.1,
 "vertical_speed":-0.7,"com1_freq":133115,"com2_freq":136000,"transponder":450,
 "squark":"S","gear_deploy":0.0,"flaps_ratio":0.0,"throttle_ratio":0.0}
```

> 说明：
> - `com1_freq` / `com2_freq`：实测该 dataref 在 XP12 返回 **kHz**（133.115 MHz → 133115），
>   插件 `freq_to_mhz1000` 按量级自动识别 Hz/kHz/MHz 并四舍五入，统一输出 MHz*1000。
> - `transponder`：实测正确的 dataref 为 `sim/cockpit2/radios/actuators/transponder_code`
>   （旧路径 `sim/cockpit2/transponders/...` 不存在）。编码为八进制数码整型（0450 → 450）。
> - `aircraft_icao`：部分机型/时段返回空串，前端仅在非空时触发机型识别。

字段说明：

| 字段 | 类型 | 单位 | 含义 |
|------|------|------|------|
| `aircraft_icao` | string | ICAO 4 位 | 当前机型（`sim/cockpit2/aircraft/view/acf_icao`） |
| `latitude` / `longitude` | float | 度 | 位置 WGS84 |
| `altitude_msl` | float | ft | 海拔高度（`elevation` × 3.28084） |
| `altitude_agl` | float | ft | 离地高度（`y_agl` × 3.28084） |
| `elevation` | float | m | 地面海拔（机场标高） |
| `pitch` / `bank` | float | deg | 俯仰 / 坡度（`true_theta` / `true_phi`） |
| `heading` / `true_heading` | float | deg | 磁航向 / 真航向（`mag_psi` / `true_psi`） |
| `groundspeed` | float | kt | 地速（× 1.943844） |
| `indicated_airspeed` | float | kt | 指示空速 |
| `vertical_speed` | float | ft/min | 升降率 |
| `com1_freq` / `com2_freq` | int | `MHz*1000` | 活动/待机频率（118000 = 118.000） |
| `transponder` | int | 4 位码 | 应答机编码 |
| `squark` | string | `S`/`N` | 应答机模式（standby / 工作） |
| `gear_deploy` | float | 0–1 | 起落架放下比例 |
| `flaps_ratio` | float | 0–1 | 襟翼位置 |
| `throttle_ratio` | float | 0–1 | 油门 |

---

## 3. 可扩展采集项清单

### 3.1 位置与姿态

| # | 数据项 | dataref 路径 | 类型 | 单位/范围 | 换算 | 示例值 | 当前 |
|---|--------|-------------|------|-----------|------|--------|------|
| 1 | 经度 | sim/flightmodel/position/longitude | float | 度 | — | 113.30785 | ✅ |
| 2 | 纬度 | sim/flightmodel/position/latitude | float | 度 | — | 23.37011 | ✅ |
| 3 | 海拔 | sim/flightmodel/position/elevation | float | m | ×3.28084 → ft | 17.5 | ✅ |
| 4 | 离地高度 | sim/flightmodel/position/y_agl | float | m | ×3.28084 → ft | 0.0 | ✅ |
| 5 | 无线电高度 | sim/flightmodel/position/radar_height | float[3] | m | ×3.28084 → ft | 0.0 | ○ |
| 6 | 真空速 | sim/flightmodel/position/true_airspeed | float[2] | m/s | ×1.94384 → kt | 122.0 | ○ |
| 7 | 地速 | sim/flightmodel/position/groundspeed | float | m/s | ×1.94384 → kt | 120.0 | ✅ |
| 8 | 地速矢量 | sim/flightmodel/position/groundspeed_vector | float[3] | m/s | — | [0,-118,3] | ○ |
| 9 | 俯仰 | sim/flightmodel/position/true_theta | float | deg | — | -0.59 | ✅ |
| 10 | 坡度 | sim/flightmodel/position/true_phi | float | deg | — | 0.30 | ✅ |
| 11 | 航向(磁) | sim/flightmodel/position/mag_psi | float | deg | — | 0.00 | ✅ |
| 12 | 航向(真) | sim/flightmodel/position/true_psi | float | deg | — | 13.96 | ✅ |
| 13 | 偏航率 | sim/flightmodel/position/psi_rate | float | deg/s | — | 0.02 | ○ |
| 14 | 俯仰率 | sim/flightmodel/position/theta_rate | float | deg/s | — | -0.01 | ○ |
| 15 | 坡度率 | sim/flightmodel/position/phi_rate | float | deg/s | — | 0.05 | ○ |
| 16 | 升降率 | sim/flightmodel/position/vh_ind_fpm | float | ft/min | — | 0.0 | ✅ |

### 3.2 飞机识别

| # | 数据项 | dataref 路径 | 类型 | 单位 | 示例值 | 当前 |
|---|--------|-------------|------|------|--------|------|
| 1 | 机型 ICAO | sim/cockpit2/aircraft/view/acf_icao | string | 4 位 | A20N | ✅ |
| 2 | 呼号 | sim/cockpit2/aircraft/view/acf_descrip | string | — | CCA1501 | ○ |
| 3 | 注册号 | sim/cockpit2/aircraft/view/acf_tailnum | string | — | B-1234 | ○ |

### 3.3 发动机与控制

| # | 数据项 | dataref 路径 | 类型 | 单位 | 示例值 | 当前 |
|---|--------|-------------|------|------|--------|------|
| 1 | 发动机 1 推力 | sim/flightmodel/engine/ENGN_thro[0] | float | 0–1 | 0.85 | ○ |
| 2 | 发动机 N1 | sim/flightmodel/engine/ENGN_N1[0] | float | %（×100） | 62.4 | ○ |
| 3 | 发动机 EGT | sim/flightmodel/engine/ENGN_EGT[0] | float | degC | 435.0 | ○ |
| 4 | 燃油总览 | sim/flightmodel2/weight/I_current_fuel_total | float | kg（×2.20462 → lb） | 3200.0 | ○ |
| 5 | 襟翼 | sim/cockpit2/flight/indicators/flaps_ratio | float | 0–1 | 0.35 | ✅ |
| 6 | 油门 | sim/cockpit2/engine/indicators/throttle_ratio | float | 0–1 | 0.30 | ✅ |
| 7 | 起落架 | sim/flightmodel/gear/deploy_ratio | float[10] | 0–1 | 0.0 | ✅ |
| 8 | 刹车 | sim/cockpit2/controls/left_brake_ratio | float | 0–1 | 0.0 | ○ |
| 9 | 配平 | sim/flightmodel2/misc/trim | float | -1..1 | 0.05 | ○ |
| 10 | 暂停状态 | sim/time/total_running_time_sec | float | s | 3600.0 | ○ |
| 11 | 冻结 | sim/time/paused | int | 0/1 | 0 | ○ |
| 12 | 时间倍率 | sim/time/time_ratio | float | × | 1.0 | ○ |

### 3.4 无线电与应答机

| # | 数据项 | dataref 路径 | 类型 | 单位 | 示例值 | 当前 |
|---|--------|-------------|------|------|--------|------|
| 1 | COM1 活动频率 | sim/cockpit2/radios/actuators/com1_frequency_hz_833 | float | **kHz**(实测) / Hz(名义) | 自动识别 Hz/kHz/MHz → MHz*1000 | 133115 | ✅ |
| 2 | COM1 待机频率 | sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833 | float | **kHz**(实测) / Hz(名义) | 同上 | 121500 | ✅ |
| 3 | COM2 活动频率 | sim/cockpit2/radios/actuators/com2_frequency_hz_833 | float | **kHz**(实测) / Hz(名义) | 同上 | 136000 | ✅ |
| 4 | COM2 待机频率 | sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833 | float | **kHz**(实测) / Hz(名义) | 同上 | 124850 | ✅ |
| 5 | COM3 活动频率 | sim/cockpit2/radios/actuators/com3_frequency_hz_833 | float | **kHz**(实测) / Hz(名义) | 同上 | 118000 | ✅ |
| 6 | COM3 待机频率 | sim/cockpit2/radios/actuators/com3_standby_frequency_hz_833 | float | **kHz**(实测) / Hz(名义) | 同上 | 0(机型无) | ✅ |
| 7 | NAV1 频率 | sim/cockpit2/radios/actuators/nav1_frequency_hz | float | Hz | 同上 | 113000 | ○ |
| 8 | 频率显示 | sim/cockpit2/radios/actuators/com1_display_freq | float | MHz | ×1000 | 133.115 | ○ |
| 9 | 收发发射 | sim/cockpit2/radios/actuators/com1_tx_is_capable | int | 0/1 | — | 1 | ○ |
| 10 | 应答机编码 | sim/cockpit2/radios/actuators/transponder_code | int[] | 八进制数码整型（0450→450） | 4 位补零显示 | 450 | ✅ |
| 11 | 应答机模式 | sim/cockpit2/radios/actuators/transponder_mode | int | 0=OFF 1=STBY 2=ON 3=ALT 4=TEST 5=GND(ModeS) 6=TA 7=TA/RA | 原样上报 | 7 | ✅ |
| 12 | TCAS 高度过滤 | sim/cockpit2/radios/actuators/tcas_filter | int | 0=ALL 1=THRT 2=ABV 3=BLW 4=URSTR | 原样上报 | 0 | ✅ |
| 13 | TCAS 告警 | sim/cockpit2/tcas/indicators/tcas_alert | int | 0/1 | 原样上报 | 0 | ✅ |
| 14 | TCAS 活动咨询 | sim/cockpit2/tcas/indicators/tcas_active_advisory | int | 0=None 1=Traffic 2=Resolution | 原样上报 | 0 | ✅ |
| 9 | 应答机可用 | sim/cockpit2/radios/actuators/transponder_mode_available | int | 0/1 | 1 | ○ |
| 10 | 高度报告选择 | sim/cockpit2/radios/actuators/transponder_ident | int | 0/1 | 0 | ○ |

### 3.5 环境与系统

| # | 数据项 | dataref 路径 | 类型 | 单位 | 示例值 | 当前 |
|---|--------|-------------|------|------|--------|------|
| 1 | 外部温度 | sim/environment/temperature | float | degC | 18.5 | ○ |
| 2 | 风速 | sim/environment/wind_speed_kt | float | kt | 6.0 | ○ |
| 3 | 气压 | sim/weather/pressure_sl | float | inHg | 29.92 | ○ |
| 4 | 能见度 | sim/environment/visibility | float | m | 10000 | ○ |
| 5 | 电池 | sim/electrical/battery_on[0] | int | 0/1 | 1 | ○ |
| 6 | 灯光 | sim/cockpit2/lights/*_lights_on | int[] | 0/1 | 1 | ○ |
| 7 | 机场标识 | sim/flightmodel/position/airport_ID | int | — | 8199 | ○ |
| 8 | 燃油选择 | sim/flightmodel2/misc/fuel_tank_selected | int | 索引 | 0 | ○ |

---

## 4. 报文示例

### 4.1 巡航中（广州附近 A320，FL180）

```json
{"type":"flight_data","aircraft_icao":"A20N","latitude":23.37120,"longitude":113.80250,
 "altitude_msl":18100.0,"altitude_agl":18082.5,"elevation":17.5,
 "pitch":1.2,"bank":-2.4,"heading":275.0,"true_heading":279.5,
 "groundspeed":248.0,"indicated_airspeed":250.0,"vertical_speed":1200.0,
 "com1_freq":118000,"com2_freq":136000,"transponder":1200,"squark":"N",
 "gear_deploy":0.0,"flaps_ratio":0.0,"throttle_ratio":0.72}
```

### 4.2 地面停机（北京首都 T3）

```json
{"type":"flight_data","aircraft_icao":"B738","latitude":40.07989,"longitude":116.60310,
 "altitude_msl":116.4,"altitude_agl":0.0,"elevation":35.5,
 "pitch":-0.02,"bank":0.01,"heading":256.0,"true_heading":259.9,
 "groundspeed":0.0,"indicated_airspeed":0.0,"vertical_speed":0.0,
 "com1_freq":118300,"com2_freq":121700,"transponder":2000,"squark":"S",
 "gear_deploy":1.0,"flaps_ratio":0.0,"throttle_ratio":0.0}
```

### 4.3 CSL 加载状态（插件上报）

```json
{"type":"csl_status","loaded":["B738","A20N"],"missing":["A333"]}
```

---

## 5. 新增采集字段的步骤

1. **插件**（`XConnect/src/xconnect.c`）
   - `DrIndex` 枚举追加项 → `kDrefNames[]` 追加路径 → `build_flight_data()` 中读取并拼入 JSON
   - 字符串类用 `XPLMGetDatab`（参考 `acf_icao`）；数值类用 `dref_float()`/`dref_int()`（自动兼容 float/float[]/int/int[]）
2. **客户端**（`xfly_client/xconnect/client.py`）
   - `FlightData` 数据类追加字段与默认值
3. **界面**（`webui/app.js`）
   - `updateFlight(fd)` 中更新状态显示 / 自动识别逻辑
4. **验证**：插件日志出现 `send flight_data` 即已生效；调试模式（设置→调试模式）可查看原始报文

> 注意事项
> - **线程**：dataref 读取只允许在 XPLM 主线程（flight loop）中进行；网络线程禁止 XPLM 调用
> - **空引用**：部分 dataref 在特定机型/状态下可能不存在，读取前须判空（`dref_*` 已封装）
> - **单位**：高度统一 ft、地速 kt、频率 `MHz*1000`、角度 deg，避免客户端二次换算