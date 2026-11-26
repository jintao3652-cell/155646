"""SimBrief 飞行计划导入。

SimBrief 提供免登录的公开接口，按用户名（或数字用户 ID）取回**最新一份 OFP**：

    https://www.simbrief.com/api/xml.fetcher.php?username=<用户名>&json=1
    https://www.simbrief.com/api/xml.fetcher.php?userid=<数字ID>&json=1

返回的 JSON 结构庞大（含 navlog、天气、燃油明细、PDF/图片链接等），这里只抽出
与本客户端 `$FP` 飞行计划对应的字段，并统一成本表单使用的格式：

| 表单字段 | SimBrief 来源 |
|---|---|
| 起降/备降场 | `origin.icao_code` / `destination.icao_code` / `alternate.icao_code` |
| 航路 | `general.route`（缺失时 `general.route_ifps`） |
| 巡航高度 | `general.initial_altitude`（英尺 → `FL350`） |
| 巡航速度 | `general.cruise_tas`（节） |
| 计划起飞时间 | `times.est_off`（UTC → `HHMM`） |
| 航程时/分 | `times.est_off → est_on` 差值（回退 `est_enroute`） |
| 燃油时/分 | 总油量/航段耗油 折算的续航（回退 航程+45min） |
| 机型 / 注册号 | `aircraft.icao_code` / `aircraft.reg` |

时间字段在不同版本里可能是 ISO 串（`2026-11-24T12:34:00Z`）、`HH:MM` 或
`HHMM`，解析器三种都吃；航程优先用起降时刻相减，比 `est_enroute` 更稳。
"""

from __future__ import annotations

import json
import re
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone

from .registration import norm_registration

API_URL = "https://www.simbrief.com/api/xml.fetcher.php"
USER_AGENT = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
              "AppleWebKit/537.36 (KHTML, like Gecko) "
              "XFlySim-Client/1.0")

# 表单里「航程/燃油」两栏是时:分，且小时输入框上限 23
_MAX_MINUTES = 23 * 60 + 59


class SimbriefError(Exception):
    """导入失败。`code` 供前端翻译文案（errors.<code>），`detail` 供日志。"""

    def __init__(self, code: str, detail: str = "") -> None:
        super().__init__(detail or code)
        self.code = code
        self.detail = detail


# ---------- 取数 ----------

