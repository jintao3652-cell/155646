"""站点上报身份（equipment / airline / livery / engines）的唯一解析点。

## 为什么单独抽一层

站点 JSON 里那四个字段分别来自两条**互不相干**的通道：

| 字段 | 通道 | 载体 |
|---|---|---|
| `equipment` | PI（`#SB…:PI:GEN`） | `EQUIPMENT=` |
| `airline` | PI | `AIRLINE=` |
| `livery` | PI | `LIVERY=` |
| `engines` | ACC（`$CQ…:ACC:{…}`） | `config.engines` |

过去每个调用点各自拼字段（`model.py` 改 icao、`session.py` 算 livery、
`webui` 从呼号推 airline），任何一处漏了就只表现为"某个字段 null"，
而且客户端**不会报错**——服务器是静默丢弃。抽成值对象后：

* 四个字段一次性算完，要么全有出处，要么带 `problems` 明确说缺哪个；
* 缺 `equipment` 或 `livery` 时**整条 PI 不发**（残缺 PI 会覆盖服务器记录）；
* `engines` 有第二级兜底：桥实测值 → swift 机型库台数，避免桥给 0 就永远不报。

纯函数、无 IO（只查本地机型库），可单测、可在任意线程调用。
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from typing import Optional

from . import aircraft_db

# 呼号里的航司前缀：SJX110 → SJX、CCA1378 → CCA。
# 只认 2~4 个字母紧跟数字的形式，`N12345` 这种注册号式呼号不算。
_CALLSIGN_AIRLINE = re.compile(r"^([A-Z]{2,4})(?=\d)")

# 站点字段名 → 缺字段时的提示文案（诊断输出用）
FIELD_NAMES = {
    "equipment": "aircraft.equipment",
    "airline": "aircraft.airline",
    "livery": "aircraft.livery",
    "engines": "aircraft.engines",
}


def airline_from_callsign(callsign: str) -> str:
    """从呼号推航司三字码（推不出返回空串，绝不瞎猜）。"""
    m = _CALLSIGN_AIRLINE.match((callsign or "").strip().upper())
    return m.group(1) if m else ""


@dataclass(frozen=True)
class AircraftIdentity:
    """一次上报所需的完整身份。字段为空表示"这条通道给不出值"。"""

    equipment: str = ""
    airline: str = ""
    livery: str = ""
    registration: str = ""
    engines: int = 0
    sources: tuple[tuple[str, str], ...] = ()   # (字段, 出处) —— 诊断用
    problems: tuple[str, ...] = ()

    # ---------- 派生 ----------

    @property
    def missing(self) -> tuple[str, ...]:
        """站点上会显示 null 的字段（按站点字段名）。"""
        out: list[str] = []
        if not self.equipment:
            out.append(FIELD_NAMES["equipment"])
        if not self.airline:
            out.append(FIELD_NAMES["airline"])
        if not self.livery:
            out.append(FIELD_NAMES["livery"])
        if self.engines <= 0:
            out.append(FIELD_NAMES["engines"])
        return tuple(out)

    @property
    def pi_sendable(self) -> bool:
        """能不能发 PI。

        `EQUIPMENT` 与 `LIVERY` 都为空时整条不发：残缺 PI 会被服务器当成一次
        机型更新，把已存好的 aircraft 段整块覆盖成 null（实测 fsd.log
        18:17:29，`PI:GEN:AIRLINE=SJX` → equipment/livery/engines 全 null）。
        """
        return bool(self.equipment or self.livery)

    def source_of(self, field_name: str) -> str:
        for name, src in self.sources:
            if name == field_name:
                return src
        return ""

    # ---------- 输出 ----------

    def audit(self) -> str:
        """单行自检日志：四个字段 + 出处 + 缺失项。"""
        def show(v: object) -> str:
            return str(v) if v not in ("", 0) else "-"

        txt = (f"[report] equipment={show(self.equipment)} "
               f"airline={show(self.airline)} livery={show(self.livery)} "
               f"engines={show(self.engines)} reg={show(self.registration)}")
        src = ",".join(f"{k}:{v}" for k, v in self.sources)
        if src:
            txt += f" src=[{src}]"
        if self.missing:
            txt += f" ⚠ missing={'/'.join(self.missing)}"
        return txt


def resolve_identity(*, icao: str = "", name: str = "", airline: str = "",
                     registration: str = "", measured_engines: int = 0,
                     manual_icao: str = "", callsign: str = "",
                     livery_override: str = "",
                     registration_in_livery: bool = True) -> AircraftIdentity:
    """把桥/配置/呼号里的碎料拼成可上报的身份。

    参数
    ----
    icao
        桥上报的机型码（可能是 `ATCCOM`、`A350-900` 这类非标准值）。
    name
        桥上报的机模名（ICAO 不可用时用它反查 swift 机型库）。
    airline
        用户配置/界面里的航司码；空则按呼号推。
    registration
        生效尾号（四级优先级的结果，由 `registration.resolve_registration` 给）。
    measured_engines
        桥实测发动机台数；0 = 未知，退回机型库。
    manual_icao
        用户在设置里手填的机型（非空时锁定，桥不再覆盖）。
    livery_override
        调用方已经算好的 LIVERY（如自带涂装码），原样使用。
    registration_in_livery
        LIVERY 末尾是否附加实际尾号（`SWIFT_A672-B-8493`）。服务器按 `A<id>`
        反查机型，尾号只是给人看的；某台服务器校验严格时可关掉。
    """
    sources: list[tuple[str, str]] = []
    problems: list[str] = []

    # ---- equipment：手填优先 → 桥值规范化 → 机模名反查 ----
    raw = (manual_icao or icao or "").strip().upper()
    if manual_icao:
        equipment = (manual_icao or "").strip().upper()
        sources.append(("equipment", "manual"))
    else:
        try:
            equipment = aircraft_db.normalize_icao(
                raw, name or "", int(measured_engines or 0))
        except Exception:                     # 机型库异常不能拖垮上报
            equipment = raw if not raw.startswith("ATC") else ""
        sources.append(("equipment", "sim" if equipment == raw and raw else
                        ("inferred" if equipment else "missing")))
    if not equipment:
        problems.append("equipment")

    # ---- livery：swift 格式 `SWIFT_A<id>` ----
    livery = (livery_override or "").strip()
    if livery:
        sources.append(("livery", "override"))
    else:
        lv = ""
        if equipment:
            try:
                lv = aircraft_db.swift_livery(equipment)
            except Exception:
                lv = ""
        if not lv and name:
            # 机型码查不到 id（冷门机模）：再拿机模名推一次，推出来必须
            # 与 equipment 同步，否则 LIVERY 的 A<id> 和 EQUIPMENT 会打架。
            try:
                guess = aircraft_db.guess_designator(
                    name, int(measured_engines or 0))
                if guess and aircraft_db.swift_livery(guess):
                    equipment, lv = guess, aircraft_db.swift_livery(guess)
                    sources.append(("equipment", "name"))
            except Exception:
                pass
        livery = lv
        sources.append(("livery", "db" if lv else "missing"))
    if not livery:
        problems.append("livery")

    reg = (registration or "").strip().upper()
    if livery and reg and registration_in_livery:
        livery = f"{livery}-{reg}"

    # ---- engines：实测 → 机型库台数 ----
    engines = int(measured_engines or 0)
    if engines > 0:
        sources.append(("engines", "sim"))
    else:
        try:
            rec = aircraft_db.lookup(equipment) if equipment else None
            engines = int((rec or {}).get("engines") or 0)
        except Exception:
            engines = 0
        sources.append(("engines", "db" if engines > 0 else "missing"))
    if engines <= 0:
        problems.append("engines")

    # ---- airline：配置 → 呼号前缀 ----
    air = (airline or "").strip().upper()
    if air:
        sources.append(("airline", "config"))
    else:
        air = airline_from_callsign(callsign)
        sources.append(("airline", "callsign" if air else "missing"))
    if not air:
        problems.append("airline")

    return AircraftIdentity(
        equipment=equipment, airline=air, livery=livery,
        registration=reg, engines=max(0, engines),
        sources=tuple(sources), problems=tuple(problems),
    )


def engine_mask(n: int) -> dict[str, dict[str, bool]]:
    """给 ACC 用的"全部熄火"占位掩码（桥没给 engines 时的兜底）。

    服务器把 `config.engines` 原样抄进站点 `aircraft.engines`；缺这个键就是
    null。宁可先报"全关"（下一帧真实值会覆盖），也不留 null。
    """
    n = max(0, min(int(n or 0), 8))
    return {str(i): {"on": False} for i in range(1, n + 1)}


def engines_from_parts(parts: Optional[dict]) -> int:
    """从 ACC/parts 里数引擎台数（含 `{"1":{"on":..}}` / `[0,1]` 两种形状）。"""
    if not isinstance(parts, dict):
        return 0
    eng = parts.get("engines")
    if isinstance(eng, dict):
        return len(eng)
    if isinstance(eng, (list, tuple)):
        return len(eng)
    try:
        return int(parts.get("engine_count") or 0)
    except (TypeError, ValueError):
        return 0