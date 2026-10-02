"""FSD 报文构造：PBH 编码、频率换算、各包类型构造。"""

from __future__ import annotations

NETWORK_RATING = 1
PROTOCOL_REVISION = 9
SIM_TYPE_XP11 = 15
SIM_TYPE_XP12 = 16

CLIENT_ID = 55538  # xPilot-assigned id（非官方使用，参照 C# 版）
CLIENT_NAME = "XFlySim-Client"
AUTH_KEY = "ImuL1WbbhVuD8d3MuKpWn2rrLZRa9iVP"

SERVER_CALLSIGN = "SERVER"


def _quantize(deg: float) -> int:
    """角度 → 0..1023 量化。"""
    if deg < 0:
        deg += 360
    if deg > 360:
        deg %= 360
    v = round(deg * 1023.0 / 360.0)
    return max(0, min(1023, v))


def encode_pbh(pitch_deg: float, bank_deg: float, heading_deg: float,
               on_ground: bool = False) -> int:
    """编码 PBH 位域。bit1=on_ground，bit2-11 heading，bit12-21 bank，bit22-31 pitch。"""
    p = _quantize(pitch_deg)
    b = _quantize(bank_deg)
    h = _quantize(heading_deg)
    return (p << 22) | (b << 12) | (h << 2) | (2 if on_ground else 0)


def decode_pbh(pbh: int) -> tuple[float, float, float, bool]:
    """解码 PBH 位域 → (heading_deg, bank_deg, pitch_deg, on_ground)。"""
    h = (pbh >> 2) & 0x3FF
    b = (pbh >> 12) & 0x3FF
    p = (pbh >> 22) & 0x3FF
    on_ground = ((pbh & 0b10) >> 1) == 1
    return h * 360.0 / 1023.0, b * 360.0 / 1023.0, p * 360.0 / 1023.0, on_ground


def client_identification(callsign: str, cid: str, client_id: int = CLIENT_ID,
                          client_name: str = CLIENT_NAME, major: int = 0,
                          minor: int = 0, sys_uid: int = 0,
                          hex_key: str = "") -> str:
    return (f"$ID{callsign}:SERVER:{client_id:04x}:{client_name}:"
            f"{major}:{minor}:{cid}:{sys_uid}:{hex_key}")


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
            f"{lat:.6f}:{lon:.6f}:{alt_true_ft}:{gs_kt}:{pbh}:{alt_correction_ft}")


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


def client_query_response(callsign: str, to: str, query_type: str,
                          *payload: str) -> str:
    line = f"$CR{callsign}:{to}:{query_type}"
    if payload:
        line += ":" + ":".join(payload)
    return line


def auth_response(callsign: str, to: str, hex_response: str) -> str:
    return f"$ZR{callsign}:{to}:{hex_response}"


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


def fsd_freq_to_display(fsd_freq: int) -> str:
    return khz_to_display(fsd_freq)