# FSD 通讯协议参考（Legacy FSD，基于 swift-project/pilotclient）

> 本文档根据 [swift-project/pilotclient](https://github.com/swift-project/pilotclient)
> 源码（`src/core/fsd/`）整理，描述 **Legacy FSD**（非 VATSIM 认证流程）模式下
> 客户端与服务器的通讯协议，并给出与 XFlySim-Client 本地实现
> （`xfly_client/fsd/`）的对照。
>
> 官方协议文档参考：<https://fsd-doc.norrisng.ca/>（swift 源码中引用的 FSD Server 文档）

---

## 1. 概述

- FSD（Flight Simulator Data / Flying Simulation Data）是虚拟航网
  （VATSIM、IVAO、各私有网）使用的**文本行协议**，基于 TCP。
- swift 中由 `CFSDClient`（`src/core/fsd/fsdclient.{h,cpp}`）实现全部连接、
  解析、应答逻辑；每种报文一个 PDU 类（`src/core/fsd/*.{h,cpp}`）。
- 服务器分两类（`ServerType`）：`LegacyFsd`（openfsd 等）与 `Vatsim`。
  **Legacy 模式差异：无 `$DI`/`$ID` 认证握手，TCP 连上后直接发 `#AP` 登录。**

## 2. 传输层

| 项目 | 说明 |
|---|---|
| 传输 | TCP（`QTcpSocket`） |
| 默认端口 | **6809**（VATSIM 服务器文件中也默认 6809） |
| 分帧 | 每行以 `\r\n` 结尾（`messageToFSDString()` 固定追加） |
| 字段分隔 | 冒号 `:` |
| 文本编码 | swift 默认 **ISO-8859-1**（可按服务器 FSD 设置配置，`CFsdSetup::getTextCodec()`）；XFlySim-Client 使用 **GB18030**（兼容中文，openfsd 常见） |

报文序列化规则（`messagebase.h`）：

```cpp
messageToFSDString = pdu() + toTokens().join(':') + "\r\n"
```

即：`PDU前缀` + `:发送方:接收方[:字段...]`。

发送采用消息队列 + 10ms 定时器批量出队（一次最多连发 6 条，防洪峰）。

## 3. 报文类型总表

swift 的 `m_messageTypeMapping`（`initializeMessageTypes()`）：

| PDU | MessageType | 方向 | 说明 |
|---|---|---|---|
| `#AA` | AddAtc | C→S | ATC/OBS 登录 |
| `#AP` | AddPilot | C→S | 飞行员登录 |
| `%` | AtcDataUpdate | C/S→ | ATC 位置更新 |
| `$ZC` | AuthChallenge | S→C | 认证挑战（VATSIM） |
| `$ZR` | AuthResponse | C→S | 认证应答（VATSIM） |
| `$ID` | ClientIdentification | C→S | 客户端标识（VATSIM） |
| `$DI` | FsdIdentification | S→C | 服务器标识 + 初始挑战（VATSIM） |
| `$CQ` | ClientQuery | C→ | 客户端查询 |
| `$CR` | ClientResponse | →C | 查询应答 |
| `#DA` | DeleteATC | C/S→ | ATC 注销 |
| `#DP` | DeletePilot | C/S→ | 飞行员注销 |
| `$FP` | FlightPlan | C→S / S→C | 飞行计划 |
| `#PC` | ProController | ATC→ | 管制移交（swift 忽略） |
| `$!!` | KillRequest | S→C | 服务器踢人 |
| `@` | PilotDataUpdate | C/S→ | 位置更新（5s） |
| `^` | VisualPilotDataUpdate | C→ | 视觉位置（200ms） |
| `#SL` | VisualPilotDataPeriodic | C→ | 视觉位置周期包（每 25 包 1 次） |
| `#ST` | VisualPilotDataStopped | C→ | 停止移动包 |
| `$SF` | VisualPilotDataToggle | S→C | 视觉位置开关 |
| `$PI` | Ping | → | 保活 |
| `$PO` | Pong | → | 保活应答 |
| `$ER` | ServerError | S→C | 服务器错误 |
| `#DL` | ServerHeartbeat | S→C | 心跳（swift 忽略） |
| `#TM` | TextMessage | → | 文本/频率/广播消息 |
| `#SB` | PilotClientCom | → | 机型信息 / 中间位置等（子类型） |
| `$XX` | Rehost | S→C | 服务器迁移 |
| `#MU` | Mute | S→C | 静音请求 |
| `SIMDATA` | EuroscopeSimData | → | Euroscope 扩展数据（请求用 `$CQ` 发 `SIMDATA`） |

> 位置类前缀：`@`（普通位置）、`%`（ATC 位置）、`^`/`#SL`/`#ST`（视觉位置）。
> `#`/`$` 两族无严格语义差异，均为 PDU 前缀。

## 4. 登录与会话流程

### 4.1 Legacy FSD（XFlySim 使用的方式）

```
1. TCP connect(host, 6809)
2. handleSocketConnected → sendLogin()
3. 直接发送 #AP（无需任何握手）
4. 状态置为 Connected；启动位置定时器
```

swift 关键代码（`fsdclient.cpp`）：

```cpp
void CFSDClient::handleSocketConnected()
{
    if (m_protocolRevision == PROTOCOL_REVISION_CLASSIC)  // 9
    {
        this->sendLogin();
        this->updateConnectionStatus(CConnectionStatus::Connected);
    }
}
```

非 VATSIM 服务器 `m_protocolRevision = PROTOCOL_REVISION_CLASSIC (9)`。

### 4.2 VATSIM 认证流程（对照用）

```
S → $DI  FSDIdentification(serverVersion, initialChallenge)
C → $ID  ClientIdentification(clientId(hex), clientName, ver, cid, sysuid, challenge)
S → $ZC  AuthChallenge(challenge)
C → $ZR  AuthResponse(md5 应答)   ← 本地 auth.py 对应 $ZC→$ZR
```

协议版本常量（`fsdclient.h`）：

```cpp
constexpr int PROTOCOL_REVISION_CLASSIC = 9;        // Legacy / 经典
constexpr int PROTOCOL_REVISION_VATSIM_ATC = 10;    // VATSIM ATC
constexpr int PROTOCOL_REVISION_VATSIM_AUTH = 100;  // VATSIM 认证
constexpr int PROTOCOL_REVISION_VATSIM_VELOCITY = 101; // VATSIM 视觉位置
```

> 认证算法为 vatsim_auth 库（MD5 challenge/response）。
> XFlySim-Client 的 `fsd/auth.py` 是 xPilot C# `FsdAuthState` 的移植：
> `respond_to_challenge()` 收到 `$ZC` 挑战后计算应答并回 `$ZR`。

### 4.3 注销 / 踢人 / 迁移

| 场景 | 报文 |
|---|---|
| 飞行员正常断开 | `#DP<呼号>:<CID>`（发送后等 200ms 再关 socket） |
| ATC/OBS 断开 | `#DA<呼号>:<CID>` |
| 服务器踢人 | `$!!<sender>:<receiver>:<reason>` → 客户端断开 |
| 服务器迁移 | `$XX<sender>:<hostname>` → 客户端新建连接到新主机（同端口），成功后切换 socket |
| 静音 | `#MU<sender>:<receiver>:1|0` |

## 5. 位置上报

### 5.1 `@` PilotDataUpdate（每 5s，广播给范围内客户端）

```
@<应答机模式>:<呼号>:<应答机编码>:<PilotRating>:<纬度>:<经度>:<真高ft>:<地速kt>:<PBH>:<高度修正>
```

字段（`pilotdataupdate.cpp` `toTokens()` 顺序）：

| # | 字段 | 说明 |
|---|---|---|
| 0 | transponderMode | `S`=standby，`N`=Mode A/C/S 开启，`Y`=ident |
| 1 | callsign | 发送方呼号 |
| 2 | transponderCode | 应答机编码（squawk） |
| 3 | rating | PilotRating：0 未知 / 1 学员 / 2 VFR / 3 IFR / 4 教员 / 5 SUP |
| 4 | latitude | 度，5 位小数 |
| 5 | longitude | 度，5 位小数 |
| 6 | altitudeTrue | 真高 ft |
| 7 | groundSpeed | 地速 kt |
| 8 | pbh | 32 位 PBH 位域（见 5.3） |
| 9 | 修正 | **气压高 − 真高**（可为负），解析端：`pressureAlt = trueAlt + token[9]` |

> ⚠️ **字段 0 的 S/N 极容易写反，方向必须记牢**：`S` = **待机不发射**，
> `N` = **正常发射**（"N" 是 normal，不是 "no"）。swift 由两段代码决定：
>
> ① `simulatorxplane.cpp::xpdrMode(xpMode, ident)` 把 X-Plane 枚举折算：
> ```cpp
> if (ident) return StateIdent;                          // → Y
> if (xpMode == 0 || xpMode == 1) return StateStandby;   // → S
> return ModeC;                                          // → N（2..7 全部）
> ```
> ② `fsd/serializer.cpp::toQString(TransponderMode)` 再出字母：
> ```cpp
> case StateStandby:       return "S";
> case ModeA/C/S、Mil1..5:  return "N";
> case StateIdent:         return "Y";
> ```
>
> 所以 X-Plane 侧正确写法是 **`mode <= 1 ? "S" : "N"`**。
> XFlySim-Client 曾写成 `mode >= 2 ? "S" : "N"` —— **方向完全反了**，
> 8 个 XP 模式全部报错（待机被报成在发射、正常发射被报成待机），
> 对端与管制员看到的应答机状态永远是反的。已在 XConnect v24 修正，
> `selftest.py` 加断言锁住方向。
>
> 另注意别拿错函数：`CTransponder::modeAsShortString()` 返回 `S`/`C`/`I`，
> 那是 **GUI 显示**用的，**不是 FSD 报文**。

> XFlySim-Client 本地报文与之一致：
> `@模式:呼号:squawk:rating:lat:lon:alt:gs:pbh:corr`（`packet.py::pilot_position`）。
> 其中 `corr` 现按协议语义填 **气压高 − 真高**（取座舱高度表
> `altitude_indicated` 与 `altitude_msl` 之差）；此前误填"场压高"，对端据此
> 推算气压高度会得到荒谬值。

### 5.2 ATC 位置 `%` AtcDataUpdate

```
%<呼号>:<频率kHz偏移>:<设施类型>:<可视范围NM>:<AtcRating>:<纬度>:<经度>:<场压高ft>
```

- 频率字段 = 实际 kHz − 100000（`toTokens()` 中 `m_frequencykHz - 100000`）
- 设施类型：0 OBS / 1 FSS / 2 DEL / 3 GND / 4 TWR / 5 APP / 6 CTR
- ATC Rating：1 OBS … 5 C1 … 11 SUP / 12 ADM
- 观察员（OBS）以 `#AA` 登录后按固定频率 199998 kHz、range 300、OBS 上报

### 5.3 PBH 位域编码（`pbh.h`）

```
bit 0    : 未用
bit 1    : onGround
bits 2-11: heading（10 bit，无符号）
bits 12-21: bank（10 bit，有符号）
bits 22-31: pitch（10 bit，有符号）
```

编码公式（**FSD 中 pitch/bank 取反**，swift 与 vPilot 均用 ×-1 而非按位取反）：

```cpp
pitch = qFloor(pitch * -256.0/90.0);
bank  = qFloor(bank  * -512.0/180.0);
hdg   = heading * 1024.0/360.0;   // 取整
```

> **XFlySim-Client 已完全对齐上述公式**（`packet.py::encode_pbh`），曾有过的
> 三处偏差都已修正，记录在此避免回退：
>
> | 项 | 曾经的错误实现 | 正确做法（= swift） | 影响 |
> |---|---|---|---|
> | 航向量化 | `×1023/360` | **`×1024/360`** | 每个航向系统性少 1 个码位（45°/90°/180°/270° 各差 1）|
> | pitch/bank 符号 | 未反转（直接 `pitch×256/90`）| **`× -1` 反转** | 姿态垂直镜像 |
> | 取整方式 | `int()` 截断（朝 0）| **`floor()`**（qFloor）| **正俯仰/正坡度恒偏 1 个码位**（pitch 1° → -3 而非 -2，约 0.35° 误差）|
>
> 第三项最容易漏：`int(-28.444) = -28` 而 `floor(-28.444) = -29`。因为
> pitch/bank 乘了 -1，**所有正姿态的中间值都是负数**，用截断就必然偏 1。
> `selftest.py` 里已加三条断言锁住这些行为（`qFloor not truncation`）。
>
> 另外 swift 注释明确写了用 `*-1` 而不是按位取反 `~`：
> 「with `~` 0 -> -1 (that is why we saw many -1 PB values on ground)」。
> vPilot 同样用 `*-1`。我们用 0° 编码后验证过 pitch/bank 域都是 0，正确。
>
> 解码（`decode_pbh`）**刻意不照抄 swift 的 `unpackPBH`**：swift 那边
> `qFloor(raw / -mult)` 会把 pitch/bank 还原成**整度**（真实报文 pbh=-8389616
> 解出 1.0°，而真实姿态是 1.05°）。我们用它填远端玩家的 `tr.pitch/bank`
> 参与插值显示，需要连续精度，故乘回系数不取整。编码是发上线的字节必须
> 逐位一致，解码只是本地显示，精度优先。

### 5.4 中间位置 / 快速位置

| 报文 | 格式 | 频率 |
|---|---|---|
| `#SB` `VI`（swift 中间位置） | `#SB<呼号>:<receiver>:VI:<lat>:<lon>:<altTrue>:<gs>:<pbh>` | 1s |
| `#SB` `I`（SquawkBox 旧格式） | 同上但子类型 `I`（swift 忽略，精度差） | 1s |
| `^` VisualPilotDataUpdate | 见下 | 200ms |
| `#SL` VisualPilotDataPeriodic | 同 `^` 13 字段 | 每 25 个 `^` 发 1 次 |
| `#ST` VisualPilotDataStopped | `#ST<呼号>:<lat>:<lon>:<altTrue>:<altAgl>:<pbh>:<noseGear>` | 速度归零时 |

> XFlySim-Client 四种都解析为位置源（`parser.try_get_pilot_position`），用于
> 比 5s 的 `@` 更平滑的 CSL 插值；`pbh` bit1（onGround）与 `^` 的 `altAgl`
> 一并传给插件做落地吸附。

`^` 完整字段（`visualpilotdataupdate.cpp`）：

```
^<呼号>:<lat(7dp)>:<lon(7dp)>:<altTrue(2dp)>:<altAGL(2dp)>:<pbh>:
 <vx(4dp)>:<vy(4dp)>:<vz(4dp)>:<pitchRate(4dp)>:<headingRate(4dp)>:<bankRate(4dp)>:<noseGear(2dp)>
```

- 速度单位为 m/s，角速度为 rad/s；`pbh` 中 onGround 固定 0
- 开关控制：服务器发 `$SF<server>:<client>:1|0` 决定客户端是否发视觉位置
- VATSIM 的 `PROTOCOL_REVISION_VELOCITY (101)` 才启用视觉位置；Legacy 默认关闭

## 6. 文本消息 `#TM`

```
#TM<发送方>:<接收方>:<消息>
```

接收方三种形式：

| 接收方 | 含义 |
|---|---|
| 具体呼号 | 私聊 |
| `@<频率偏移>` | 频率消息（偏移 = kHz − 100000，如 118.800 MHz → `@18800`）；多频率用 `&` 连接：`@18800&19980` |
| `*` / `*A` / `*P` / `*S` | 全部 / 全部管制 / 全部飞行员 / 全部 SUP |

频率换算（与 XFlySim `packet.py::freq_to_target` 一致）：

```
目标字符串 = "@%05d" % (round(mHz * 1000) - 100000)
解析时      freq_kHz = int(token) + 100000
```

> 收到频率消息后 swift 会按 COM1/COM2 匹配，并先做 8.33kHz 信道间隔取整。
> Legacy 服务器上的 ATIS 回复也常以 `#TM` 私聊形式到达（swift 对已发出
> `$CQ:ATIS` 的管制有 5s 窗口的特殊处理）。

## 7. 客户端查询 `$CQ` / `$CR`

```
$CQ<发送方>:<目标>:<查询类型>[:数据...]
$CR<发送方>:<接收方>:<查询类型>[:应答数据...]
```

查询类型（`serializer.cpp`）：

| 类型码 | 含义 | 说明 |
|---|---|---|
| `ATC` | IsValidATC | 发给 SERVER，数据为目标呼号；应答 `Y`/`N` |
| `CAPS` | Capabilities | 应答为 `KEY=1` 键值对列表 |
| `C?` | Com1Freq | 应答为 COM1 频率 MHz（3 位小数） |
| `RN` | RealName | 应答：真实姓名 + 扇区文件(空) + 评级 |
| `SV` | Server | 应答服务器地址 |
| `ATIS` | ATIS | 仅 ATC 应答；行类型前缀 `V`(语音)/`T`(文本)/`Z`(Zulu 下线时间)/`E`(行数) |
| `IP` | PublicIP | 仅服务器应答 |
| `INF` | 特权信息 | SUP 用，以 `#TM` 回传 |
| `FP` | FlightPlan | 发给 SERVER，请求缓存计划；应答为 `$FP` |
| `ACC` | AircraftConfig | 携带 JSON 的机模配置（详见第 12 节） |
| `SIMDATA` | EuroscopeSimData | 发给 `@94835`，请求 Euroscope SIMDATA 流 |

能力协商（CAPS 应答键，`Capabilities`）：

| bit | 键 | 含义 |
|---|---|---|
| 1<<1 | `ATCINFO` | 可收 ATIS |
| 1<<2 | `SECPOS` | 次要位置 |
| 1<<3 | `MODELDESC` | 现代机型信息包（EQUIPMENT/AIRLINE/LIVERY） |
| 1<<4 | `ONGOINGCOORD` | 管制移交（ATC） |
| 1<<5 | `INTERIMPOS` | 中间位置（SquawkBox 旧格式） |
| 1<<6 | `FASTPOS` | 快速位置 |
| 1<<7 | `VISUPDATE` | 视觉位置 |
| 1<<8 | `STEALTH` | 隐身 |
| 1<<9 | `ACCONFIG` | 机模配置 |
| 1<<10 | `ICAOEQ` | 计划中机型字段用 ICAO 设备码 |

## 8. 机型信息 `#SB`（PilotClientCom）

| 子类型 | 报文 | 说明 |
|---|---|---|
| `PIR` | `#SB<呼号>:<目标>:PIR` | 请求机型信息 |
| `PI` | `#SB<呼号>:<目标>:PI:GEN:EQUIPMENT=<机型>[:AIRLINE=<航司>][:LIVERY=<尾号或涂装>][:REG=<尾号>][:ENGINES=<数量>]` | 应答（`GEN` 现代格式） |
| `FSIPIR` | `#SB<呼号>:<目标>:FSIPIR:0:<航司>:<机型>::::::<组合类型>:<model>` | FSInn 请求（12 字段） |
| `FSIPI` | `#SB<呼号>:<目标>:FSIPI:0:<航司>:<机型>::::::<组合类型>:<model>` | FSInn 应答 |

> **`PI:` 后面那个字段是"类型串"，只有 `GEN` 有效**。swift 源码
> `CFSDClient::handleCustomPilotPacket()` 的分支是：
> ```cpp
> else if (subType == "PI") {
>     if (tokens.size() > 6 && tokens.at(3) == "X") {
>         // This is the old version of a plane info request and no active
>         // client should ever send it.
>     }
>     else if (tokens.size() > 4 && tokens.at(3) == "GEN") {
>         ... PlaneInformation::fromTokens(tokens); emit planeInformationReceived(...);
>     }
> }
> ```
>
> | 类型串 | 含义 | swift 处理 |
> |---|---|---|
> | `GEN` | **当前唯一合法格式**（general） | 解析 EQUIPMENT/AIRLINE/LIVERY，正常处理 |
> | `X` | **旧版 PI 格式**（历史遗留，已废弃） | **静默丢弃**（空分支，只留注释）|
> | `H` | Swift 里**不存在**这个分支 | **静默丢弃**（`PI` 分支无 else 兜底）|
>
> ⚠️ 容易混淆的两点：
> 1. `H` 常被误认为 "HEAVY"（重型机标识）。**尾流类别不在这里表达** ——
>    swift 的 `CAircraftIcaoCode` 用独立的 wake turbulence 字段（如 `L2J`/`H`）
>    从机型数据库查得，PI 报文里只有 `EQUIPMENT=<ICAO机型码>`。
> 2. `A359` 是 **`EQUIPMENT` 的值**（A350-900 的 ICAO 机型码），不是类型串，
>    位置在 `PI:GEN:` 之后。同理 `B738`/`A333`。
>
> 所以正确写法只有：`#SB<呼号>:<目标>:PI:GEN:EQUIPMENT=<机型码>...`
> 发 `PI:H:...` 或 `PI:X:...` 对端会直接忽略，收不到任何机型信息。

FSInn 格式字段序号（swift 解析只取 4、5、10、11）：

```
#SB from:to:FSIPI:0:airline:aircraft:空:空:空:空:combinedType:modelString
               ^2  ^3    ^4      ^5        ^10           ^11
```

> XFlySim-Client 的 `session.py` 用 `#SB...:PI:GEN`（`send_plane_info`）与
> `#SB...:FSIPI:0:...`（`send_plane_info_fsi`）两种应答，与 swift 完全一致。
>
> 接收侧（`model.py::_on_plane_info_received`）优先级：
> **PI:GEN 的 `EQUIPMENT`/`AIRLINE`/`LIVERY` → FSIPI 的机型/航司/描述串尾号 →
> `$FP` 计划机型 → 呼号前三位当涂装码**。请求侧同时发 `PIR`（swift/xPilot）与
> `FSIPIR`（FSInn），每个目标 30s 节流；收到应答后不再重复请求。`EQUIPMENT`
> 会去掉 `/W` 之类尾流后缀再做 CSL 匹配。
>
> **`LIVERY` 放注册号（尾号），不放涂装码。** 原因：swift 收到 PI 后只保存
> `EQUIPMENT/AIRLINE/LIVERY` 三个已知键，`REG=` 这类未知键会被丢弃，再转发给
> 下一个请求者时尾号就丢了；把尾号塞进 `LIVERY` 才能穿透 swift 中继（
> `ours → swift → 第三方`）。没有尾号时退回原涂装码，报文与旧版一致。
>
> 接收侧判定规则（`_looks_like_registration`，与插件 `tail_like` 同规则）：
> `REG=` 存在则直接采信；否则 `LIVERY` 归一化后按形态判定——
> **含数字的**（`B-1234` / `N829PL` / `VH-OEI`）要求 ≥5 位且含字母或 `-`；
> **纯字母的**要求带 `-` 且字母 ≥4 个（欧陆格式 `D-AIKN` / `G-EUUU` / `F-WWBB`，
> 注册号里常常一个数字都没有——X-Plane 自带 A330 的尾号就是 `D-AIKN`）。
> 纯字母短码（`DLH`/`CCA`/`SJX`）、4 位机型码（`A320`/`B738`）、含 `_` 的 swift
> 内部映射 ID（`swift_a9742`）都当涂装码/直接丢弃。FSIPI 的末位描述串
> （`DLH A359 B-1234`）也会扫一遍尾号。
>
> ⚠️ 历史坑：早期规则写死"必须含数字"，导致 `D-AIKN` 这类欧陆尾号被整类丢弃，
> `LIVERY` 永不发出、服务器侧只能得到 `"livery": null`。XConnect v14 已修正。
>
> **`REG=<尾号>` 仍附带一份**（本客户端扩展键，swift 解析时忽略未知键，无害）：
> 让对端不必猜 `LIVERY` 的语义，也兼容只看 `REG=` 的旧版客户端。插件据此按
> "同一架飞机"选 CSL 涂装：注册号 → 同航司同机型 → 同机型 → 兜底（见
> `XConnect/PROTOCOL.md`）。swift / FSInn 原生不发尾号，对端为它们时自动从
> 第 2 级开始匹配。
>
> **`ENGINES=<数量>` 是 XFlySim 服务器识别的扩展键**（官方文档只列了
> `EQUIPMENT`/`AIRLINE`/`LIVERY` 三个，但站点 `/pilots` JSON 的
> `aircraft.engines` 字段对应它）。取值来自 X-Plane
> `sim/aircraft/engine/acf_num_engines`（A333=2、B744=4、C172=1）；
> 数量为 0/未知时整键省略——此时站点 JSON 表现为 `"engines": null`，
> 与旧版行为一致。swift 解析时忽略该键，无害。
>
> ⚠️ 历史坑：插件曾把尾号 dataref 写成 `acf_tailnumber`，而 X-Plane 实际叫
> **`acf_tailnum`**。写错名字时 `XPLMFindDataRef` 返回 NULL，尾号永远读到空串，
> 于是 `LIVERY` 从未随 PI 发出，服务器侧只能得到 `"livery": null`。已在
> XConnect v13 修正。

## 9. 飞行计划 `$FP`（17 字段）

```
$FP<呼号>:SERVER:<I|V|S|D>:<机型/设备码>:<巡航速度kt>:<起飞机场>:
 <计划起飞时间hhmm>:<实际起飞时间hhmm>:<巡航高度>:<目的地>:
 <航程时>:<航程分>:<燃油时>:<燃油分>:<备降场>:<备注>:<航路>
```

- 飞行类型：`I`=IFR、`V`=VFR、`S`=SVFR、`D`=DVFR
- 巡航高度可为 `FLxxx` 或 `xxxxxft`
- 报文内**禁止出现冒号**：swift 发送前会把 route/remarks 中的 `:` 移除

## 10. Ping / Pong 保活

```
$PI<呼号>:<目标>:<时间戳ms>   → 收到后必须回
$PO<呼号>:<目标>:<时间戳ms>
```

- 时间戳为 `QDateTime::currentMSecsSinceEpoch()`（毫秒）
- 收到 `$PI` 立即回 `$PO`（原样带时间戳），可计算 RTT
- 服务器心跳 `#DL` 客户端忽略
- XFlySim-Client 的 60s 保活走 `#TM...:SERVER:ping`（其服务器无 `$PI` 处理器）

## 11. 服务器错误 `$ER`

```
$ER<server>:<呼号>:<错误码>:<出错参数>:<描述>
```

错误码（`enums.h` `ServerErrorCode`，int 序列）：

| # | 码 | # | 码 |
|---|---|---|---|
| 0 | NoError | 9 | NoWeatherProfile |
| 1 | CallsignInUse | 10 | InvalidRevision |
| 2 | InvalidCallsign | 11 | RequestedLevelTooHigh |
| 3 | AlreadyRegistered | 12 | ServerFull |
| 4 | SyntaxError | 13 | CidSuspended |
| 5 | InvalidSrcCallsign | 14 | InvalidCtrl |
| 6 | InvalidCidPassword | 15 | RatingTooLow |
| 7 | NoSuchCallsign | 16 | InvalidClient |
| 8 | NoFlightPlan | 17 | AuthTimeout |

**致命错误**（收到即断开，`isFatalError()`）：
CallsignInUse、InvalidCallsign、AlreadyRegistered、InvalidCidPassword、
InvalidRevision、RequestedLevelTooHigh、ServerFull、CidSuspended、
RatingTooLow、InvalidClient、AuthTimeout。

> XFlySim-Client `session.py` 收到 `$ER` 时展示错误；仅当错误码属于上述**致命集合**
> 时断链，其余（含 `NoFlightPlan(8)`、`NoSuchCallsign(7)`）保持连接。飞机计划查询
> `$CQ...:SERVER:FP:<呼号>` 对无计划的目标会返回 `NoFlightPlan`，属正常应答。

## 12. 其他扩展报文

### 12.1 AircraftConfig（`$CQ` + `ACC`）

```
请求: $CQ<呼号>:<目标>:ACC:{"request":"full"}
应答: $CQ<呼号>:<目标>:ACC:{"config":{...}}（JSON，非 ASCII 字符转 \uXXXX）
```

- 增量配置发给伪呼号 `@94836`（swift 每 100ms 检查机模部件变化，受 5s 令牌桶限速）
- 解析端丢弃 `{ "request": "full" }` 请求包，其余按 `config` 对象处理

### 12.2 Euroscope SIMDATA

```
请求: $CQ<呼号>:@94835:SIMDATA:1
数据: SIMDATA:<空>:<呼号>:<model>:<livery>:<时间戳>:<lat>:<lon>:<alt>:
      <heading>:<bank>:<pitch>:<gs>:<onGround>:<gear%>:<thrust%>:0:0.0:<灯光标志>
```

- 共 18 字段；灯光标志为位标志（nav=0x008、beacon=0x002、landing=0x020、
  taxi=0x040、strobe=0x004、logo=0x200、recognition=0x080、cabin=0x400）

### 12.3 ATIS（Legacy 服务器）

- 客户端发 `$CQ<呼号>:<管制>:ATIS`
- 应答经 `$CR` 以 `V`/`T`/`Z`/`E` 行类型前缀分多条到达
- 部分服务器以 `#TM` 私聊逐行回复；swift 等待 5s 后组合，`^\d{0,4}z$` 行视为结束

## 13. 定时器与速率参数（`fsdclient.h`）

| 参数 | 值 | 说明 |
|---|---|---|
| `c_updatePositionIntervalMsec` | 5000 | 位置 `@` 5s |
| `c_updateInterimPositionIntervalMsec` | 1000 | 中间位置 `#SB VI` 1s |
| `c_updateVisualPositionIntervalMsec` | 200 | 视觉位置 200ms |
| `c_sendFsdMsgIntervalMsec` | 10 | 发送队列处理周期 |
| `c_processingIntervalMsec` | 100 | 机模配置检查 |
| `PendingConnectionTimeoutMs` | 7500 | 登录挂起超时 |
| 单次最多读取 | 74 行 | 超过则延时 10ms 继续 |
| 位置时间偏移 | 6000ms / 1500ms | 收到 `@`/中间位置的预估时间戳 |

## 14. 编码表汇总

### 14.1 SimType（`serializer.cpp`，Legacy）

| 值 | 模拟器 | 值 | 模拟器 |
|---|---|---|---|
| 0 | 未知 | 12 | X-Plane 8 |
| 1 | MSFS95 | 13 | X-Plane 9 |
| 2 | MSFS98 | 14 | X-Plane 10 |
| 3 | MSCFS | 16 | X-Plane 11 |
| 4 | MSFS2000 | 25 | FlightGear |
| 5 | MSCFS2 | 30 | P3D v1–v4 |
| 6 | MSFS2002 | 0 | XP12 / P3Dv5（尚未定义标准值，swift 暂回 0） |
| 7 | MSCFS3 | | |
| 8 | MSFS2004 | | |
| 9 | MSFSX | | |
| 10 | MSFS 2020 | | |
| 11 | MSFS 2024 | | |

> XFlySim-Client 使用 **15=XP11、16=XP12**（xPilot C# 习惯）。
>
> ⚠️ **同一个编号在 swift 里含义不同**（`fsd/serializer.cpp::toQString(SimType)`）：
>
> | 编号 | swift | XFlySim-Client |
> |---|---|---|
> | 14 | XP10 | — |
> | 15 | — | **XP11** |
> | 16 | **XP11** | **XP12** |
> | `"0"` | XP12 / P3Dv5（swift 未分配编号）| — |
>
> 即 swift 至今没给 XP12 单独编号（与 P3Dv5 一起返回 `"0"`）。
> 接第三方服务器前必须先确认该服务器采用哪套约定 —— 用错会让服务器
> 按错误的模拟器类型处理你的连接。

### 14.2 评级 / 设施 / 应答机 / 飞行规则

| 类型 | 取值 |
|---|---|
| PilotRating | 0 未知 / 1 学员 / 2 VFR / 3 IFR / 4 教员 / 5 SUP |
| AtcRating | 0 未知 / 1 OBS / 2 S1 / 3 S2 / 4 S3 / 5 C1 / 6 C2 / 7 C3 / 8 I1 / 9 I2 / 10 I3 / 11 SUP / 12 ADM |
| Facility | 0 OBS / 1 FSS / 2 DEL / 3 GND / 4 TWR / 5 APP / 6 CTR |
| Transponder | `S`=standby、`N`=Mode A/C/S 开启、`Y`=ident |
| FlightType | `I` / `V` / `S` / `D` |

## 15. 与本地 XFlySim-Client 实现对照

| pilotclient（swift） | XFlySim-Client | 说明 |
|---|---|---|
| `fsdclient.cpp` 连接/解析 | `xfly_client/fsd/session.py` | 会话、登录、事件分发 |
| 各 PDU 类 `toTokens()` | `xfly_client/fsd/packet.py` | 报文构造 |
| `parseMessage()` / PDU 类 `fromTokens()` | `xfly_client/fsd/parser.py` | 报文解析 |
| `QTcpSocket` + 编码器 | `xfly_client/fsd/link.py` | GB18030 增量解码、CRLF 分帧 |
| `vatsim_auth` 库 | `xfly_client/fsd/auth.py` | `$ZC`→`$ZR` challenge/response |
| Legacy 登录 `#AP`（rev 9） | `packet.add_pilot(...)` | 一致 |
| 位置 `@`（5s） | `packet.pilot_position(...)` | 一致 |
| 中间位置 `#SB VI`（1s） | 未实现（只读不报） | 私有网一般不启用 |
| 视觉位置 `^`/`#SL`/`#ST`/`$SF` | 解析支持、不发送 | XFlySim 服务器未见启用 |
| `#TM` 私聊/频率 | `send_text` / `send_frequency_text` | 一致（`@%05d` 频率偏移） |
| `$CQ`/`$CR` | 解析支持；**`ACC` 机模配置已实现**（`packet.aircraft_config` + `session.notify_aircraft_parts`） | 增量/全量/`@94836` 广播 |
| `$FP` 飞行计划 | `send_flight_plan`/webapp 自动补交 | 一致 |
| `$ER` 错误处理 | `session._on_line` 断链 | 行为近似 |

## 16. 参考资料

- pilotclient 源码：<https://github.com/swift-project/pilotclient>（`src/core/fsd/`）
- FSD 协议文档：<https://fsd-doc.norrisng.ca/>
- swift 开发者 wiki：<https://github.com/swift-project/pilotclient/wiki>
- 本项目实现：`xfly_client/fsd/`（packet.py / parser.py / auth.py / link.py / session.py）
