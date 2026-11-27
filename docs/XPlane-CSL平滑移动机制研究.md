# X-Plane CSL 映射与 5 秒位置更新的平滑移动机制研究

> 研究对象：[swift-project/pilotclient](https://github.com/swift-project/pilotclient)（Swift Pilot Client）
> 以及其 X-Plane 插件子模块 [swift-project/xplanemp2](https://github.com/swift-project/xplanemp2)（XPMP/libxplanemp）。
>
> 本文档回答一个问题：**网络端以约 5 秒为周期上报位置时，X-Plane 里的 CSL（Common Shared Library）模型是如何做到平滑移动而不是一跳一跳的。**

---

## 1. 结论（TL;DR）

Swift 采用 **“客户端插值 + 插件端轻度补帧 + 渲染端每帧直采”** 的四层结构：

1. **FSD 协议层**：正常位置包每 5 秒一个；每个收到的位置会打上一个**时间偏移**（默认 6000 ms，快速更新时 1500 ms），把样本“推移到未来”，插值窗口因此永远落在 `[上一个样本, 下一个样本]` 内，避免外推导致的跳变。
2. **客户端插值引擎**（核心平滑逻辑）：对每架远程飞机维护最多 50 个位置快照，默认用**三次样条（Spline）**在 ECEF 归一化向量空间插值经纬度，线插值俯仰/坡度/航向（带 ±180° 回绕），高度与接地系数同样样条化。
3. **xswiftbus 插件**：收到客户端插值结果后，再保留最近 2 个位置的 1 秒历史，逐帧做一次**简单的线性外插**，只用于抹平 D-Bus 传输抖动（超过 3 秒就放弃）。
4. **XPMP/xplanemp2 渲染端**：`XPMPPlane` 直接把“当前插值位置”写到 OBJ8 实例（`XPLMInstanceSetPosition`），每帧位置都是连续值，所以视觉上就是平滑移动。

CSL 模型映射本身与平滑无关，它是 `xsb_aircraft.txt` → 模型匹配（ICAO/航司/涂装）→ 实例化 OBJ8 的过程，本文第 7 节一并说明。

---

## 2. 整体数据链路

```
远程飞行员 ──FSD(5s 位置包 / 1s interim / 200ms visual)──▶ Swift 客户端
                                                               │  CFSDClient 解析 + 时间偏移
                                                               ▼
                                               CInterpolatorMulti (Spline/Linear)
                                                               │  每个 X-Plane 帧(simFrame)触发
                                                               ▼
                                        CSimulatorXPlane::updateRemoteAircraft()
                                                               │  D-Bus (xswiftbus)
                                                               ▼
                                              xswiftbus 插件 CTraffic::setPlanesPositions
                                                               │  逐帧线性外插 (interpolatePosition)
                                                               ▼
                                    XPMPUpdatePlanes → XPMPPlane::updatePosition
                                                               │  每渲染帧 doInstanceUpdate
                                                               ▼
                        Obj8CSL::updateInstance → XPLMInstanceSetPosition → OBJ8 实例
```

---

## 3. 第一层：FSD 位置更新频率与“时间偏移”机制

### 3.1 网络端上报频率

`src/core/fsd/fsdclient.h` 定义了三个发送定时器：

| 定时器 | 周期 | 用途 |
|---|---|---|
| `c_updatePositionIntervalMsec` | **5000 ms** | 常规位置更新（PDU，即“5 秒更新”） |
| `c_updateInterimPositionIntervalMsec` | 1000 ms | 快速 interim 位置（需服务器支持 FastPos） |
| `c_updateVisualPositionIntervalMsec` | 200 ms | VATSIM 视觉位置（带 6DOF 速度） |

即：**常规网络更新就是每 5 秒一个位置包**，这是平滑问题的根源。

### 3.2 收到位置后打“未来偏移”（关键机制）

`src/core/fsd/fsdclient.cpp` 的 `receivedPositionFixTsAndGetOffsetTime()`（约 1896–1917 行）：

- 每架飞机维护一个 `m_lastPositionUpdate` 时间戳表；
- 首包直接返回默认偏移；
- 统计最近若干包的到达间隔平均值 `avgTimeMs`：
  - 平均间隔 < `c_interimPositionTimeOffsetMsec`（即对方在发 1s 快速包）→ 偏移 = **1500 ms**；
  - 否则 → 偏移 = **6000 ms**。

偏移常量定义在 `src/misc/network/fsdsetup.h`：

```cpp
//! Offset times basically telling when to expect the next value from network plus some reserve
static constexpr qint64 c_positionTimeOffsetMsec = 6000;      // 正常 5s 包：+6s
static constexpr qint64 c_interimPositionTimeOffsetMsec = 1500; // 快速 1s 包：+1.5s
```

随后 `situation.setTimeOffsetMs(offsetTimeMs)`。插值使用的时间是 **“调整后时间”**：

```cpp
// src/misc/timestampbased.h
qint64 getAdjustedMSecsSinceEpoch() const { return this->getMSecsSinceEpoch() + this->getTimeOffsetMs(); }
```

**为什么这样能平滑？**
假设位置 A 在 `t=0` 收到、位置 B 在 `t=5s` 收到，两者都加 6 s 偏移后，插值区间变成
`[A+6s, B+6s]`，跨度恰为 5 s。只要渲染时使用的“当前时间”始终落在这个区间内（而不是区间之前），插值就永远是**真正的插值**而不是外推。代价是画面里的飞机相对网络实时状态滞后约数秒，但换来的是全程连续轨迹，5 秒一跳的问题被彻底抹平。

### 3.3 快照保留

- `src/misc/simulation/remoteaircraftprovider.h`：每架远程飞机最多保留 `MaxSituationsPerCallsign = 50` 个位置快照。
- 插入时按调整后时间保持“最新在前”排序：`push_frontKeepLatestAdjustedFirst(...)`（`src/misc/timestampobjectlist.h`）。

---

## 4. 第二层：客户端插值引擎（平滑的核心）

### 4.1 总入口

- `CXPlaneMPAircraft`（`src/plugins/simulator/xplane/xplanempaircraft.cpp`）持有一个 `CInterpolatorMulti`；
- 每个 X-Plane 帧，xswiftbus 通过 D-Bus 发 `simFrame` 信号，`CSimulatorXPlane::updateRemoteAircraft()`（`simulatorxplane.cpp` 896–996 行）对每架飞机调用 `getInterpolation(now, setup, n)`，得到插值后的 `CAircraftSituation`，再推给插件（`m_trafficProxy->setPlanesPositions(...)`）。
- 插值模式（`CInterpolationAndRenderingSetupBase::InterpolatorMode`）默认 **Spline**，可在设置中切换为 Linear。

### 4.2 Spline 模式（默认）

`src/misc/simulation/interpolation/interpolatorspline.cpp`

- **取 3 个样本**：`m_s[0]`（较旧）、`m_s[1]`（上一插值结果）、`m_s[2]`（最新收到的网络位置）；
- 对经纬度在**归一化向量空间（normal vector / ECEF 风格）**的 x/y/z 三个分量分别构造**自然三次样条**：用三对角矩阵算法（Thomas algorithm）求各点一阶导数，再在 `[t1, t2]` 区间上做 Hermite 型三次求值（`evalSplineInterval`，82–95 行）；
- **高度**同样样条化（`getCorrectedAltitude`，避免插到地面以下）；
- **接地系数 groundFactor** 也样条化；如果首尾都判定为“空中”或“地面”，直接取同值（`interpolateGroundFactor`，368–392 行）；
- 导数只在“有新位置”或“越过下一采样点”时才重算（`recalculate`，174–232 行），性能友好；
- 时间分数 `timeFraction = (now - prevAdjusted) / (nextAdjusted - prevAdjusted)`，并 `clampValidTimeFraction` 到 [0,1]——配合 3.2 节的时间偏移，正常情况下不会越界。

### 4.3 Linear 模式

`src/misc/simulation/interpolation/interpolatorlinear.cpp`

- 以“当前时间”为分界线，把快照列表分成 `newer` 与 `older` 两组；
- 取分界前最新的旧样本和分界后最老的新样本做线性插值；
- 若没有“未来的样本”，就用两个历史样本做**匀速外推**（保持轨迹延续）；若超过 2 个采样间隔仍无新数据，则停在最后位置；
- `simulationTimeFraction` 计算方式见 193–212 行，同样 clamp 到 [0,1]。

### 4.4 姿态角 PBH 插值

`src/misc/simulation/interpolation/interpolatorlinearpbh.cpp`

- 航向/俯仰/坡度全部用线性插值，但对角度差做 **±180° 回绕归一化**，避免 359°→1° 之类的反向大转弯：
  ```cpp
  double deltaDeg = (end - begin).value(CAngleUnit::deg());
  if (deltaDeg > 180.0) { deltaDeg -= 360; }
  else if (deltaDeg < -180.0) { deltaDeg += 360; }
  ```
- 地速也线性插值；fraction 严格限制在 [0,1]，不做角度外推。

### 4.5 其他平滑相关细节

- **地面海拔**：插值前预先从海拔缓存/两样本间插值出地面海拔（`presetGroundElevation`，`interpolator.cpp` 109–157 行）；插值后的位置再补地面海拔、校正高度（`interpolateElevation` / `correctAltitude`，见 `getInterpolatedSituation`，191–370 行）。
- **GND 标志**：不能插值时用 `CAircraftSituationChange::null().guessOnGround(...)` 猜测；可插值时插 groundFactor。
- **部件（parts）**：起落架/襟翼等不需要逐帧插值，采用“慢更新”策略（每 25 帧、按飞机编号取模抽帧），`getInterpolatedOrGuessedParts`（405–450 行）。

---

## 5. 第三层：xswiftbus 插件内的二次平滑

文件：`src/xswiftbus/traffic.cpp`

- **每帧驱动**：`CTraffic::process()` 每 X-Plane 帧调用 `doPlaneUpdates()`，并且 `emitSimFrame()` **隔帧才发一次** D-Bus `simFrame` 信号（149–153 行），减轻客户端-插件通信压力。
- **位置历史缓冲**：`Plane` 结构里有 `positions[0..2]` 和 `positionTimes[0..2]`。`setPlanesPositions()`（374–408 行）把新位置写入 `positions[2]`，若与上一次写入间隔超过 1 秒，则把历史向后滚动保存（`[0]=旧[1]`, `[1]=旧[2]`），**刻意只保留 1 秒粒度的近邻历史**。
- **线性外插补帧**：`interpolatePosition()`（831–850 行）每帧把 `positions[3]`（交给 XPMP 的实际位置）设为：
  ```cpp
  positions[3].lat = positions[0].lat + dLat * t2 / t1;  // t1=最近两次更新间隔, t2=距旧点时间
  ```
  即基于最近两次收到的位置做匀速外插。注释明确写着“只用于抹平小误差，误差过大（>3s）就放弃”，因为 5 秒级的大间隙已经由客户端 Spline 解决。
- **起落架动画**：`interpolateGear()`（852–869 行）让起落架在 **5 秒**内匀速收放（`gearMoveTimeMs = 5000`），收放过程本身也是平滑的。
- **提交渲染**：`doPlaneUpdates()` 组装 `XPMPUpdate_t` 数组，一次 `XPMPUpdatePlanes()` 批量更新所有飞机，然后手动调用 `XPMP_PrepListHook` 触发本帧预渲染列表。

---

## 6. 第四层：XPMP / xplanemp2 渲染端

子模块仓库 `swift-project/xplanemp2`（原 libxplanemp）。

- **位置写入**：`src/XPMPPlane.cpp`
  - `updatePosition()`（78–82 行）只是 `memcpy` 覆盖最新位置，**没有插值**——平滑已经由上层完成；
  - `doInstanceUpdate()`（101–169 行）每渲染帧执行：`XPLMWorldToLocal(lat, lon, elevation*ft→m)` 得到本地坐标，连同 heading/pitch/roll、`clampToGround`、`offsetScale` 交给 `mCSL->updateInstance(...)`。
- **实例矩阵**：`src/obj8/Obj8InstanceData.cpp::updateInstance()`（33–123 行）填充 `XPLMDrawInfo_t`（x/y/z + heading/pitch/roll），并把襟翼/舵面/灯光/发动机转速等写入 `XPLMInstanceSetPosition` 的 dataref 值数组。OBJ8 动画（如螺旋桨旋转）由 X-Plane 用这些自定义 dataref（`libxplanemp/...`）驱动。
- **CSL 基类**：`src/CSL.h` 中 `updateInstance` 声明包含 `bool clampToSurface` 与 `float offsetScale`，用于地面钳制与缩放偏移；xswiftbus 固定传 `offsetScale=1.0`、`clampToGround=true`（`traffic.cpp` 393–394 行）。
- **渲染回调**：`src/Renderer.cpp` 的 `Render_PrepLists()`（每帧、按 cycle 去重）遍历全部 `XPMPPlane` 调 `doInstanceUpdate`。

> 一句话：XPMP 是“给什么画什么”，它负责的是把连续位置高效地实例化到 OBJ8，而非生成连续位置。

---

## 7. CSL 模型映射（model mapping）

模型映射决定“这个呼号应该显示哪套 OBJ8 模型”，与位置平滑相互独立。

### 7.1 客户端侧：解析 CSL 包并选中模型

`src/plugins/simulator/xplane/simulatorxplane.cpp`

- `loadCslPackages()`（669–745 行）：扫描 X-Plane 目录下的 `xsb_aircraft.txt`（每包一个文件，BlueBell 有 9 个、X-CSL 有 76 个），逐个交给插件加载（`m_trafficProxy->loadPlanesPackage`），报告加载失败/目录外等错误。
- 模型清单来源：`src/misc/simulation/xplane/aircraftmodelloaderxplane.cpp`
  - `parseCslPackages()`（238 行起）解析 `xsb_aircraft.txt` 的 `OBJECT`/`EXPORT`/`DEPENDS` 等命令，生成 CSL 包与机型（OBJ7/OBJ8）列表；
  - 模型字符串形如 `__XPFW_Jets:A320_a:A320_a_Austrian_Airlines.obj`（49 行注释），区分 CSL 设计符。
- 每架远程飞机根据网络 ICAO/航司/涂装做模型匹配（`CAircraftModelList` / `CSL` 匹配），得到 `modelString`。

### 7.2 插件侧：加载 CSL 包与创建飞机

`src/xswiftbus/traffic.cpp`

- `loadPlanesPackage()`（243–261 行）：`XPMPMultiplayerLoadCSLPackages(path)` 加载整包，并把全部已装模型的大写名登记到 `m_modelStrings`，供客户端传来的模型名做精确匹配。
- `addPlane()`（292–327 行）：
  - 若客户端给的模型名在已装列表中 → `XPMPCreatePlaneWithModelName(...)`（精确匹配）；
  - 否则回退到 `XPMPCreatePlane(icao, airline, livery)`，由 xplanemp2 内置的 `CSL_MatchPlane`（`src/CSLLibrary.cpp`）按 ICAO→航司→涂装的质量打分（`mMatchQuality`）选择最佳模型。
- 默认机型：`setDefaultIcao()` → `XPMPSetDefaultPlaneICAO`。

### 7.3 渲染端：模型匹配质量与 LOD

`src/xplanemp2`（`XPMPPlane.cpp`、`CSLLibrary.cpp`）

- `setCSL()` / `upgradeCSL()`：只在更高匹配质量出现时才换模型，避免频繁换皮；
- `Obj8InstanceData::updateInstance()`：按距离切换 **Solid / LowLevelOfDetail / LightsOnly** 三档实例类型（51–60 行），远距离只画灯光。

### 7.4 X-CSL 特例

- `simulatorxplane.cpp` `fixSimulatorCg()`（1093–1103 行）：对 **X-CSL（XCSL）** 发行包，模型 CG（重心）若为空强制置 0，否则 DB 中的 CG 值会覆盖掉 XCSL 原本“无偏移”的语义；
- 发行商识别：`CDistributor::xplaneXcsl()`（`src/misc/simulation/distributor.cpp`）。

---

## 8. 关键代码位置索引

| 环节 | 文件 | 关键点 |
|---|---|---|
| 5s/1s/200ms 发送定时器 | `src/core/fsd/fsdclient.h` | 692–697 行 |
| 位置到达 + 时间偏移 | `src/core/fsd/fsdclient.cpp` | 1216–1219、1257–1260、1896–1917 |
| 偏移常量 | `src/misc/network/fsdsetup.h` | 62–64 行 |
| 调整后时间 | `src/misc/timestampbased.h` | 205 行 |
| 快照上限 50 | `src/misc/simulation/remoteaircraftprovider.h` | 47 行 |
| 插值总入口 | `src/misc/simulation/interpolation/interpolatormulti.cpp` | 18–32 行 |
| Spline 插值 | `src/misc/simulation/interpolation/interpolatorspline.cpp` | 98–392 行 |
| Linear 插值 | `src/misc/simulation/interpolation/interpolatorlinear.cpp` | 52–241 行 |
| 姿态角 PBH 插值 | `src/misc/simulation/interpolation/interpolatorlinearpbh.cpp` | 28–90 行 |
| 帧驱动插值并推送到插件 | `src/plugins/simulator/xplane/simulatorxplane.cpp` | 896–996 行 |
| 插件二次线性外插 | `src/xswiftbus/traffic.cpp` | 831–850 行 |
| 插件位置历史缓冲 | `src/xswiftbus/traffic.cpp` | 374–408 行 |
| 插件逐帧提交 XPMP | `src/xswiftbus/traffic.cpp` | 814–829 行 |
| XPMP 位置写入/渲染 | `xplanemp2/src/XPMPPlane.cpp` | 78–82、101–169 行 |
| OBJ8 实例矩阵 | `xplanemp2/src/obj8/Obj8InstanceData.cpp` | 33–123 行 |
| 渲染回调 | `xplanemp2/src/Renderer.cpp` | 76–107 行 |
| CSL 模型匹配 | `xplanemp2/src/CSLLibrary.cpp`、`src/CSL.h` | — |
| CSL 包解析 | `src/misc/simulation/xplane/aircraftmodelloaderxplane.cpp` | 238–330 行 |
| XCSL CG 特例 | `src/plugins/simulator/xplane/simulatorxplane.cpp` | 1093–1103 行 |

---

## 9. 补充说明

1. **为什么不用单一插值层？** 客户端 Spline 负责大间隔（5 s）的轨迹重建；插件线性外插只负责抹平 D-Bus 帧间抖动；渲染端不做插值，保持简单与高性能。三层各司其职。
2. **时间偏移的代价**：远程飞机画面会滞后网络状态约 1–6 秒（取决于对方发送模式），这是可接受且常见的设计（FSInn/VATSIM 同思路）。
3. **平滑的极限**：当位置包真的停止（>3 s 插件层放弃、客户端长时间无新样本）时，Linear 模式会做短暂匀速外推后停住，Spline 会停在最后插值位置，不会出现“瞬移”。
4. **与 CSL 模型映射的关系**：映射决定“画什么模型”，平滑链路决定“模型出现在哪里、姿态如何过渡”，两者通过 `XPMPPlanePosition_t`（含 heading/pitch/roll、clampToGround、offsetScale）与 `XPMPPlaneSurfaces_t` 解耦。
