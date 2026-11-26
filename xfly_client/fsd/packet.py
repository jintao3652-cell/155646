"""FSD 报文构造（以 swift pilotclient 的 Legacy FSD 协议为准）。

参考 xfly_client/fsd/FSD_PROTOCOL.md：
- PBH 编码：pitch/bank 有符号并取反（×-1），heading 1024 量化
- 位置 @ 经纬度 5 位小数
- 保活：服务器无 $PI 处理器时沿用 #TM...:SERVER:ping（本地约定）
"""

from __future__ import annotations

import math

NETWORK_RATING = 1
PROTOCOL_REVISION = 9
# 模拟器类型：**按 XFlySim/ISFP 私有服务器约定**（沿用 xPilot C# 习惯）
#   15 = XP11、16 = XP12
#
# ⚠️ 与 swift **不同**，不要拿 swift 的编号来对：
#   swift `fsd/serializer.cpp::toQString(SimType)`：
#       14 = XPLANE10, 16 = **XPLANE11**, 12..13 = XP8/XP9
#       XPLANE12 与 P3Dv5 一起返回 "0"（swift 至今没给 XP12 分配编号）
#   即同一个 16 在 swift 里是 XP11、在我们这里是 XP12。接第三方服务器前
#   必须先确认该服务器的约定（xPilot/SquawkBox 系多用 15/16 表 XP11/XP12）。
SIM_TYPE_XP11 = 15
SIM_TYPE_XP12 = 16

CLIENT_ID = 55538          # xPilot-assigned id（非官方，参照 C# 版）
AUTH_KEY = "ImuL1WbbhVuD8d3MuKpWn2rrLZRa9iVP"

SERVER_CALLSIGN = "SERVER"

# 服务器错误码（enums.h ServerErrorCode，见 FSD_PROTOCOL.md §11）
NO_ERROR = 0
ERR_CALLSIGN_IN_USE = 1
ERR_INVALID_CALLSIGN = 2
ERR_ALREADY_REGISTERED = 3
ERR_SYNTAX = 4
ERR_INVALID_SRC_CALLSIGN = 5
ERR_INVALID_CID_PASSWORD = 6
ERR_NO_SUCH_CALLSIGN = 7
ERR_NO_FLIGHT_PLAN = 8         # 查询无计划的呼号时返回（非致命）
ERR_NO_WEATHER_PROFILE = 9
ERR_INVALID_REVISION = 10
ERR_REQUESTED_LEVEL_TOO_HIGH = 11
ERR_SERVER_FULL = 12
ERR_CID_SUSPENDED = 13
ERR_INVALID_CTRL = 14
ERR_RATING_TOO_LOW = 15
ERR_INVALID_CLIENT = 16
ERR_AUTH_TIMEOUT = 17

# 致命错误（收到即断开，swift isFatalError()）。其余（含 NoFlightPlan）不断线。
FATAL_ERROR_CODES = frozenset({
    ERR_CALLSIGN_IN_USE, ERR_INVALID_CALLSIGN, ERR_ALREADY_REGISTERED,
    ERR_INVALID_CID_PASSWORD, ERR_INVALID_REVISION,
    ERR_REQUESTED_LEVEL_TOO_HIGH, ERR_SERVER_FULL, ERR_CID_SUSPENDED,
    ERR_RATING_TOO_LOW, ERR_INVALID_CLIENT, ERR_AUTH_TIMEOUT,
})

# 错误码名称（部分服务器以名字而非数字下发时兜底解析）
ERROR_CODE_NAMES = {
    "NoError": NO_ERROR,
    "CallsignInUse": ERR_CALLSIGN_IN_USE,
    "InvalidCallsign": ERR_INVALID_CALLSIGN,
    "AlreadyRegistered": ERR_ALREADY_REGISTERED,
    "SyntaxError": ERR_SYNTAX,
    "InvalidSrcCallsign": ERR_INVALID_SRC_CALLSIGN,
    "InvalidCidPassword": ERR_INVALID_CID_PASSWORD,
    "NoSuchCallsign": ERR_NO_SUCH_CALLSIGN,
    "NoFlightPlan": ERR_NO_FLIGHT_PLAN,
    "NoWeatherProfile": ERR_NO_WEATHER_PROFILE,
    "InvalidRevision": ERR_INVALID_REVISION,
    "RequestedLevelTooHigh": ERR_REQUESTED_LEVEL_TOO_HIGH,
    "ServerFull": ERR_SERVER_FULL,
    "CidSuspended": ERR_CID_SUSPENDED,
    "InvalidCtrl": ERR_INVALID_CTRL,
    "RatingTooLow": ERR_RATING_TOO_LOW,
    "InvalidClient": ERR_INVALID_CLIENT,
    "AuthTimeout": ERR_AUTH_TIMEOUT,
}


