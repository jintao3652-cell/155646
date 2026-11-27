# X-Plane 12 飞行数据采集清单（XConnect 插件）

> 采集源：XConnect 插件（`XConnect/src/xconnect.c`），X-Plane 12 运行中实测。
> 传输：TCP 127.0.0.1:51001，JSON 行，每行一条，5Hz 推送。
> 实测场景：上海浦东（ZSPD）地面停机，XP12。

---

## 1. 实测报文（2026-10-05 运行中 XP12 捕获）

```json
{
  "type": "flight_data",
  "aircraft_icao": "",
  "latitude": 31.198351,
  "longitude": 121.328453,
  "altitude_msl": 18.5,
  "altitude_agl": 0.1,
  "elevation": 5.6,
  "pitch": -0.21,
  "bank": 0.01,
  "heading": 294.46,
  "true_heading": 287.98,
  "groundspeed": 0.0,
  "indicated_airspeed": 14.1,
  "vertical_speed": -0.7,
  "com1_freq": 133,
  "com2_freq": 136,
  "transponder": 0,
  "squark": "N",
  "gear_deploy": 0.0,
  "flaps_ratio": 0.0,
  "throttle_ratio": 0.0
}
```

> 机上仪表实读：COM1 **133.115**、COM2 **136.000**、应答机 **0450**（模式 ALT）。
> 上表为修复前的插件输出：`com1_freq=133`（dataref 返回 kHz 被 /1000 截断）、
> `transponder=0`（dataref 路径错误）。修复后应为
> `com1_freq=133115` / `com2_freq=136000` / `transponder=450` / `squark="S"`。

## 2. 字段总表

| 字段 | 类型 | 单位 | dataref | 换算 | 实测值 |
|------|------|------|---------|------|--------|
| `aircraft_icao` | string | ICAO | sim/cockpit2/aircraft/view/acf_icao | — | （空，见 §4） |
| `latitude` | float | 度 | sim/flightmodel/position/latitude | — | 31.198351 |
| `longitude` | float | 度 | sim/flightmodel/position/longitude | — | 121.328453 |
| `altitude_msl` | float | ft | sim/flightmodel/position/elevation | ×3.28084 | 18.5 |
| `altitude_agl` | float | ft | sim/flightmodel/position/y_agl | ×3.28084 | 0.1 |
| `elevation` | float | m | sim/flightmodel/position/elevation | — | 5.6 |
| `pitch` | float | deg | sim/flightmodel/position/true_theta | — | -0.21 |
| `bank` | float | deg | sim/flightmodel/position/true_phi | — | 0.01 |
| `heading` | float | deg | sim/flightmodel/position/mag_psi | 磁航向 | 294.46 |
| `true_heading` | float | deg | sim/flightmodel/position/true_psi | 真航向 | 287.98 |
| `groundspeed` | float | kt | sim/flightmodel/position/groundspeed | ×1.943844 | 0.0 |
| `indicated_airspeed` | float | kt | sim/flightmodel/position/indicated_airspeed | — | 14.1 |
| `vertical_speed` | float | ft/min | sim/flightmodel/position/vh_ind_fpm | — | -0.7 |
| `com1_freq` | int | MHz*1000 | sim/cockpit2/radios/actuators/com1_frequency_hz_833 | 见 §3 | 133→133115 |
| `com2_freq` | int | MHz*1000 | sim/cockpit2/radios/actuators/com2_frequency_hz_833 | 见 §3 | 136→136000 |
| `com3_freq` | int | MHz*1000 | sim/cockpit2/radios/actuators/com3_frequency_hz_833 | 见 §3 | 0（机型无 COM3） |
| `com1_standby` | int | MHz*1000 | sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833 | 见 §3 | 待机频率 |
| `com2_standby` | int | MHz*1000 | sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833 | 见 §3 | 待机频率 |
| `com3_standby` | int | MHz*1000 | sim/cockpit2/radios/actuators/com3_standby_frequency_hz_833 | 见 §3 | 0（机型无） |
| `transponder` | int | 4 位码 | sim/cockpit2/radios/actuators/transponder_code | — | 0→450 |
| `xpdr_mode` | int | 枚举 | sim/cockpit2/radios/actuators/transponder_mode | 0=OFF 1=STBY 2=ON 3=ALT 4=TEST 5=GND 6=TA 7=TARA | — |
| `squark` | string | S/N | （由 xpdr_mode≥2 派生） | — | N→S |
| `tcas_filter` | int | 枚举 | sim/cockpit2/radios/actuators/tcas_filter | 0=ALL 1=THRT 2=ABV 3=BLW 4=URSTR | — |
| `tcas_alert` | int | 0/1 | sim/cockpit2/tcas/indicators/tcas_alert | — | 0 |
| `tcas_advisory` | int | 枚举 | sim/cockpit2/tcas/indicators/tcas_active_advisory | 0=None 1=Traffic 2=Resolution | 0 |
| `gear_deploy` | float | 0–1 | sim/flightmodel/gear/deploy_ratio | — | 0.0 |
| `flaps_ratio` | float | 0–1 | sim/cockpit2/flight/indicators/flaps_ratio | — | 0.0 |
| `throttle_ratio` | float | 0–1 | sim/cockpit2/engine/indicators/throttle_ratio | — | 0.0 |