def fetch_ofp(username: str, timeout: float = 10.0) -> dict:
    """按用户名/用户 ID 拉取最新 OFP 的 JSON 原文。

    `timeout` 同时作用于连接与读取；网络不通时用户最多等这么久（界面有加载态）。
    """
    u = (username or "").strip()
    if not u:
        raise SimbriefError("simbrief_no_user")
    # 纯数字视为 SimBrief 用户 ID（个人页面 /user/<id>），否则按用户名查
    key = "userid" if u.isdigit() else "username"
    url = f"{API_URL}?{key}={urllib.parse.quote(u)}&json=1"
    req = urllib.request.Request(
        url, headers={"User-Agent": USER_AGENT, "Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            raw = resp.read()
    except urllib.error.HTTPError as exc:
        raise SimbriefError("simbrief_http", f"HTTP {exc.code}") from exc
    except urllib.error.URLError as exc:
        # 代理隧道错误（公司网/全局代理）单独归一类，文案更准确
        reason = str(getattr(exc, "reason", exc))
        if "tunnel" in reason.lower() or "proxy" in reason.lower():
            raise SimbriefError("simbrief_proxy", reason) from exc
        raise SimbriefError("simbrief_network", reason) from exc
    except Exception as exc:                      # 超时/DNS/TLS
        raise SimbriefError("simbrief_network", str(exc)) from exc

    text = raw.decode("utf-8", "replace").lstrip("\ufeff").strip()
    if not text.startswith("{"):
        # 用户不存在 / 没有计划时，接口回 XML 或纯文本错误
        low = text.lower()
        if any(k in low for k in ("not found", "no flight", "error", "invalid")):
            raise SimbriefError("simbrief_no_plan", text[:200])
        raise SimbriefError("simbrief_bad_response", text[:200])
    try:
        doc = json.loads(text)
    except ValueError as exc:
        raise SimbriefError("simbrief_bad_response", str(exc)) from exc
    if not isinstance(doc, dict) or not doc.get("general"):
        raise SimbriefError("simbrief_no_plan")
    return doc


# ---------- 解析小工具 ----------

def _num(v) -> float | None:
    try:
        return float(str(v).strip())
    except (TypeError, ValueError):
        return None


def _iso_dt(v) -> datetime | None:
    """ISO8601 → 带时区的 datetime（统一折算到 UTC）。"""
    s = str(v or "").strip()
    if "T" not in s:
        return None
    try:
        dt = datetime.fromisoformat(s.replace("Z", "+00:00"))
    except ValueError:
        return None
    return dt.astimezone(timezone.utc) if dt.tzinfo else dt.replace(tzinfo=timezone.utc)


def clock_hhmm(v) -> str:
    """时刻 → `HHMM`（UTC）。支持 ISO 串 / `HH:MM` / `HMM` / `HHMM`。"""
    dt = _iso_dt(v)
    if dt:
        return f"{dt.hour:02d}{dt.minute:02d}"
    s = str(v or "").strip()
    if not s:
        return ""
    if ":" in s:
        hh, _, mm = s.partition(":")
        if hh.strip().isdigit() and mm.strip()[:2].isdigit():
            h, m = int(hh), int(mm.strip()[:2])
            if 0 <= h < 24 and 0 <= m < 60:
                return f"{h:02d}{m:02d}"
        return ""
    digits = re.sub(r"\D", "", s)
    if len(digits) in (3, 4):
        h, m = int(digits[:-2]), int(digits[-2:])
        if 0 <= h < 24 and 0 <= m < 60:
            return f"{h:02d}{m:02d}"
    return ""


def minutes_of(v) -> int | None:
    """时长 → 分钟。支持 `HH:MM` / `HHMM` / 秒 / 分钟。"""
    s = str(v or "").strip()
    if not s:
        return None
    if ":" in s:
        hh, _, mm = s.partition(":")
        n = _num(hh)
        m = _num(mm)
        if n is not None and m is not None:
            return int(n * 60 + m)
        return None
    n = _num(s)
    if n is None or n <= 0:
        return None
    # 纯数字歧义：4 位且能拆成合法时刻（0135 → 1h35）按 HHMM，否则秒/分钟
    if s.isdigit() and len(s) == 4:
        h, m = int(s[:2]), int(s[2:])
        if h <= 23 and m < 60:
            return h * 60 + m
    # 其余纯数字：>1000 视为秒（20 分钟以上才会越过该阈值），否则视为分钟
    return int(round(n / 60)) if n > 1000 else int(round(n))


def flight_level(v) -> str:
    """高度（英尺）→ `FL350`；低空保留英尺，0/非数字原样透传（空则空）。"""
    n = _num(v)
    if n is None:
        return str(v or "").strip().upper()
    if n <= 0:
        return ""
    if n >= 10000:
        return f"FL{int(round(n / 100))}"
    return str(int(n))


# ---------- 映射 ----------

def build_plan(ofp: dict) -> dict:
    """OFP JSON → 本客户端飞行计划表单字段（纯函数，便于离线测试）。"""
    g = ofp.get("general") or {}
    times = ofp.get("times") or {}
    ac = ofp.get("aircraft") or {}
    fuel = ofp.get("fuel") or {}
    orig = ofp.get("origin") or {}
    dest = ofp.get("destination") or {}
    altn = ofp.get("alternate") or {}

    # 航程：优先 est_off → est_on 相减（不受字段单位歧义影响）
    enroute_min = 0
    off, on = _iso_dt(times.get("est_off")), _iso_dt(times.get("est_on"))
    if off and on and on >= off:
        enroute_min = int((on - off).total_seconds() // 60)
    if not enroute_min:
        for key in ("est_enroute", "sched_enroute"):
            enroute_min = minutes_of(times.get(key)) or 0
            if enroute_min:
                break

    # 燃油：总油量 / 航段耗油 折算续航（$FP 的燃油位是"可用时间"而非重量）
    fuel_min = 0
    for key in ("est_fuel", "sched_fuel", "endurance"):
        fuel_min = minutes_of(times.get(key)) or 0
        if fuel_min:
            break
    if not fuel_min:
        burn = _num(fuel.get("enroute_burn"))
        ramp = _num(fuel.get("plan_ramp"))
        if burn and ramp and enroute_min:
            fuel_min = int(round(enroute_min * ramp / burn))
    if not fuel_min:
        fuel_min = enroute_min + 45

    enroute_min = min(enroute_min, _MAX_MINUTES)
    fuel_min = min(fuel_min, _MAX_MINUTES)

    reg = norm_registration(str(ac.get("reg") or ""))
    aircraft = str(ac.get("icao_code") or ac.get("icao") or "").strip().upper()
    tas = str(g.get("cruise_tas") or "").strip()
    if _num(tas) in (0, 0.0):        # 少数机型 SimBrief 给 0，视为未提供
        tas = ""
    route = re.sub(r"\s+", " ", str(g.get("route") or g.get("route_ifps") or "")).strip()
    # SimBrief 会把整个航路用 DCT 串起来，FSD 里保留即可；末尾多余 DCT 去掉
    route = re.sub(r"(?:\s+DCT)+$", "", route).strip()

    return {
        "type": str(g.get("flight_rules") or "I").strip().upper()[:1] or "I",
        "aircraft": aircraft,
        "registration": reg,
        "cruise_speed": tas,
        "cruise_alt": flight_level(g.get("initial_altitude")
                                   or g.get("cruise_altitude")),
        "dep": str(orig.get("icao_code") or "").strip().upper(),
        "dest": str(dest.get("icao_code") or "").strip().upper(),
        "alt": str(altn.get("icao_code") or "").strip().upper(),
        "plan_time": clock_hhmm(times.get("est_off") or times.get("sched_off")),
        "actual_time": "0000",          # 实际起飞时间导入时尚未发生
        "enroute_h": enroute_min // 60,
        "enroute_m": enroute_min % 60,
        "fuel_h": fuel_min // 60,
        "fuel_m": fuel_min % 60,
        "remarks": str(g.get("remarks") or "").strip(),
        "route": route,
        # 仅用于界面提示，不进入表单
        "callsign": str(g.get("callsign") or "").strip().upper(),
        "airline": str(g.get("icao_airline") or "").strip().upper(),
        "ofp_id": str(g.get("release")
                      or (ofp.get("params") or {}).get("request_id") or ""),
    }


def import_plan(username: str, timeout: float = 10.0) -> dict:
    """拉取 + 映射，返回 `build_plan` 结果（失败抛 `SimbriefError`）。"""
    return build_plan(fetch_ofp(username, timeout=timeout))
