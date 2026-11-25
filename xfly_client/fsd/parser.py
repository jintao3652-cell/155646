"""FSD 报文解析：全类型解析 + 位置包字段提取。"""

from __future__ import annotations

from dataclasses import dataclass, field, fields as dc_fields


@dataclass
class ParsedPacket:
    kind: str = ""
    from_: str = ""
    to: str = ""
    type: str = ""
    flds: list[str] = field(default_factory=list)
    raw: str = ""

    @property
    def fields(self) -> list[str]:
        return self.flds

    def __str__(self) -> str:
        return self.raw


@dataclass
class PilotPositionData:
    callsign: str = ""
    mode: str = "S"
    squawk: str = ""
    rating: int = 0
    lat: float = 0.0
    lon: float = 0.0
    alt_ft: int = 0
    gs_kt: int = 0
    pbh: int = 0
    on_ground: bool = False   # PBH bit1（@ 与 VI/SL/ST 都携带）
    agl_ft: float = -1.0      # 遥测离地高度（仅 ^ / #SL / #ST 携带，-1 未知）


@dataclass
class AtcPositionData:
    callsign: str = ""
    freq_khz: int = 0
    facility: int = 0
    rating: int = 0
    lat: float = 0.0
    lon: float = 0.0


_NO_TO = {"@", "^", "#ST", "#SL", "#DP", "#DA", "%"}
_WITH_TO = {"#AP", "#AA", "#TM", "#SB", "$DI", "$ID", "$ER",
            "$AX", "$AR", "$CQ", "$CR", "$PI", "$PO", "$ZC",
            "$ZR", "$FP", "$!!"}


def get_kind(line: str) -> str:
    if not line:
        return ""
    if line[0] in "@^%":
        return line[0]
    for prefix in ("#SB", "#ST", "#SL", "#DP", "#DA", "#AP", "#AA", "#TM",
                   "$DI", "$ID", "$ER", "$AX", "$AR", "$CQ", "$CR",
                   "$PI", "$PO", "$ZC", "$ZR", "$!!"):
        if line.startswith(prefix):
            return prefix
    return line[:2]


def parse(line: str) -> ParsedPacket:
    p = ParsedPacket(raw=line)
    if len(line) < 3:
        return p

    kind = get_kind(line)
    rest = line[len(kind):]
    f = rest.split(":")

    if kind in _NO_TO:
        p.kind = kind
        if kind == "@":
            # @mode:from:squawk:rating:lat:lon:alt:gs:pbh:corr
            p.from_ = f[1] if len(f) > 1 else ""
            p.flds = f
        else:
            # %from:freq:facility:vis:rating:lat:lon:0
            # #DPfrom:cid  / 快位置 from:lat:lon:...
            p.from_ = f[0] if f else ""
            p.flds = f
    elif kind in _WITH_TO:
        p.kind = kind
        p.from_ = f[0] if f else ""
        if len(f) > 1:
            p.to = f[1]
        if len(f) > 2:
            p.type = f[2]
        if len(f) > 3:
            p.flds = f[3:]
    else:
        p.kind = kind
        p.type = "RAW"
    return p


def parse_freq(raw: str) -> int:
    """解析 "22800" 或 "22800&19980" 频率字段，返回首个频率（kHz 偏移）。"""
    first = raw.split("&")[0]
    try:
        return int(float(first))
    except ValueError:
        return 0


def try_get_atc_position(p: ParsedPacket) -> AtcPositionData | None:
    if p.kind != "%" or len(p.flds) < 7:
        return None
    try:
        return AtcPositionData(
            callsign=p.flds[0],
            freq_khz=parse_freq(p.flds[1]),
            facility=int(float(p.flds[2])),
            rating=int(float(p.flds[4])),
            lat=float(p.flds[5]),
            lon=float(p.flds[6]),
        )
    except ValueError:
        return None


def _pbh_on_ground(pbh: int) -> bool:
    """PBH bit1 = onGround（swift packPBH / vPilot 同约定）。"""
    return bool((pbh >> 1) & 1)


def try_get_pilot_position(p: ParsedPacket) -> PilotPositionData | None:
    if p.kind == "@":
        f = p.flds
        if len(f) < 10:
            return None
        try:
            pbh = int(float(f[8]))
            return PilotPositionData(
                callsign=f[1],
                mode=f[0][0] if f[0] else "S",
                squawk=f[2],
                rating=int(float(f[3])),
                lat=float(f[4]),
                lon=float(f[5]),
                alt_ft=int(float(f[6])),
                gs_kt=int(float(f[7])),
                pbh=pbh,
                on_ground=_pbh_on_ground(pbh),
            )
        except ValueError:
            return None

    # 快速位置变体 ^ #SL #ST: from:lat:lon:altTrue:altAgl:pbh[:vel...]
    if p.kind in ("^", "#SL", "#ST"):
        f = p.flds
        if len(f) < 6:
            return None
        try:
            pbh = int(float(f[5]))
            agl = float(f[4])
            return PilotPositionData(
                callsign=f[0],
                mode="N",
                squawk="",
                lat=float(f[1]),
                lon=float(f[2]),
                alt_ft=int(float(f[3])),
                gs_kt=0,  # 快速包不携带地速，保留上次 @ 包的值
                pbh=pbh,
                # AGL≈0 也视为在地面（部分客户端 PBH 的 onGround 位恒为 0）
                on_ground=_pbh_on_ground(pbh) or (0.0 <= agl <= 1.5),
                agl_ft=agl,
            )
        except ValueError:
            return None

    # swift 中间位置：#SB<from>:<to>:VI:<lat>:<lon>:<altTrue>:<gs>:<pbh>
    # （SquawkBox 旧格式子类型 I 与 swift 一样**不解析**：字段布局存在历史差异，
    #   精度也差，误判会把飞机放到错误位置；@ 包 5s 一次已足够兜底）
    if p.kind == "#SB" and p.type == "VI":
        f = p.flds          # flds[0]=lat, 1=lon, 2=alt, 3=gs, 4=pbh
        if len(f) < 5:
            return None
        try:
            pbh = int(float(f[4]))
            return PilotPositionData(
                callsign=p.from_,
                mode="N",
                squawk="",
                lat=float(f[0]),
                lon=float(f[1]),
                alt_ft=int(float(f[2])),
                gs_kt=int(float(f[3])),
                pbh=pbh,
                on_ground=_pbh_on_ground(pbh),
            )
        except ValueError:
            return None

    return None


def extract_fields(data) -> dict:
    return {f.name: getattr(data, f.name) for f in dc_fields(data)}