def resolve_error_code(raw: str) -> int:
    """把服务器下发的错误码（数字或名称）解析成整型；未知返回 -1。"""
    s = (raw or "").strip()
    if not s:
        return -1
    try:
        return int(s)
    except ValueError:
        return ERROR_CODE_NAMES.get(s, -1)


def _qfloor(v: float) -> int:
    """等价于 Qt 的 `qFloor`：真正的向下取整。

    不能用 `int(v)` —— 那是朝 0 截断，负数会偏大 1。PBH 的 pitch/bank
    都是负数（因为乘了 -1），用 `int()` 会让每个正姿态都少 1。
    """
    return math.floor(v)


def encode_pbh(pitch_deg: float, bank_deg: float, heading_deg: float,
               on_ground: bool = False) -> int:
    """PBH 编码（逐项对齐 swift `core/fsd/pbh.h::packPBH`）。

    - pitch = qFloor(pitch * -256/90)    （10bit 有符号，乘 -1 反转）
    - bank  = qFloor(bank  * -512/180)   （10bit 有符号，乘 -1 反转）
    - hdg   = int(heading * 1024/360)    （10bit 无符号，不反转）
    bit0 未用；bit1 onGround；bits2-11 heading；bits12-21 bank；bits22-31 pitch。

    ⚠️ **必须用 floor 而不是截断**。swift 用 `qFloor`，而负数的 `int()` 截断
    是朝 0 取整，两者差 1：

        pitch=1°  → ×-256/90 = -2.844
                    int()=-2   ← 错（朝 0 截断）
                    floor=-3   ← 对（swift）

    这在正俯仰 / 正坡度时**总是**偏 1，相当于 pitch 误差 0.35°、bank 误差 0.18°。
    小角度下看不出来，但对端做插值时会表现为姿态轻微漂移。

    swift 注释也说明了为什么用 `*-1` 而不是按位取反 `~`：
    用 `~` 会让 0 变成 -1（地面上出现过大量 -1 的 PB 值），vPilot 同样用 `*-1`。
    """
    p = _qfloor(pitch_deg * -256.0 / 90.0)
    b = _qfloor(bank_deg * -512.0 / 180.0)
    h = int(heading_deg * 1024.0 / 360.0)
    p = max(-256, min(255, p)) & 0x3FF
    b = max(-512, min(511, b)) & 0x3FF
    h = max(0, min(1023, h))
    return (p << 22) | (b << 12) | (h << 2) | (2 if on_ground else 0)


def decode_pbh(pbh: int) -> tuple[float, float, float, bool]:
    """解码 PBH 位域 → (heading_deg, bank_deg, pitch_deg, on_ground)。

    **这里刻意不逐字照抄 swift 的 `unpackPBH`**，原因如下。

    swift 的解码是：
        const int iPitch = qFloor(pbhstrct.pitch / -pitchMultiplier());
        pitch = iPitch;      // ← 赋给 double 的是**整数**
    因为 encode 用了 qFloor（有损），decode 再 floor 一次，得到的是
    **整度**——真实报文 `pbh=-8389616`（raw pitch=-3）swift 解出恰好 1.0°，
    而对应真实姿态是 1.05°。swift 在 UI 里也不用这个值做插值。

    我们这里要的是**连续姿态**（`model.py` 用它填远端玩家的
    `tr.pitch/bank` 并参与插值显示），所以还原时乘回系数、不做再取整：
        1.05°  而不是 1.0°
    与真实值一致，也让小角度姿态不会看着"台阶化"。

    编码侧（`encode_pbh`）仍严格用 qFloor 对齐 swift —— 那是**发上线的
    字节**，必须和 swift 一致；解码只是本地显示，精度优先。
    """
    h = (pbh >> 2) & 0x3FF
    b = (pbh >> 12) & 0x3FF
    p = (pbh >> 22) & 0x3FF
    if b >= 512:
        b -= 1024
    if p >= 512:
        p -= 1024
    on_ground = ((pbh & 0b10) >> 1) == 1
    # 参照 swift 的系数但乘回（不 floor），保留亚度精度
    pitch = p / (-256.0 / 90.0)
    bank = b / (-512.0 / 180.0)
    heading = h / (1024.0 / 360.0)
    return heading, bank, pitch, on_ground


def add_pilot(callsign: str, cid: str, password: str,
              network_rating: int = NETWORK_RATING,
              protocol_revision: int = PROTOCOL_REVISION,
              simulator_type: int = SIM_TYPE_XP11,
              real_name: str = "") -> str:
    return (f"#AP{callsign}:SERVER:{cid}:{password}:{network_rating}:"
            f"{protocol_revision}:{simulator_type}:{real_name}")