## 3. 频率 dataref 的单位（实测坑）

`sim/cockpit2/radios/actuators/com1_frequency_hz_833` 名义单位是 Hz，但
**实测 XP12（当前机型）返回 kHz**：133.115 MHz → 133115。旧插件直接
`value / 1000` 并截断 → 133，前端补位显示成 100.133。

修复（`freq_to_mhz1000`，`XConnect/src/xconnect.c`）：按量级自动识别并四舍五入，
兼容 Hz / kHz / MHz 三种返回，统一输出 MHz*1000：

```c
if (raw >= 1e7) return (long)(raw / 1000.0 + 0.5);   /* Hz */
if (raw >= 1e4) return (long)(raw + 0.5);            /* kHz */
return (long)(raw * 1000.0 + 0.5);                   /* MHz */
```

前端 `fmtFreq` 移除 `<100000 补 100000` 的历史 hack，直接
`Math.floor(m/1000).(m%1000 补 3 位)` → `133.115`。

## 4. 应答机 dataref 路径（实测坑）

插件原用 `sim/cockpit2/transponders/transponder_code` 与
`.../transponder_mode`——**该路径不存在**，`XPLMFindDataRef` 返回 NULL → 恒 0/恒 "N"。
正确路径在 `sim/cockpit2/radios/actuators/` 下：

| 数据 | dataref（正确） | 说明 |
|------|----------------|------|
| 应答机编码 | sim/cockpit2/radios/actuators/transponder_code | int 数组，八进制数码整型（0450 → 450） |
| 应答机模式 | sim/cockpit2/radios/actuators/transponder_mode | 0=OFF 1=STBY 2=ON 3=ALT |

前端以 4 位补零显示：`String(450).padStart(4,'0')` → `0450`。

## 5. 其它实测发现

- `aircraft_icao` 在部分机型/时段返回空串（实测为空）；前端自动识别仅在非空时触发。
- `heading`（磁航向）与 `true_heading`（真航向）存在磁差：294.46 vs 287.98。
- 地面静止时 `groundspeed=0`、`indicated_airspeed` 有 ±14 kt 的静噪。
- 插件为**单客户端**：一个客户端占用 51001 期间，新连接不会接管（进入 backlog）。
- **应答机模式枚举**（XP12）：`transponder_mode` 同时表达 TCAS ModeS 选择——
  5=GND、6=TA、7=TA/RA；`tcas_filter` 即 TCAS 高度范围 ALL/ABV/BLW/THRT。
- **机型自定义 dataref**：应答机 1/2 号选择、STBY/AUTO/ON 空客模式等不在 X-Plane
  通用 dataref 中。插件 v6.3 启动时用 `XPLMCountDataRefs` 扫描所有已注册
  dataref，把含 `tcas/xpdr/transponder` 关键词的路径写入 `xconnect.log`
  （前缀 `dref:`）。已实测发现 ToLiss A321（AirbusFBW）：

  | 字段 | dataref | 语义（待旋钮校准） |
  |------|---------|--------------------|
  | 应答机系统选择 | AirbusFBW/XPDRSystem | 1/2 号应答机 |
  | 应答机电源 | AirbusFBW/XPDRPower | OFF/STBY/AUTO/ON |
  | TCAS 模式 | AirbusFBW/XPDRTCASMode | STBY/TA/TARA |
  | TCAS 高度选择 | AirbusFBW/XPDRTCASAltSelect | ALL/ABV/BLW/THRT |
  | 面板动画值 | ckpt/transponder/{sys,pwrx,alt,mode}/anim | 旋钮位置动画 |

  插件 v6.4 上报上述字段原始值（无该 dataref 时 -1），并把变化记录到
  xconnect.log（前缀 `xpdrv:`），用于校准数值→标签映射。

## 6. 对应修复版本

- 插件 `XConnect` v6.3（`build/XConnect.xpl`）：频率单位修复、应答机 dataref
  路径修复、VHF1/2/3 待机频率、xpdr_mode/tcas_* 上报、启动 dataref 扫描。
  需在 X-Plane 退出（或插件管理禁用）后替换
  `Resources/plugins/XConnect/win_x64/` 下的旧文件。
- 前端 `webui`（`npm run build` 产物已更新）：频率 4 位补零、应答机/TCAS 状态显示。
