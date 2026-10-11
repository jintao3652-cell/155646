# XConnect 协议（TCP / JSON 行）

- 传输：TCP，端口 **51001**，UTF-8，每条消息一行（以 `\n` 结尾）
- 角色：**插件 = TCP 服务端**（监听），客户端主动连接

## 插件 → 客户端

### connected（连接建立，插件就绪）

```json
{"type":"connected","version":1}
```

### flight_data（本机飞行数据，5Hz）

```json
{"type":"flight_data","latitude":23.1300,"longitude":113.2000,
 "altitude_msl":3010.0,"altitude_agl":2510.0,"elevation":10.0,
 "pitch":1.5,"bank":-2.0,"heading":92.0,"true_heading":92.0,
 "groundspeed":248.0,"indicated_airspeed":250.0,"vertical_speed":0.0,
 "com1_freq":122800,"com2_freq":118050,"com3_freq":0,
 "com1_standby":121500,"com2_standby":124850,"com3_standby":0,
 "transponder":1234,"xpdr_mode":3,"squark":"S",
 "tcas_filter":0,"tcas_alert":0,"tcas_advisory":0,
 "gear_deploy":0.0,"flaps_ratio":0.0,"throttle_ratio":0.3}
```

- 频率字段单位：`MHz*1000`（`122800` = 122.800 MHz）
- 高度/地速单位：ft / kt
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
          "airline":"CCA"}]}
```

- `callsign`：玩家呼号（主键）
- `latitude/longitude`：WGS84 度
- `altitude`：真高 ft（MSL）
- `pitch/bank/heading`：度
- `ground_speed`：kt
- `aircraft`：机型 ICAO，用于查找 `CSL/{ICAO}/xsb_aircraft.txt`
- `airline`：涂装/航司码（呼号前三位大写，或用对方 `#SB PI` 上报的 `AIRLINE`），
  对应 xsb 中的 `AIRLINE`/`LIVERY` 变体码，选 `CSL/{ICAO}/{ICAO}_{airline}.obj`
- `aircraft_family`：CSL 包名（可选回退，用 `CSL/{family}/{icao}.obj`）
- 未在列表中或超过 30s 未更新的玩家，插件移除其模型

> **复合模型与地面钳制**：X-CSL 的一架飞机由多个 OBJ 组成（机身涂装 + 发动机/
> 风扇等），插件解析 `CSL/{ICAO}/xsb_aircraft.txt` 的 `OBJ8_AIRCRAFT` 块逐个实例化，
> 并按 `AIRLINE`/`LIVERY` 行选中涂装变体（无匹配时回退 `ANY`/首个变体）。渲染时
> 对近地目标用 `XPLMProbeTerrainXYZ` 把模型落到地形高度（等价 XPMP2
> `clampToGround`），避免整机悬空、轮子离地。

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

### Send:EFB:Weather / Send:EFB:Route（预留，插件暂不处理）

## 会话管理

- 客户端断开：插件清空全部 CSL 实例与管制数据
- 同一时刻仅接受一个客户端连接（新连接替换旧连接）
- 插件不重连（非阻塞；X-Plane 重启后客户端需重连）