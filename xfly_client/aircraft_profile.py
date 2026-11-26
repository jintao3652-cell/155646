"""机型档案 + 部件状态统一中间层（对齐 swift 的 CAircraftIcaoCode / CAircraftParts）。

## 为什么需要这一层

采集端 X-Plane 给 9 个灯、MSFS 给 5 个灯；上报端 FSD 服务器只收 6 个灯；
渲染端 CSL 只认 5 个灯位 + 单个 engine_rotation_angle_deg[0]。
任何一端直接吃另一端的原始字段，都会出现"多出的灯被丢、缺的灯被当成关、
4 发被压成 1 发"这类脱节。

swift 的做法是先用值对象把差异吃掉：
  - CAircraftIcaoCode / CAircraftCategory：机型属性（引擎数、类型、类别）归一化
  - CAircraftLights / CAircraftEngineList：9 灯 + 1..n 发（1-based，可变长度）
  - CAircraftParts::PartsDetails：NotSet / FSDAircraftParts / GuessedParts —— 数据出处
  - toFullJson() / toIncrementalJson()：is_full_data 标记决定合并还是覆盖
  - getAdjustedLights() / isFixedGearDown()：渲染前的兜底推导

本模块照这套契约实现，作为上报和渲染之间唯一的 canonical 表示。
"""

from __future__ import annotations

import math
from functools import lru_cache
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Mapping, Optional

from . import aircraft_db

# swift CAircraftLights 的 9 个灯（顺序与 CAircraftLights::ColumnIndex 一致）
LIGHT_KEYS: tuple[str, ...] = (
    "strobe_on", "landing_on", "taxi_on", "beacon_on", "nav_on",
    "logo_on", "recognition_on", "cabin_on", "wing_on",
)
# XFlySim FSD 服务器 ACC schema 接受的 6 个灯。多一个未知键整块被丢。
SERVER_LIGHT_KEYS: tuple[str, ...] = LIGHT_KEYS[:6]

# 采集端 camelCase（用户 JSON / 部分插件）→ canonical snake_case
_LIGHT_ALIASES = {
    "strobe": "strobe_on", "strobeon": "strobe_on",
    "landing": "landing_on", "landingon": "landing_on",
    "taxi": "taxi_on", "taxion": "taxi_on",
    "beacon": "beacon_on", "beaconon": "beacon_on",
    "nav": "nav_on", "navon": "nav_on",
    "logo": "logo_on", "logoon": "logo_on",
    "recognition": "recognition_on", "recognitionon": "recognition_on",
    "cabin": "cabin_on", "cabinon": "cabin_on",
    "wing": "wing_on", "wingon": "wing_on",
}

SOURCE_NOT_SET = "not_set"
SOURCE_FSD = "fsd_parts"
SOURCE_GUESS = "guessed"


class EngineType(str, Enum):
    """swift combinedType 的第 3 位：J/T/P/R。"""

    JET = "J"
    TURBOPROP = "T"
    PISTON = "P"
    ROTOR = "R"
    UNKNOWN = "?"


# 这些设计器前缀/代码在 swift 的 aircrafticao.json 里是 T/P/R。
# 保留为数据驱动表：新增机型加一行即可，不要在渲染处写 if aircraft == ...
_TURBOPROP = (
    "AT45", "AT46", "AT72", "AT73", "AT75", "AT76", "ATR", "DH8", "DHC",
    "Q400", "B190", "BE20", "C130", "C295", "CN35", "F27", "F50", "L188",
    "E110", "E120", "SB20", "J41", "JS41", "A400", "C27J", "TU95", "TU114",
    "AN24", "AN26", "AN32", "IL114", "AG60", "MA60", "MA6", "Z20", "YY20",
    "Y20", "CLT", "VISC", "DHC6", "DHC2", "TBM", "PC12", "PC6", "PA31",
)
_PISTON = (
    "C150", "C152", "C172", "C182", "C185", "C206", "C207", "C210", "C310",
    "C337", "PA28", "PA32", "PA34", "PA44", "P28", "BE36", "BE33", "BE58",
    "DA40", "DA42", "DA50", "SR20", "SR22", "RV", "MOONEY", "BONANZA",
    "GLID", "PIPER", "CESS", "CUB", "LSA", "S22T", "CTLS", "TXT",
)
_ROTOR = (
    "EC12", "EC13", "EC14", "EC15", "EC35", "EC45", "AS32", "AS35", "AS50",
    "AW10", "AW13", "AW13", "AW16", "AW39", "BELL", "B407", "B412", "B429",
    "H145", "H160", "H175", "MI8", "MI17", "MI26", "MI28", "KA50", "KA52",
    "S70", "S76", "S92", "UH60", "AH64", "CH47", "R22", "R44", "R66",
    "H125", "H130", "H135", "H140", "H225", "Z20", "Z8", "Z10", "WZ10",
)


