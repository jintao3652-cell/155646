# XConnect CSL 动画 dataref 存在性核对（XP12 运行中实读）

> 核对对象：`Resources/plugins/XConnect/CSL/` 下 **242 个 OBJ 模型**全部 `ANIM_*` 键，
> 对照 `csl.c::kCslDataRefs` 实例 dataref 清单。
> 绑定机制：`XPLMInstance` 按**精确字符串**把实例 dataref 名与 OBJ 动画键匹配——
> 名字对不上（含 `[0]` 索引后缀差异）即该通道无值。

## 1. 结论

- 模型实际用到 **21 个 `libxplanemp/...` 键 + 8 个 `sim/...` 全局键**。
- 修复前 `kCslDataRefs` 与模型键**不完全匹配**：缺失 **12 个数组索引形态**（`[0]`/`[1]`/`[2]`），
  导致起落架（`gear_ratio[0..2]`）、引擎风扇（`engine_rotation_angle_deg[0]`）、
  轮胎旋转/下压（`tire_*[0..2]`）等通道全部无值 → 动画不显示。
- 全局 `sim/...` 键由 X-Plane OBJ8 直接读取（本机 dataref），无需实例提供。

## 2. 模型实际使用的 libxplanemp 键 vs 修复前清单

| # | 模型 ANIM 键（精确形态） | 修复前在清单 | 修复后 | 说明 |
|---|---|---|---|---|
| 1 | libxplanemp/controls/beacon_lites_on | ✅ | ✅ | 频闪信标 |
| 2 | libxplanemp/controls/flap_ratio | ✅ | ✅ | 襟翼 |
| 3 | libxplanemp/controls/gear_ratio | ✅ | ✅ | 起落架（基名） |
| 4 | libxplanemp/controls/gear_ratio[0] | ❌ | ✅ | 起落架 1 号（索引） |
| 5 | libxplanemp/controls/gear_ratio[1] | ❌ | ✅ | 起落架 2 号 |
| 6 | libxplanemp/controls/gear_ratio[2] | ❌ | ✅ | 起落架 3 号 |
| 7 | libxplanemp/controls/landing_lites_on | ✅ | ✅ | 着陆灯 |
| 8 | libxplanemp/controls/nav_lites_on | ✅ | ✅ | 航行灯 |
| 9 | libxplanemp/controls/nws_ratio | ✅ | ✅ | 前轮转向 |
| 10 | libxplanemp/controls/slat_ratio | ✅ | ✅ | 缝翼 |
| 11 | libxplanemp/controls/speed_brake_ratio | ✅ | ✅ | 减速板 |
| 12 | libxplanemp/controls/spoiler_ratio | ✅ | ✅ | 扰流板 |
| 13 | libxplanemp/controls/strobe_lites_on | ✅ | ✅ | 频闪灯 |
| 14 | libxplanemp/controls/taxi_lites_on | ✅ | ✅ | 滑行灯 |
| 15 | libxplanemp/controls/thrust_ratio | ✅ | ✅ | 推力 |
| 16 | libxplanemp/controls/thrust_revers | ✅ | ✅ | 反推 |
| 17 | libxplanemp/controls/yoke_heading_ratio | ✅ | ✅ | 操纵杆航向 |
| 18 | libxplanemp/controls/yoke_pitch_ratio | ✅ | ✅ | 操纵杆俯仰 |
| 19 | libxplanemp/controls/yoke_roll_ratio | ✅ | ✅ | 操纵杆横滚 |
| 20 | libxplanemp/engines/engine_rotation_angle_deg[0] | ❌ | ✅ | 引擎风扇转角（索引） |
| 21 | libxplanemp/engines/engine_rotation_speed_rad_sec[0] | ❌ | ✅ | 引擎风扇转速（索引） |
| 22 | libxplanemp/gear/tire_rotation_angle_deg | ✅ | ✅ | 轮胎转角（基名） |
| 23 | libxplanemp/gear/tire_rotation_angle_deg[0] | ❌ | ✅ | 轮胎 1 转角 |
| 24 | libxplanemp/gear/tire_rotation_angle_deg[1] | ❌ | ✅ | 轮胎 2 转角 |
| 25 | libxplanemp/gear/tire_rotation_angle_deg[2] | ❌ | ✅ | 轮胎 3 转角 |
| 26 | libxplanemp/gear/tire_vertical_deflection_mtr[0] | ❌ | ✅ | 轮胎 1 垂直压缩 |
| 27 | libxplanemp/gear/tire_vertical_deflection_mtr[1] | ❌ | ✅ | 轮胎 2 垂直压缩 |
| 28 | libxplanemp/gear/tire_vertical_deflection_mtr[2] | ❌ | ✅ | 轮胎 3 垂直压缩 |
| 29 | libxplanemp/misc/touch_down | ✅ | ✅ | 触地标志 |