def delete_pilot(callsign: str, cid: str) -> str:
    return f"#DP{callsign}:{cid}"


def pilot_position(callsign: str, transponder_mode: str, squawk: str,
                   network_rating: int, lat: float, lon: float,
                   alt_true_ft: int, gs_kt: int, pbh: int,
                   alt_correction_ft: int = 0,
                   mode: str | None = None) -> str:
    mode = mode if mode is not None else transponder_mode[0]
    return (f"@{mode}:{callsign}:{squawk}:{network_rating}:"
            f"{lat:.5f}:{lon:.5f}:{alt_true_ft}:{gs_kt}:{pbh}:{alt_correction_ft}")


def interim_position(callsign: str, lat: float, lon: float,
                     alt_true_ft: int, gs_kt: int, pbh: int,
                     to: str = "ALL") -> str:
    """#SB VI 中间位置（每 1s，swift INTERIMPOS）。

    格式：#SB<from>:<to>:VI:<lat>:<lon>:<altTrue>:<gs>:<pbh>
    """
    return (f"#SB{callsign}:{to}:VI:{lat:.5f}:{lon:.5f}:"
            f"{int(alt_true_ft)}:{int(gs_kt)}:{int(pbh)}")


def visual_position(callsign: str, lat: float, lon: float,
                    alt_true_ft: float, alt_agl_ft: float, pbh: int,
                    vx_ms: float, vy_ms: float, vz_ms: float,
                    pitch_rate: float = 0.0, heading_rate: float = 0.0,
                    bank_rate: float = 0.0, nose_gear: float = 1.0) -> str:
    """`^` 视觉位置（每 200ms，swift VISUPDATE/Velocity 协议）。

    格式：^呼号:lat:lon:altTrue:altAGL:pbh:vx:vy:vz:pitchRate:hdgRate:bankRate:noseGear
    """
    return (f"^{callsign}:{lat:.7f}:{lon:.7f}:{alt_true_ft:.2f}:"
            f"{alt_agl_ft:.2f}:{int(pbh)}:{vx_ms:.1f}:{vy_ms:.1f}:{vz_ms:.1f}:"
            f"{pitch_rate:.2f}:{heading_rate:.2f}:{bank_rate:.2f}:"
            f"{nose_gear:.2f}")


def plane_info_request(callsign: str, to: str) -> str:
    return f"#SB{callsign}:{to}:PIR"


def plane_info_request_fsi(callsign: str, to: str) -> str:
    return f"#SB{callsign}:{to}:FSIPIR:0"


def plane_info(callsign: str, to: str, equipment: str, airline: str = "",
               livery: str = "", registration: str = "",
               engines: int = 0) -> str:
    """机型信息（PI:GEN）。

## `LIVERY` 必须用 swift 格式，不能塞尾号

**这是 2026-11-25 修掉的一个严重 bug 的结论。**

XFlySim 服务器**从 `LIVERY` 反查机型**，而不是直接用 `EQUIPMENT=`。
证据（同一时刻的服务器 API 响应）：

    swift 发: EQUIPMENT=F14:LIVERY=SWIFT_L181A4278M87561
    服务器存: "equipment":"F14", "livery":"SWIFT_L181A4278M87561"   ✅

    swift 发: EQUIPMENT=A320:LIVERY=SWIFT_L1759A672M87075
    服务器存: "equipment":"A320"                                    ✅

    swift 发: EQUIPMENT=A388:LIVERY=SWIFT_L764A679M89612
    服务器存: "equipment":"A388"                                    ✅

三条的 `A<n>` 都是 aircrafticao.json 的 `id`，且该 id 的 designator
与服务器存下的 equipment 完全一致（4278→F14、672→A320、679→A388）。

而旧代码把**尾号塞进 LIVERY**（为了"穿透 swift 中继"）：

    EQUIPMENT=F14:AIRLINE=SJX:LIVERY=N988DR:ENGINES=2:REG=N988DR

`N988DR` 里没有 `A<id>`，服务器解析不出机型 →
**整条 PI 的 aircraft 段被静默丢弃**：equipment/airline/livery/engines
全部变 null，而服务器**不返回任何错误**（$ER 里看不到）。
这个"静默失败"是它藏了很久的原因。

## 现在的做法

调用方（`session.send_aircraft_info`）传入的 `livery` 已经是
`aircraft_db.swift_livery()` 生成的 `SWIFT_A<id>`；
尾号单独走 `REG=`（服务器认这个键，JSON 里 registration 有值）。

`ENGINES=<数量>` 是服务器识别的扩展键（对应 `aircraft.engines`）。
不传则整键省略，服务器侧为 null。

**字段顺序很重要**：`ENGINES=` 必须紧跟 `LIVERY=`，排在 `REG=` 之前。
服务器 JSON 里 `engines` 的位置恰好夹在 `livery`（PI 组）与
`flapsPct`（ACC 组）之间，说明它属于 PI 组的第 4 个字段。若把 `REG=`
插在中间，服务器按位置取第 4 项拿到 `REG=D-AIKI`，转数字失败 →
`engines` 恒为 null（实测踩过）。所以顺序固定为：
    EQUIPMENT, AIRLINE, LIVERY, ENGINES, REG
"""
    tail = registration or ""
    parts = ["#SB" + callsign + ":" + to + ":PI:GEN"]
    # EQUIPMENT 不能发空值：`EQUIPMENT=` 这种"有键无值"在服务器侧很可能
    # 导致**整条 PI 被丢弃**（表现：equipment/airline/livery 一起变成 null），
    # 而不是仅机型为 null。机型未知时宁可不发这个键。
    # 实测：切到某机型后 icao 一度为空，服务器把整个 PI 扔掉了。
    eq = (equipment or "").strip()
    if eq:
        parts.append(f"EQUIPMENT={eq}")
    if airline:
        parts.append(f"AIRLINE={airline}")
    # LIVERY **只放 swift 格式的 A<id>**，绝不放尾号（见上方说明）。
    # `livery` 为空时直接不写这个键 —— 比写个解析不了的裸尾号安全：
    # 裸尾号会让服务器丢弃整条 PI，而缺键只是让 livery 字段为 null。
    lv = (livery or "").strip()
    if lv:
        parts.append(f"LIVERY={lv}")
    # ENGINES 必须在 REG 之前（见上，服务器按位置取第 4 项）
    if engines and engines > 0:
        parts.append(f"ENGINES={int(engines)}")
    if tail:
        parts.append(f"REG={tail}")
    return ":".join(parts)


