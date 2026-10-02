# XConnect 协议（TCP / JSON 行）

- 传输：TCP，端口 **51001**，UTF-8，每条消息一行（以 `\n` 结尾）
- 角色：**插件 = TCP 服务端**（监听），客户端主动连接
- 兼容 ISFP-xLink 协议风格（字段 snake_case）

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
 "com1_freq":122800,"com2_freq":118050,"transponder":1234,
 "squark":"S","gear_deploy":0.0,"flaps_ratio":0.0,"throttle_ratio":0.3}
```

- 频率字段单位：`MHz*1000`（`122800` = 122.800 MHz）
- 高度/地速单位：ft / kt
- `squark`：`"S"` 应答机开启、`"N"` 应答机关闭

## 客户端 → 插件

### Send:OPD:FSD9（在线玩家列表 → CSL 渲染）

```json
{"type":"Send:OPD:FSD9","mycid":"1001",
 "data":[{"callsign":"CCA999","latitude":23.1,"longitude":113.0,
          "altitude":3000,"pitch":1.0,"bank":-5.0,"heading":90.0,
          "ground_speed":250,"aircraft":"B738","aircraft_family":""}]}
```

- `callsign`：玩家呼号（主键）
- `latitude/longitude`：WGS84 度
- `altitude`：真高 ft（MSL）
- `pitch/bank/heading`：度
- `ground_speed`：kt
- `aircraft`：机型 ICAO，用于查找 `CSL/{ICAO}.obj`
- `aircraft_family`：CSL 包名（可选，优先使用 `CSL/{family}/{icao}.obj`）
- 未在列表中或超过 30s 未更新的玩家，插件移除其模型

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