def _classify_engine_type(designator: str, engines: int, wtc: str) -> EngineType:
    d = (designator or "").upper()
    if d.startswith(_ROTOR):
        return EngineType.ROTOR
    if d.startswith(_TURBOPROP):
        return EngineType.TURBOPROP
    if d.startswith(_PISTON):
        return EngineType.PISTON
    # 兜底：涡扇/涡喷运输类。引擎数未知时保持 unknown，交给上报端实测值。
    if engines >= 2 and wtc in ("M", "H"):
        return EngineType.JET
    if engines == 1 and wtc in ("M", "H"):
        return EngineType.JET
    return EngineType.UNKNOWN


@dataclass(frozen=True)
class AircraftProfile:
    """一架飞机的静态属性。渲染/上报只通过档案取引擎数、类型、起落架类别。"""

    designator: str = ""
    engines: int = 0                       # 0 = 数据库未知，用实测值
    engine_type: EngineType = EngineType.UNKNOWN
    wtc: str = ""
    family: str = ""
    gear: str = "retractable"              # fixed | retractable | unknown
    source: str = "unknown"

    @classmethod
    @lru_cache(maxsize=512)
    def from_designator(cls, designator: str, *, measured_engines: int = 0) -> "AircraftProfile":
        des = (designator or "").strip().upper()
        if not des:
            return cls()
        try:
            rec = aircraft_db.lookup(des)
        except Exception:                  # DB 缺失不能拖垮上报链路
            rec = None
        if not rec:
            engines = int(measured_engines or 0)
            official = aircraft_db.engine_type_of(des)
            try:
                etype = EngineType(official) if official else _classify_engine_type(
                    des, engines, "")
            except ValueError:
                etype = _classify_engine_type(des, engines, "")
            return cls(
                designator=des, engines=engines,
                engine_type=etype,
                gear="fixed" if des in _PISTON and des.startswith(("C150", "C172", "PA28", "DA40")) else "retractable",
                source="fallback",
            )
        engines = int(rec.get("engines") or 0) or int(measured_engines or 0)
        wtc = rec.get("wtc") or ""
        # 官方 DB 的 engine 字段优先；缺失才走前缀表 + WTC 启发式。
        official = (rec.get("engine_type") or "").upper()
        try:
            etype = EngineType(official) if official else _classify_engine_type(
                des, engines, wtc)
        except ValueError:
            etype = _classify_engine_type(des, engines, wtc)
        return cls(
            designator=des,
            engines=engines,
            engine_type=etype,
            wtc=wtc,
            family=rec.get("model") or "",
            gear="fixed" if des.startswith(("C150", "C152", "PA28", "DA40", "R22", "R44")) else "retractable",
            source="aircraft_db",
        )


def _as_tri(raw: Any) -> Optional[bool]:
    """三态布尔：NaN/None/空串 = 未知(None)，不是 False。"""
    if raw is None:
        return None
    if isinstance(raw, bool):
        return raw
    if isinstance(raw, (int, float)):
        if isinstance(raw, float) and not math.isfinite(raw):
            return None
        return raw != 0
    if isinstance(raw, str):
        s = raw.strip().lower()
        if s in ("", "null", "none", "nan", "unknown", "-1"):
            return None
        if s in ("true", "yes", "on", "1"):
            return True
        if s in ("false", "no", "off", "0"):
            return False
    return None


