"""FSD 报文构造（以 swift pilotclient 的 Legacy FSD 协议为准）。

参考 xfly_client/fsd/FSD_PROTOCOL.md：
- PBH 编码：pitch/bank 有符号并取反（×-1），heading 1024 量化
- 位置 @ 经纬度 5 位小数
- 保活：服务器无 $PI 处理器时沿用 #TM...:SERVER:ping（本地约定）
"""

from __future__ import annotations

NETWORK_RATING = 1
PROTOCOL_REVISION = 9
# 模拟器类型按 ISFP 私有服务器约定（xPilot 习惯）：15=XP11, 16=XP12
SIM_TYPE_XP11 = 15
SIM_TYPE_XP12 = 16

CLIENT_ID = 55538          # xPilot-assigned id（非官方，参照 C# 版）
AUTH_KEY = "ImuL1WbbhVuD8d3MuKpWn2rrLZRa9iVP"

SERVER_CALLSIGN = "SERVER"


def encode_pbh(pitch_deg: float, bank_deg: float, heading_deg: float,
               on_ground: bool = False) -> int:
    """PBH 编码（swift 公式）。

    - pitch = qFloor(pitch * -256/90)   （10bit 有符号，取反）
    - bank  = qFloor(bank  * -512/180)  （10bit 有符号，取反）
    - hdg   = heading * 1024/360        （10bit 无符号）
    bit0 未用；bit1 onGround；bits2-11 heading；bits12-21 bank；bits22-31 pitch。
    """
    p = int(pitch_deg * -256.0 / 90.0)
    b = int(bank_deg * -512.0 / 180.0)
    h = int(heading_deg * 1024.0 / 360.0)
    p = max(-256, min(255, p)) & 0x3FF
    b = max(-512, min(511, b)) & 0x3FF
    h = max(0, min(1023, h))
    return (p << 22) | (b << 12) | (h << 2) | (2 if on_ground else 0)


def decode_pbh(pbh: int) -> tuple[float, float, float, bool]:
    """解码 PBH 位域 → (heading_deg, bank_deg, pitch_deg, on_ground)。"""
    h = (pbh >> 2) & 0x3FF
    b = (pbh >> 12) & 0x3FF
    p = (pbh >> 22) & 0x3FF
    if b >= 512:
        b -= 1024
    if p >= 512:
        p -= 1024
    on_ground = ((pbh & 0b10) >> 1) == 1
    return h * 360.0 / 1024.0, -b * 180.0 / 512.0, -p * 90.0 / 256.0, on_ground


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


def plane_info_request(callsign: str, to: str) -> str:
    return f"#SB{callsign}:{to}:PIR"


def plane_info_request_fsi(callsign: str, to: str) -> str:
    return f"#SB{callsign}:{to}:FSIPIR:0"


def plane_info(callsign: str, to: str, equipment: str, airline: str = "",
               livery: str = "") -> str:
    parts = ["#SB" + callsign + ":" + to + ":PI:GEN", f"EQUIPMENT={equipment}"]
    if airline:
        parts.append(f"AIRLINE={airline}")
    if livery:
        parts.append(f"LIVERY={livery}")
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