**修复前缺失 12 项（红色 ❌），修复后 29 项全部在清单。**

## 3. 模型直接读取的全局 `sim/...` 键（无需实例提供）

| 键 | 说明 |
|---|---|
| sim/flightmodel2/engines/engine_rotation_angle_deg[0..1] | 引擎风扇转角（本机全局） |
| sim/flightmodel2/gear/deploy_ratio[0] | 本机起落架 |
| sim/flightmodel2/lights/nav_lights_brightness_ratio[0] | 本机航行灯亮度 |
| sim/flightmodel2/wing/aileron1_deg[2..3] | 副翼 |
| sim/graphics/animation/airport_beacon_rotation | 机场信标旋转 |
| sim/time/total_flight_time_sec / total_running_time_sec | 时间驱动动画 |

> 这些是 X-Plane 全局 dataref，OBJ8 渲染引擎自动读取（反映本机状态），
> 与实例 dataref 无关。

## 4. 修复后清单（csl.c::kCslDataRefs，34 项）

```
libxplanemp/controls/gear_ratio        libxplanemp/controls/gear_ratio[0]
libxplanemp/controls/gear_ratio[1]     libxplanemp/controls/gear_ratio[2]
libxplanemp/controls/flap_ratio        libxplanemp/controls/spoiler_ratio
libxplanemp/controls/speed_brake_ratio libxplanemp/controls/slat_ratio
libxplanemp/controls/wing_sweep_ratio  libxplanemp/controls/thrust_ratio
libxplanemp/controls/yoke_pitch_ratio  libxplanemp/controls/yoke_heading_ratio
libxplanemp/controls/yoke_roll_ratio   libxplanemp/controls/thrust_revers
libxplanemp/controls/taxi_lites_on     libxplanemp/controls/landing_lites_on
libxplanemp/controls/beacon_lites_on   libxplanemp/controls/strobe_lites_on
libxplanemp/controls/nav_lites_on      libxplanemp/controls/nws_ratio
libxplanemp/engines/thrust_reverser_deploy_ratio
libxplanemp/engines/prop_rotation_angle_deg
libxplanemp/engines/engine_rotation_angle_deg
libxplanemp/engines/engine_rotation_angle_deg[0]
libxplanemp/engines/engine_rotation_speed_rad_sec
libxplanemp/engines/engine_rotation_speed_rad_sec[0]
libxplanemp/gear/tire_rotation_angle_deg
libxplanemp/gear/tire_rotation_angle_deg[0..2]
libxplanemp/gear/tire_vertical_deflection_mtr[0..2]
libxplanemp/misc/touch_down
```

`CSL_MAX_DATAREFS=48` 足够容纳；`players.c` 按新索引填值（0-3 起落架、
14-18 五组灯光，其余 0）。

## 5. 附：核对方法

- 模型扫描：对 242 个 `.obj` 正则提取 `(libxplanemp|sim)/[\w/\[\]]+` 并去重。
- 绑定规则：`XPLMInstance` 实例 dataref 数组（`XPLMCreateInstance` 传入）与
  OBJ 的 `ANIM_*` 键**逐字符精确匹配**；`XPLMInstanceSetPosition` 的 values
  数组按该数组顺序逐一对应。