@dataclass
class AircraftParts:
    """canonical 部件状态。字段缺省表示"未上报"，绝不擅自当成 off/up。"""

    lights: dict[str, bool] = field(default_factory=dict)
    gear_down: Optional[bool] = None
    flaps_pct: Optional[int] = None
    spoilers_out: Optional[bool] = None
    # 油门 0..1（各发共用一个比值；None = 未上报）。
    # 上报时量化到 5%（round(x*20)/20）：推油门是连续动作，不量化的话
    # 每帧都变 → ACC 增量把令牌桶吃光，灯光/起落架更新会被饿死。
    throttle_ratio: Optional[float] = None
    engines: dict[int, bool] = field(default_factory=dict)   # 1-based，1..n
    on_ground: Optional[bool] = None
    source: str = SOURCE_NOT_SET
    is_full_data: bool = False
    timestamp_ms: int = 0

    # ---------- 归一化 ----------

    @classmethod
    def from_raw(cls, raw: Optional[Mapping[str, Any]], *,
                 source: str = SOURCE_FSD,
                 is_full_data: bool = True,
                 timestamp_ms: int = 0) -> "AircraftParts":
        raw = raw or {}
        p = cls(source=source, is_full_data=bool(is_full_data), timestamp_ms=timestamp_ms)

        lights = raw.get("lights") or {}
        if isinstance(lights, Mapping):
            for k, v in lights.items():
                lk = str(k).lower()
                key = lk if lk in LIGHT_KEYS else _LIGHT_ALIASES.get(lk)
                if key is None or key not in LIGHT_KEYS:
                    continue
                b = _as_tri(v)
                if b is not None:
                    p.lights[key] = b
        # 顶层灯字段（部分模拟器直出 beaconOn 等）
        for k, v in raw.items():
            lk = str(k).lower()
            key = lk if lk in LIGHT_KEYS else _LIGHT_ALIASES.get(lk)
            if key and key not in p.lights:
                b = _as_tri(v)
                if b is not None:
                    p.lights[key] = b

        p.gear_down = _as_tri(raw.get("gear_down", raw.get("gearDown")))
        p.spoilers_out = _as_tri(raw.get("spoilers_out", raw.get("spoilersOut")))
        p.on_ground = _as_tri(raw.get("on_ground", raw.get("onGround")))

        if "throttle_ratio" in raw:
            try:
                t = float(raw["throttle_ratio"])
                if math.isfinite(t):
                    t = max(0.0, min(1.0, t))
                    p.throttle_ratio = round(t * 20.0) / 20.0   # 5% 量化
            except (TypeError, ValueError):
                pass

        flaps = raw.get("flaps_pct")
        if flaps is None:
            flaps = raw.get("flapsPercent", raw.get("flapsDown"))
        if flaps is None and "flaps_ratio" in raw:
            try:
                flaps = float(raw["flaps_ratio"]) * 100.0
            except (TypeError, ValueError):
                flaps = None
        if flaps is not None:
            try:
                f = float(flaps)
                if math.isfinite(f):
                    p.flaps_pct = max(0, min(100, int(f + 0.5)))
            except (TypeError, ValueError):
                pass

        p.engines = cls._norm_engines(raw)
        return p

    @staticmethod
    def _norm_engines(raw: Mapping[str, Any]) -> dict[int, bool]:
        """接受 {"1":{"on":1}} / {"1":true} / [1,0] / {"engine_count":4}。"""
        out: dict[int, bool] = {}
        eng = raw.get("engines")
        if isinstance(eng, Mapping):
            for key, val in eng.items():
                try:
                    idx = int(str(key).strip())
                except (TypeError, ValueError):
                    continue
                if idx < 1 or idx > 8:      # 远超真实最大 4，留扩展余量
                    continue
                b = _as_tri(val.get("on") if isinstance(val, Mapping) else val)
                if b is not None:
                    out[idx] = b
        elif isinstance(eng, (list, tuple)):
            for i, val in enumerate(eng, 1):
                b = _as_tri(val)
                if b is not None:
                    out[i] = b
        count = raw.get("engine_count") or raw.get("engines_count")
        if count is not None:
            try:
                n = int(count)
                for i in range(1, min(max(n, 0), 8) + 1):
                    out.setdefault(i, False)
            except (TypeError, ValueError):
                pass
        return out

    # ---------- swift 对应语义 ----------

    def get_adjusted_lights(self) -> dict[str, bool]:
        """swift getAdjustedLights()：cabin/recognition 由任一发动机在转推导。"""
        out = dict(self.lights)
        any_engine = any(self.engines.values())
        out["recognition_on"] = any_engine
        out["cabin_on"] = any_engine
        return out

    def is_fixed_gear_down(self) -> Optional[bool]:
        """swift isFixedGearDown()：gear_down 或 on_ground 即视为放下。"""
        if self.gear_down is True or self.on_ground is True:
            return True
        if self.gear_down is False:
            return False
        return None

    def effective(self, profile: Optional[AircraftProfile] = None) -> dict[str, Any]:
        """渲染/上报前的最终解析值。未知项保持 None，由调用方决定兜底。"""
        gear = self.is_fixed_gear_down()
        if gear is None and profile is not None and profile.gear == "fixed":
            gear = True
        count = (profile.engines if profile and profile.engines else 0)
        count = max(count, max(self.engines, default=0))
        engines = {i: self.engines.get(i, False) for i in range(1, count + 1)}
        return {
            "lights": self.get_adjusted_lights(),
            "gear_down": gear,
            "flaps_pct": self.flaps_pct or 0,
            "spoilers_out": bool(self.spoilers_out),
            "engines": engines,
            "on_ground": bool(self.on_ground),
            "engine_count": len(engines),
            "source": self.source,
        }

    # ---------- 序列化 ----------

    def _server_fields(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "lights": {k: bool(v) for k, v in self.lights.items() if k in SERVER_LIGHT_KEYS}
        }
        if self.gear_down is not None:
            out["gear_down"] = bool(self.gear_down)
        if self.spoilers_out is not None:
            out["spoilers_out"] = bool(self.spoilers_out)
        if self.throttle_ratio is not None:
            out["throttle_ratio"] = float(self.throttle_ratio)
        if self.flaps_pct is not None:
            out["flaps_pct"] = int(self.flaps_pct)
        if self.on_ground is not None:
            out["on_ground"] = bool(self.on_ground)
        if self.engines:
            out["engines"] = {str(i): {"on": bool(on)} for i, on in sorted(self.engines.items())}
        return out

    def to_full_json(self) -> dict[str, Any]:
        out = self._server_fields()
        out["is_full_data"] = True
        return out

    def to_incremental_json(self) -> dict[str, Any]:
        out = self._server_fields()
        out["is_full_data"] = False
        return out

    # ---------- 增量合并 ----------

    def merge(self, incoming: "AircraftParts") -> "AircraftParts":
        """is_full_data=true 时整块替换；否则只覆盖显式出现的字段（swift 语义）。"""
        if incoming.is_full_data:
            return AircraftParts(
                lights=dict(incoming.lights),
                gear_down=incoming.gear_down,
                flaps_pct=incoming.flaps_pct,
                spoilers_out=incoming.spoilers_out,
                throttle_ratio=incoming.throttle_ratio,
                engines=dict(incoming.engines),
                on_ground=incoming.on_ground,
                source=incoming.source or self.source,
                is_full_data=True,
                timestamp_ms=incoming.timestamp_ms or self.timestamp_ms,
            )
        out = AircraftParts(
            lights=dict(self.lights), gear_down=self.gear_down,
            flaps_pct=self.flaps_pct, spoilers_out=self.spoilers_out,
            throttle_ratio=self.throttle_ratio,
            engines=dict(self.engines), on_ground=self.on_ground,
            source=self.source, is_full_data=False,
            timestamp_ms=self.timestamp_ms,
        )
        out.lights.update(incoming.lights)
        for name in ("gear_down", "flaps_pct", "spoilers_out", "on_ground",
                     "throttle_ratio"):
            val = getattr(incoming, name)
            if val is not None:
                setattr(out, name, val)
        if incoming.engines:
            out.engines.update(incoming.engines)
        if incoming.source != SOURCE_NOT_SET:
            out.source = incoming.source
        if incoming.timestamp_ms:
            out.timestamp_ms = incoming.timestamp_ms
        return out