def plane_info_fsi(callsign: str, to: str, equipment: str, airline: str = "",
                   model: str = "") -> str:
    return (f"#SB{callsign}:{to}:FSIPI:0:{airline}:{equipment}::::::{model}")


def text_message(callsign: str, to: str, message: str) -> str:
    return f"#TM{callsign}:{to}:{message}"


def metar_request(callsign: str, station: str) -> str:
    return f"$AX{callsign}:SERVER:METAR:{station}"


def pong(callsign: str, to: str, timestamp: str = "0") -> str:
    return f"$PO{callsign}:{to}:{timestamp}"


def auth_response(callsign: str, to: str, hex_response: str) -> str:
    return f"$ZR{callsign}:{to}:{hex_response}"


def aircraft_config(callsign: str, to: str, config_json: str) -> str:
    """$CQ ... :ACC 机模配置（swift：增量广播给 @94836；全量应答给请求方）。"""
    return f"$CQ{callsign}:{to}:ACC:{config_json}"


def aircraft_config_request(callsign: str, to: str) -> str:
    """请求对方全量机模配置。"""
    return aircraft_config(callsign, to, '{"request":"full"}')


def client_query_fp(callsign: str, target: str) -> str:
    """$CQ 飞行计划查询：向服务器请求某呼号的缓存飞行计划（应答 $FP 含机型）。
    用于远程玩家机型识别（位置包不带机型，PIR 无人应答时的兜底）。"""
    return f"$CQ{callsign}:SERVER:FP:{target}"


def flight_plan(callsign: str, flight_type: str, aircraft: str,
                cruise_speed: str, dep_airport: str, plan_time: str,
                actual_time: str, cruise_alt: str, dest_airport: str,
                enroute_h: int, enroute_m: int,
                fuel_h: int, fuel_m: int,
                alt_airport: str = "", remarks: str = "",
                route: str = "") -> str:
    """$FP 飞行计划（swift 17 段格式，报文内禁止冒号）。"""
    def clean(s: str) -> str:
        return (s or "").replace(":", "")

    return (f"$FP{callsign}:SERVER:{flight_type}:{clean(aircraft)}:"
            f"{clean(cruise_speed)}:{clean(dep_airport)}:"
            f"{clean(plan_time)}:{clean(actual_time)}:{clean(cruise_alt)}:"
            f"{clean(dest_airport)}:"
            f"{int(enroute_h)}:{int(enroute_m)}:{int(fuel_h)}:{int(fuel_m)}:"
            f"{clean(alt_airport)}:{clean(remarks)}:{clean(route)}")


def freq_to_target(mhz: float) -> str:
    khz = round(mhz * 1000) - 100000
    if khz < 0:
        khz = 0
    return f"@{khz:05d}"


def khz_to_display(khz: int) -> str:
    full = 100000 + khz
    return f"{full // 1000}.{full % 1000:03d}"


def millihertz_to_display(milli: int) -> str:
    if milli < 100000:
        milli += 100000
    return f"{milli // 1000}.{milli % 1000:03d}"