"""网络数据模型：粘合 FSD 会话与 XConnect 插件客户端。

- 从 % 包构建管制列表，从 @/^/#SL/#ST 包构建玩家列表
- 应答 PIR/FSIPIR 机型查询，解析对方 PI/FSIPI 响应
- 将插件推送的飞行数据转发为 FSD 位置上报
- 定期把在线管制/玩家 roster 推送给插件 → CSL 渲染
- 插件断线（X-Plane 关闭）→ 自动断开 FSD 连接
"""

from __future__ import annotations

import json
import math
import threading
import time
from dataclasses import dataclass, field
from typing import Callable, Optional

from . import aircraft_db
from .aircraft_profile import AircraftParts, AircraftProfile, SOURCE_GUESS
from .fsd import packet as pk
from .fsd.parser import (
    AtcPositionData, PilotPositionData, ParsedPacket,
    try_get_atc_position, try_get_pilot_position,
)
from .fsd.session import FsdSession, STATE_DISCONNECTED, STATE_ONLINE
from .registration import (
    looks_like_registration as _looks_like_registration,
    norm_registration as _norm_registration,
    resolve_registration, source_label,
)
from .xconnect.client import AtcEntry, FlightData, PlayerEntry, XConnectClient

ATC_TIMEOUT = 45.0     # ATC 每 15s 重播 %，45s 无更新判离线
PLAYER_TIMEOUT = 30.0  # 玩家位置包通常 5s 一次
# roster 推送周期：与 #SB VI（1s）对齐；插件侧会对未变化的位置去重，
# 避免 3s 重复帧把插值速度拆成“零速→跳变→零速”，滑行看起来一卡一卡。
ROSTER_PUSH_INTERVAL = 1.0

# 远端 ACC 全量轮询：只靠增量广播会漏包，导致灯光/起落架/襟翼长期陈旧。
# 50km 内每架每 2s 一次（全部请求）；50km 外 30s 一次，每轮最多 10 个。
ACC_FULL_POLL_NEAR_S = 2.0
ACC_FULL_POLL_FAR_S = 30.0
ACC_FULL_NEAR_RADIUS_M = 50000.0
ACC_FULL_FAR_PER_TICK = 10


@dataclass
class PlayerTrack:
    callsign: str = ""
    lat: float = 0.0
    lon: float = 0.0
    alt_ft: int = 0
    gs_kt: int = 0
    pitch: float = 0.0
    bank: float = 0.0
    heading: float = 0.0
    on_ground: bool = False
    agl_ft: float = -1.0   # 遥测离地高度（^/#SL/#ST），-1 未知
    model_requested: bool = False
    info_answered: bool = False   # 已收到 PI/FSIPI 应答（不再重复请求机型）
    aircraft: str = ""      # 机型 ICAO；空 = 尚未识别（不猜默认机型，避免全员 A320）
    family: str = ""
    airline: str = ""       # 航司/涂装码（PI AIRLINE 优先）
    livery: str = ""        # PI LIVERY（swift 的涂装串，常与 AIRLINE 同为航司码）
    registration: str = ""  # 尾号（PI LIVERY/REG；本客户端把尾号放 LIVERY，swift 不提供）
    # 远端 ACC 机模配置（增量合并后的灯光/襟翼/起落架/扰流板等），下发给插件渲染
    parts: dict = field(default_factory=dict)


def _dist_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Haversine 距离（米）。"""
    r = 6371000.0
    p1 = math.radians(lat1)
    p2 = math.radians(lat2)
    dp = p2 - p1
    dl = math.radians(lon2 - lon1)
    a = (math.sin(dp * 0.5) ** 2
         + math.cos(p1) * math.cos(p2) * math.sin(dl * 0.5) ** 2)
    return 2.0 * r * math.asin(min(1.0, math.sqrt(a)))


def _merge_parts(dst: dict, src: dict) -> None:
    """把 ACC 合并进已有部件表。

    swift 契约：is_full_data=true 整块替换（对端重新上线/请求全量后），
    false/unset 才逐字段增量合并。旧实现无条件合并，对端重启后残留的
    gear/lights 会一直盖住新值 —— 这正是"灯和起落架对不上"的一类根因。
    """
    if not src:
        return
    if src.get("is_full_data") is True:
        dst.clear()
        dst.update({k: v for k, v in src.items() if k != "is_full_data"})
        return
    for k, v in src.items():
        if k == "is_full_data":
            continue
        if isinstance(v, dict) and isinstance(dst.get(k), dict):
            _merge_parts(dst[k], v)
        else:
            dst[k] = v


# 第 ② 级诊断：下发插件的部件 JSON，按呼号 30s 节流。
_PARTS_LOG_INTERVAL = 30.0
_parts_log_at: dict[str, float] = {}

_FACILITY_NAMES = {1: "FSS", 2: "DEL", 3: "GND", 4: "TWR", 5: "APP", 6: "CTR"}


def facility_name(facility: int) -> str:
    return _FACILITY_NAMES.get(facility, "OBS")


def _clean_plan_aircraft(raw: str) -> str:
    """清洗飞行计划机型字段：去尾流后缀（A350/H → A350）、去空白/非法字符、
    大写，仅保留字母数字与连字符，限 2~6 字符。"""
    acc = []
    for ch in (raw or "").upper():
        if ch == "/":
            break            # /H /M /L 尾流类别直接截断
        if ch.isalnum() or ch == "-":
            acc.append(ch)
    s = "".join(acc)
    if 2 <= len(s) <= 6:
        return s
    return ""


class NetworkModel:
    def __init__(self, xconnect: XConnectClient | None = None) -> None:
        self._gate = threading.Lock()
        self._atc: dict[str, AtcPositionData] = {}
        self._players: dict[str, PlayerTrack] = {}
        self._atc_last_seen: dict[str, float] = {}
        self._pl_last_seen: dict[str, float] = {}
        self._model_req_at: dict[str, float] = {}   # 远程机型 PIR 请求节流
        self._parts_req_at: dict[str, float] = {}   # 远端全量 ACC 请求节流

        self.fsd: Optional[FsdSession] = None
        # 插件客户端可注入共享实例（应用级监听插件上线，生命周期由调用方管理）
        self._owns_xconnect = xconnect is None
        self.xconnect: XConnectClient = (
            xconnect if xconnect is not None else XConnectClient())

        self.my_callsign = ""
        self.my_cid = ""
        self.aircraft_icao = "A320"
        # 上次因机模缺 ICAO 而推断出的机型。仅用于抑制重复日志
        # （推断每帧都会跑，但同一机型只打印一次）。
        self._last_guessed_icao = ""
        self.airline_icao = ""
        self.livery_icao = ""
        # 注册号（尾号）四级来源，见 registration.py：
        #   手动填写 > 模拟器（尾号/涂装目录）> 同机型上次值 > 空（不伪造）
        self.registration_manual = ""        # 用户在飞行计划里手填
        self.registration_cache: dict[str, str] = {}   # 机型 ICAO → 上次自动值
        self.registration = ""               # 生效尾号（随 #SB PI 广播）
        self.registration_source = ""        # manual / tailnum / livery / cache
        self.engine_count = 0                # 发动机数量（随 PI 的 ENGINES= 广播）
        self._sim_reg = ""                   # 最近一帧模拟器上报的尾号
        self._sim_reg_src = ""               # 该值的来源（tailnum / livery）
        self._sim_reg_icao = ""              # 该值对应的机型（换机后不复用）
        # 尾号变化回调：(呼号级别) (reg, src, icao) → 应用层持久化 + 刷新界面
        self.on_registration_resolved: Optional[Callable[[str, str, str], None]] = None

        self.on_log: Optional[Callable[[str], None]] = None
        self.on_roster_changed: Optional[Callable[[], None]] = None
        self.on_local_flight_data: Optional[Callable[[FlightData], None]] = None
        self.on_text_received: Optional[Callable[[str, str], None]] = None
        self.on_metar_received: Optional[Callable[[str], None]] = None
        self.on_fsd_unexpected_disconnect: Optional[Callable[[], None]] = None
        self.on_fsd_error: Optional[Callable[[str], None]] = None
        self.on_xconnect_connected: Optional[Callable[[], None]] = None
        self.on_xconnect_disconnected: Optional[Callable[[], None]] = None
        self.on_plugin_message: Optional[Callable[[dict], None]] = None
        self.on_csl_status: Optional[Callable[[dict], None]] = None
        self.on_fsd_state_changed: Optional[Callable[[str], None]] = None

        self._stop = threading.Event()
        self._gc_thread = threading.Thread(
            target=self._gc_loop, name="netmodel-gc", daemon=True)
        self._gc_thread.start()
        self._push_thread = threading.Thread(
            target=self._push_loop, name="netmodel-push", daemon=True)
        self._push_thread.start()

        self.xconnect.on_flight_data = self._on_xconnect_flight_data
        self.xconnect.on_connected = self._on_xconnect_connected
        self.xconnect.on_disconnected = self._on_xconnect_disconnected
        self.xconnect.on_message = self._on_plugin_message
        self.xconnect.on_csl_status = self._on_csl_status
        self.xconnect.on_log = self._log

    # ---------- 生命周期 ----------

    def start_xconnect(self, host: str | None = None, port: int | None = None) -> None:
        if host:
            self.xconnect.host = host
        if port:
            self.xconnect.port = port
        self.xconnect.start()

    def _on_xconnect_connected(self) -> None:
        self._log("[xconnect] 数据通道就绪")
        if self.on_xconnect_connected:
            self.on_xconnect_connected()

    def _on_plugin_message(self, doc: dict) -> None:
        """插件原始报文透传（供调试模式显示）。"""
        if self.on_plugin_message:
            self.on_plugin_message(doc)

    def _on_csl_status(self, doc: dict) -> None:
        """CSL 加载状态透传。"""
        if self.on_csl_status:
            self.on_csl_status(doc)

    def _on_xconnect_disconnected(self) -> None:
        self._log("[xconnect] 未发现模拟机")
        if self.on_xconnect_disconnected:
            self.on_xconnect_disconnected()
        # 插件离线（X-Plane 关闭）→ 自动断开 FSD 连接
        if self.fsd is not None and self.fsd.state != STATE_DISCONNECTED:
            self._log("[fsd] 插件离线，自动断开 FSD 连接")
            threading.Thread(target=self._auto_disconnect_fsd,
                             daemon=True).start()

    def _auto_disconnect_fsd(self) -> None:
        self.stop_fsd()
        if self.on_fsd_unexpected_disconnect:
            self.on_fsd_unexpected_disconnect()

    def start_fsd(self, host: str, port: int, callsign: str, cid: str,
                  password: str, real_name: str, sim_type: int) -> None:
        self.stop_fsd()
        self.my_callsign = callsign
        self.my_cid = cid

        session = FsdSession(host, port, callsign, cid, password,
                             real_name, sim_type)
        session.aircraft_icao = self.aircraft_icao
        session.airline_icao = self.airline_icao
        session.livery_icao = self.livery_icao
        session.registration = self.registration
        session.engines = self.engine_count
        # 重置发动机数缓存：让插件首帧数据必然触发一次 PI 重发。
        # _on_xconnect_flight_data 里靠 "eng != self.engine_count" 判定变化，
        # 若这里保留上次会话的值（2），首帧 2 == 2 不成立 → ENGINES= 永远不发，
        # 服务器侧 aircraft.engines 就一直是 null。归零后首帧必然是 0→2 变化。
        self.engine_count = 0
        session.engines = 0
        session.on_log = self._log
        session.on_position_packet = self._on_fsd_packet
        session.on_plane_info_requested = self._on_plane_info_requested
        session.on_plane_info_received = self._on_plane_info_received
        session.on_flight_plan_received = self._on_flight_plan_received
        session.on_aircraft_config_received = self._on_aircraft_config_received
        session.on_text_received = lambda f, m: self.on_text_received and \
            self.on_text_received(f, m)
        session.on_metar_received = lambda m: self.on_metar_received and \
            self.on_metar_received(m)
        session.on_socket_down_event = self._on_fsd_socket_down
        session.on_state_changed = self._on_fsd_state_change
        session.on_error_text = self._on_fsd_error_text

        self.fsd = session
        session.connect()
        session.start_reporting()

    def stop_fsd(self) -> None:
        if self.fsd is not None:
            session, self.fsd = self.fsd, None
            session.on_position_packet = None
            session.on_plane_info_requested = None
            session.on_plane_info_received = None
            session.disconnect()
        self._clear_roster()

    def shutdown(self) -> None:
        self._stop.set()
        self.stop_fsd()
        if self._owns_xconnect:
            self.xconnect.stop()

    def _on_fsd_socket_down(self) -> None:
        if self.fsd is not None:
            self._clear_roster()
            if self.on_fsd_unexpected_disconnect:
                self.on_fsd_unexpected_disconnect()

    def _on_fsd_state_change(self, state: str) -> None:
        # 刚变为 ONLINE：主动补发一次完整 PI（机型/航司/尾号/发动机数）。
        # 必须主动发 —— _on_xconnect_flight_data 里只在"值变化"时才发 PI
        # （changed 判定）。若连上时 engine_count 已经是最终值（插件早就在推送、
        # 或复用上次状态），变化永远不会发生 → 服务器侧 aircraft.engines 恒为
        # null。实测就是这么漏的：登录后 aircraft.engines 一直是 null。
        if state == STATE_ONLINE:
            fsd = self.fsd
            if fsd is not None:
                fsd.send_aircraft_info()
        if self.on_fsd_state_changed is not None:
            self.on_fsd_state_changed(state)

    def _on_fsd_error_text(self, text: str) -> None:
        """服务器 $ER 提示（非致命错误不再断链，仅显示）。"""
        self._log(f"[fsd] {text}")
        if self.on_fsd_error is not None:
            self.on_fsd_error(text)

    def _clear_roster(self) -> None:
        with self._gate:
            self._atc.clear()
            self._players.clear()
            self._atc_last_seen.clear()
            self._pl_last_seen.clear()
            self._parts_req_at.clear()
        if self.on_roster_changed:
            self.on_roster_changed()
        self.push_roster()  # 立即通知插件清空场景

    # ---------- FSD 包处理 ----------

    def _on_fsd_packet(self, p: ParsedPacket) -> None:
        changed = False

        if p.kind == "%":
            atc = try_get_atc_position(p)
            if atc is not None:
                with self._gate:
                    self._atc[atc.callsign] = atc
                    self._atc_last_seen[atc.callsign] = time.monotonic()
                changed = True

        elif p.kind in ("@", "^", "#SL", "#ST") or (
                p.kind == "#SB" and p.type == "VI"):
            pp = try_get_pilot_position(p)
            if pp is not None and pp.callsign != self.my_callsign:
                with self._gate:
                    tr = self._players.get(pp.callsign)
                    if tr is None:
                        tr = PlayerTrack(callsign=pp.callsign)
                    # @ 与 #SB VI 携带地速；^/#SL/#ST 只有位置/姿态，保留上次值
                    overwrite_gs = p.kind in ("@", "#SB")
                    _update_from_pbh(tr, pp, overwrite_gs=overwrite_gs)
                    self._players[pp.callsign] = tr
                    self._pl_last_seen[pp.callsign] = time.monotonic()
                    if not tr.model_requested:
                        tr.model_requested = True
                        fsd = self.fsd
                        if fsd is not None:
                            threading.Thread(
                                target=fsd.send_plane_info_request,
                                args=(pp.callsign,), daemon=True).start()
                changed = True

        elif p.kind == "#DP":
            with self._gate:
                self._players.pop(p.from_, None)
                self._pl_last_seen.pop(p.from_, None)
            changed = True

        elif p.kind == "#DA":
            with self._gate:
                self._atc.pop(p.from_, None)
                self._atc_last_seen.pop(p.from_, None)
            changed = True

        elif p.kind == "#AP":
            if p.flds and p.from_ != self.my_callsign:
                with self._gate:
                    if p.from_ not in self._players:
                        self._players[p.from_] = PlayerTrack(callsign=p.from_)
                        self._pl_last_seen[p.from_] = time.monotonic()
                        changed = True

        elif p.kind == "#AA":
            with self._gate:
                self._atc_last_seen[p.from_] = time.monotonic()

        elif p.kind == "$!!":
            reason = ":".join(p.flds)
            self._log(f"[fsd] killed by admin: {reason}")

        if changed and self.on_roster_changed:
            self.on_roster_changed()

    # ---------- 机型信息 ----------

    def _on_plane_info_requested(self, from_callsign: str, req_type: str) -> None:
        fsd = self.fsd
        if fsd is None:
            return
        if req_type == "FSIPIR":
            fsd.send_plane_info_fsi(from_callsign)
        else:
            fsd.send_plane_info(from_callsign)
        self._log(f"[fsd] plane info request from {from_callsign} ({req_type}) -> answered")

    def _on_plane_info_received(self, from_callsign: str, resp_type: str,
                                fields: list[str], raw: str) -> None:
        equipment = ""
        family = ""
        airline = ""
        livery = ""
        liv_raw = ""
        registration = ""
        try:
            if resp_type == "PI" and fields:
                # Fields=[GEN, EQUIPMENT=A320, AIRLINE=CCA, LIVERY=CCA, ...]
                if fields[0].strip().upper() == "X":
                    return          # 旧版 PI 格式（swift 亦直接忽略）
                for kv in fields:
                    if "=" not in kv:
                        continue
                    key, _, val = kv.partition("=")
                    key = key.strip().upper()
                    val = val.strip()
                    if key == "EQUIPMENT":
                        equipment = _clean_plan_aircraft(val)
                    elif key == "AIRLINE":
                        airline = _norm_airline_code(val)
                    elif key == "LIVERY":
                        # 本客户端发的是尾号（B-1234），swift 发的是航司码或
                        # 内部映射 ID（swift_a9742），留到循环后统一判定
                        liv_raw = val
                    elif key == "REG":
                        # 本客户端扩展键：明确声明是尾号，优先于 LIVERY 猜测
                        registration = _norm_registration(val)
                    elif key == "CSL":
                        family = val
            elif resp_type == "FSIPI" and len(fields) >= 3:
                # Fields=[0, airline, aircraft, ..., model串]
                airline = _norm_airline_code(fields[1])
                equipment = _clean_plan_aircraft(fields[2])
                if len(fields) > 3:
                    # 末位是 FSInn 的描述串（"DLH A359 B-1234"）：其中的尾号
                    # 可当注册号，整串是单个 3~5 位机型/包名时才当 CSL 包提示
                    model = fields[-1].strip().upper()
                    if model:
                        if " " not in model and 3 <= len(model) <= 5 \
                                and model.isalnum():
                            family = model
                        else:
                            for tok in model.replace("/", " ").split():
                                if _looks_like_registration(tok):
                                    registration = _norm_registration(tok)
                                    break
        except Exception as exc:
            self._log(f"[fsd] plane info parse error from {from_callsign}: {exc}")

        # LIVERY 的语义判定：先看 REG= 是否已给出；否则按形状判断——含数字的
        # 长串（B-1234 / N829PL）当尾号，纯字母短码（DLH/CCA）当涂装码
        if not registration and liv_raw and _looks_like_registration(liv_raw):
            registration = _norm_registration(liv_raw)
            liv_raw = ""
        if liv_raw:
            livery = _norm_airline_code(liv_raw)

        if not equipment and not airline and not registration:
            return
        with self._gate:
            tr = self._players.get(from_callsign)
            if tr is not None:
                if equipment:
                    tr.aircraft = equipment
                if family:
                    tr.family = family
                if airline:
                    tr.airline = airline
                if livery:
                    tr.livery = livery
                if registration:
                    tr.registration = registration
                tr.info_answered = True
                self._pl_last_seen[from_callsign] = time.monotonic()
        self._log(f"[fsd] plane info from {from_callsign}: {equipment or '-'} "
                  f"({family or '-'}, {airline or '-'}, livery={livery or '-'}, "
                  f"reg={registration or '-'})")

    def _on_aircraft_config_received(self, sender: str, config: dict) -> None:
        """远端机模配置（$CQ:@94836:ACC）：合并部件，供 CSL 渲染灯光/襟翼等。

        插件之前把远端灯光硬编码成全亮（因为收不到遥测），现在把对方 ACC 里的
        lights / flaps_pct / gear_down / spoilers_out 合并后随 roster 下发。
        """
        if not sender or not isinstance(config, dict):
            return
        full = bool(config.get("is_full_data"))
        data = {k: v for k, v in config.items() if k != "is_full_data"}
        with self._gate:
            tr = self._players.get(sender)
            if tr is None:
                return          # 尚未收到过位置：不创建"幽灵"玩家
            if full:
                tr.parts = data
            else:
                _merge_parts(tr.parts, data)
            self._pl_last_seen[sender] = time.monotonic()

    def _on_flight_plan_received(self, from_callsign: str, fp_type: str,
                                 flds: list[str], raw: str) -> None:
        """远程客户端飞行计划（$FP，服务器转发）→ 机型兜底匹配。

        仅当 PI/PIR 未给出机型（aircraft 为空）时使用计划中的 aircraft 字段做
        CSL 匹配；已有 PI 结果则保留（PI 带涂装、更准确）。"""
        try:
            aircraft = (flds[0] or "").strip().upper()
        except (IndexError, AttributeError):
            return
        aircraft = _clean_plan_aircraft(aircraft)
        if not aircraft:
            return
        with self._gate:
            tr = self._players.get(from_callsign)
            if tr is None:
                return
            if tr.aircraft:
                return  # 已有机型信息（PIR 应答/PI），不覆盖
            tr.aircraft = aircraft
            self._pl_last_seen[from_callsign] = time.monotonic()
        self._log(f"[fsd] flight plan aircraft from {from_callsign}: {aircraft}")

    # ---------- 后台循环 ----------

    def _gc_loop(self) -> None:
        while not self._stop.is_set():
            time.sleep(5.0)
            changed = False
            now = time.monotonic()
            with self._gate:
                for cs in list(self._atc_last_seen):
                    if now - self._atc_last_seen[cs] > ATC_TIMEOUT:
                        self._atc.pop(cs, None)
                        self._atc_last_seen.pop(cs, None)
                        changed = True
                for cs in list(self._pl_last_seen):
                    if now - self._pl_last_seen[cs] > PLAYER_TIMEOUT:
                        self._players.pop(cs, None)
                        self._pl_last_seen.pop(cs, None)
                        changed = True
            if changed and self.on_roster_changed:
                self.on_roster_changed()

    # ---------- roster 推送（→ 插件渲染 CSL） ----------

    def _push_loop(self) -> None:
        while not self._stop.is_set():
            time.sleep(ROSTER_PUSH_INTERVAL)
            try:
                self.push_roster()
            except Exception as exc:
                self._log(f"[xconnect] roster 推送失败: {exc}")

    def push_roster(self) -> None:
        if not self.xconnect.connected:
            return
        with self._gate:
            atc_list: list[AtcEntry] = []
            for a in self._atc.values():
                atc_list.append(AtcEntry(
                    callsign=a.callsign,
                    frequency=pk.khz_to_display(a.freq_khz),
                    type=facility_name(a.facility),
                    latitude=a.lat,
                    longitude=a.lon,
                    rating=a.rating,
                ))
            atc_callsigns = {a.callsign.lower() for a in atc_list}

            player_list: list[PlayerEntry] = []
            pseen = {}
            for tr in self._players.values():
                pseen.setdefault(tr.callsign.lower(), tr)
            for tr in pseen.values():
                if tr.callsign.lower() in atc_callsigns:
                    continue  # 剔除与管制员同名的条目，防插件双重渲染
                # 统一中间层：机型档案决定引擎数/类型/起落架类别，
                # canonical 部件负责三态解析与兜底；插件不再按单机写死。
                raw_parts = tr.parts if isinstance(tr.parts, dict) else {}
                measured = len(raw_parts.get("engines") or {})
                profile = AircraftProfile.from_designator(
                    tr.aircraft, measured_engines=measured)
                if raw_parts:
                    cp = AircraftParts.from_raw(
                        raw_parts, source=raw_parts.get("parts_source") or SOURCE_GUESS)
                    eff = cp.effective(profile)
                    parts = dict(raw_parts)
                    parts["engines"] = {
                        str(i): {"on": bool(on)}
                        for i, on in eff["engines"].items()
                    }
                    if eff["gear_down"] is not None:
                        parts["gear_down"] = eff["gear_down"]
                else:
                    parts = {}
                parts["engine_count"] = profile.engines or measured
                parts["engine_type"] = profile.engine_type.value
                parts["gear_type"] = profile.gear
                parts["parts_source"] = (
                    raw_parts.get("parts_source") or
                    (SOURCE_GUESS if raw_parts else "not_set"))
                if parts and (time.monotonic() -
                              _parts_log_at.get(tr.callsign, 0.0)
                              > _PARTS_LOG_INTERVAL):
                    _parts_log_at[tr.callsign] = time.monotonic()
                    self._log(
                        f"[parts] roster {tr.callsign}: "
                        f"{json.dumps(parts, separators=(',', ':'), ensure_ascii=False)}")
                # ACC 的 on_ground 比 PBH 位更可信（部分客户端 PBH 地面位恒 0）
                og = raw_parts.get("on_ground", tr.on_ground)
                player_list.append(PlayerEntry(
                    callsign=tr.callsign,
                    latitude=tr.lat,
                    longitude=tr.lon,
                    altitude=float(tr.alt_ft),
                    pitch=tr.pitch,
                    bank=tr.bank,
                    heading=tr.heading,
                    ground_speed=float(tr.gs_kt),
                    aircraft=tr.aircraft,
                    aircraft_family=tr.family,
                    airline=_derive_airline(tr),
                    registration=tr.registration,
                    on_ground=bool(og),
                    altitude_agl=tr.agl_ft,
                    parts=parts,
                ))

        self.xconnect.send_atc(atc_list)
        self.xconnect.send_players(self.my_cid, player_list)
        # 机型/涂装未知的玩家：PIR + FSIPIR 双请求（swift / FSInn 两系客户端），
        # 收到 PI/FSIPI 应答后 aircraft/airline 更新，下一轮 roster 携带正确
        # 机型与涂装供插件选 CSL
        self._request_unknown_models()

    def _poll_aircraft_configs(self, items, now: float) -> int:
        """50km 内每架每 2s 请求全量 ACC；50km 外 30s 一次（本轮最多 10 个）。

        近距离全部请求，不做本轮限流（用户要求 50km 内都是 2s）。
        分开成独立方法便于单测；调用方已确认 FSD online。
        """
        fsd = self.fsd
        if fsd is None or fsd.state != STATE_ONLINE:
            return 0
        own_lat = float(getattr(fsd, "_lat", 0.0) or 0.0)
        own_lon = float(getattr(fsd, "_lon", 0.0) or 0.0)
        have_pos = abs(own_lat) > 1e-6 or abs(own_lon) > 1e-6

        near = []
        far = []
        for tr in items:
            if have_pos and _dist_m(own_lat, own_lon,
                                    tr.lat, tr.lon) <= ACC_FULL_NEAR_RADIUS_M:
                near.append(tr)
            else:
                far.append(tr)

        def send(tr) -> None:
            self._parts_req_at[tr.callsign] = now
            try:
                fsd.request_aircraft_config(tr.callsign)
            except Exception:
                pass

        sent = 0
        for tr in near:
            if (now - self._parts_req_at.get(tr.callsign, 0.0)
                    < ACC_FULL_POLL_NEAR_S):
                continue
            send(tr)
            sent += 1

        far_sent = 0
        for tr in far:
            if far_sent >= ACC_FULL_FAR_PER_TICK:
                break
            if (now - self._parts_req_at.get(tr.callsign, 0.0)
                    < ACC_FULL_POLL_FAR_S):
                continue
            send(tr)
            far_sent += 1
            sent += 1
        return sent

    def _request_unknown_models(self, limit: int = 5) -> None:
        """远程玩家机型/涂装识别（FSD 位置包不带机型，需外部补齐）：
        1) #SB ...:PIR → PI:GEN EQUIPMENT/AIRLINE/LIVERY 应答（swift/xPilot 系，
           精确度最高，同时给出涂装）；
        2) #SB ...:FSIPIR → FSIPI 应答（FSInn 系客户端）；
        3) $CQ:...:SERVER:FP:<呼号> → $FP 应答含计划机型（仅机型，无涂装）。
        每目标 30s 节流，每轮最多 limit 个；已收到 PI 应答的不再重复请求
        （即使对方没报涂装，也避免无休止刷请求）。"""
        fsd = self.fsd
        if fsd is None or fsd.state != STATE_ONLINE:
            return
        now = time.monotonic()
        with self._gate:
            items = list(self._players.values())
        self._poll_aircraft_configs(items, now)
        req = 0
        for tr in items:
            if req >= limit:
                break
            if tr.aircraft and tr.info_answered:
                continue
            if now - self._model_req_at.get(tr.callsign, 0.0) < 30.0:
                continue  # 30s 内已请求过，不重复骚扰
            self._model_req_at[tr.callsign] = now
            tr.model_requested = True
            req += 1
            if not tr.aircraft:
                fsd.request_flight_plan(tr.callsign)
            fsd.send_plane_info_request(tr.callsign)
            fsd.send_plane_info_request_fsi(tr.callsign)

    # ---------- 插件飞行数据 → FSD 上报 ----------

    def _on_xconnect_flight_data(self, fd: FlightData) -> None:
        # 机型推断要在推送前做：前端工具栏显示的 `aircraft_icao` 直接取自
        # 这里，而有些机模（F-14 等）插件读到的是空串。若只把插件原值推给
        # 前端，工具栏会一直显示**上一架飞机**的机型（实测：切 F-14 后
        # 仍显示 MD82）。所以先把推断结果写回 fd.aircraft_icao。
        raw_icao = (fd.aircraft_icao or "").strip().upper()
        if not raw_icao:
            guessed = aircraft_db.guess_designator(
                fd.aircraft_name or "", int(fd.engine_count or 0))
            if guessed:
                fd.aircraft_icao = guessed
                if guessed != self._last_guessed_icao:
                    self._last_guessed_icao = guessed
                    self._log(f"机型推断：\"{fd.aircraft_name}\" → {guessed}"
                              f"（机模未提供 ICAO，按 swift 机型库匹配）")
        else:
            self._last_guessed_icao = ""

        if self.on_local_flight_data:
            self.on_local_flight_data(fd)

        fsd = self.fsd
        if fsd is not None and fsd.state == STATE_ONLINE:
            # @ 包字段 9 = 气压高 − 真高（swift PilotDataUpdate::toTokens）。
            # 有座舱高度表（指示高度）时用它推算；缺失则退化为 0。
            corr_ft = 0
            if fd.altitude_indicated > 0.0:
                corr_ft = round(fd.altitude_indicated - fd.altitude_msl)
            # 应答机模式字母（FSD 只认三个）：
            #   S = Standby（待机，不发射）
            #   N = Normal（Mode A/C/S 正常发射）
            #   Y = Ident（瞬时脉冲，不发）
            # 插件侧已按 swift 规则折算（XP mode 0/1→S，其余→N），这里只做
            # 白名单校验，避免任何非 S/N 的值混进报文被服务器当成 S。
            # 之前写成 `"N" if squark == "N" else "S"` —— 虽然结果一样，
            # 但会把插件发来的 Y 静默改判成 S，语义不清晰。
            mode = fd.squark if fd.squark in ("S", "N", "Y") else "S"
            squawk = f"{fd.transponder:04d}"
            # onGround：优先机模部件真实标志（sim/flightmodel/failures/
            # onground_any，插件已读好放在 parts.on_ground）。
            #
            # 兜底**不能用"起落架放下"**：巡航中放下起落架会被对端误判为在地面，
            # 进而被按地形高度吸附（这是之前踩过的坑）。改用 AGL 高度兜底：
            # 离地 15ft 内算地面。gear_deploy 只在最后做兜底，且仅当 AGL
            # 不可用（部分快速包不带 AGL）。
            parts = fd.parts or {}
            if "on_ground" in parts:
                on_ground = bool(parts.get("on_ground"))
            elif fd.altitude_agl > 0.0:
                on_ground = fd.altitude_agl < 15.0
            else:
                on_ground = fd.gear_deploy > 0.9
            fsd.update_flight_state(
                fd.latitude, fd.longitude, round(fd.altitude_msl),
                round(fd.groundspeed), fd.pitch, fd.bank, fd.true_heading,
                corr_ft, mode, squawk, on_ground,
                alt_agl_ft=fd.altitude_agl, vs_fpm=fd.vertical_speed,
                gear_deploy=fd.gear_deploy)
            # 模拟器机型变化 → 向服务器更新 equipment（#SB PI:GEN）
            #
            # icao 已在函数开头做过推断填充（见 _on_xconnect_flight_data 起始处），
            # 这里只负责与上次值比较并触发重发。
            #
            # ⚠️ **空值也要参与比较**：原来的写法 `if icao and icao != ...`
            # 在 icao 为空时直接跳过，导致 self.aircraft_icao 保留上一架飞机的
            # 值，而尾号是独立更新的 → 服务器上出现 "equipment: MD82" +
            # "livery: N988DR(F-14)" 这种机型与尾号不匹配的脏数据（实测踩过）。
            icao = (fd.aircraft_icao or "").strip().upper()
            changed = False
            if icao != self.aircraft_icao:
                self.aircraft_icao = icao
                fsd.aircraft_icao = icao
                changed = True
            # 发动机数量（PI 的 ENGINES= 扩展键）：换机模时数量会变，需重发
            eng = int(fd.engine_count or 0)
            if eng > 0 and eng != self.engine_count:
                self.engine_count = eng
                fsd.engines = eng
                changed = True
            reg_changed = self._apply_registration(
                fd.registration, fd.registration_src, icao)
            if changed or reg_changed:
                fsd.send_aircraft_info()
            # 机模部件变化 → 增量机模配置 ACC 上报（$CQ:@94836）
            fsd.notify_aircraft_parts(fd.parts)

    # ---------- 注册号（尾号） ----------

    def set_registration_manual(self, manual: str) -> None:
        """用户在飞行计划里填写/清空尾号：立即生效（清空 = 回到自动识别）。"""
        manual = _norm_registration(manual)
        if manual == self.registration_manual:
            return
        self.registration_manual = manual
        # 清空时立刻回落到模拟器最近上报的值（不必等下一帧数据）
        sim = self._sim_reg if self._sim_reg_icao == self.aircraft_icao else ""
        sim_src = self._sim_reg_src if sim else ""
        changed = self._apply_registration(sim, sim_src, self.aircraft_icao)
        fsd = self.fsd
        if changed and fsd is not None and fsd.state == STATE_ONLINE:
            fsd.send_aircraft_info()

    def _apply_registration(self, fsd_reg: str, sim_src: str, icao: str) -> bool:
        """按四级优先级重算生效尾号 → 返回是否变化（调用方决定是否重发 PI）。

        `icao` 为模拟器当前机型，用于「同机型上次值」缓存键；空串表示未知机型，
        此时不使用缓存（避免给刚换的机型套上一个无关尾号）。
        """
        icao = (icao or "").strip().upper()
        key = icao or self.aircraft_icao
        if icao and _norm_registration(fsd_reg):
            # 记住模拟器最近一次上报值：用户清空手填时立刻回落到它
            self._sim_reg = _norm_registration(fsd_reg)
            self._sim_reg_src = sim_src
            self._sim_reg_icao = icao
        cached = self.registration_cache.get(key, "") if icao else ""
        reg, src = resolve_registration(
            self.registration_manual, fsd_reg, cached, sim_src)
        if reg and src in ("tailnum", "livery", "sim") and icao:
            # 记入缓存（仅模拟器读到的值；手动值不入缓存，避免清空后仍被回填）
            if self.registration_cache.get(icao, "") != reg:
                self.registration_cache[icao] = reg
                self._log(f"[fsd] 注册号记录 {icao} -> {reg}")
        if reg == self.registration and src == self.registration_source:
            return False
        old, self.registration, self.registration_source = (
            self.registration, reg, src)
        fsd = self.fsd
        if fsd is not None:
            fsd.registration = reg
        self._log(f"[fsd] 注册号 {reg or '—'}（{source_label(src)}）"
                  + (f"，原 {old}" if old and old != reg else ""))
        if self.on_registration_resolved:
            try:
                self.on_registration_resolved(reg, src, key)
            except Exception:
                pass
        return True

    # ---------- 工具 ----------

    def _log(self, msg: str) -> None:
        if self.on_log:
            self.on_log(msg)


def _derive_airline(tr: PlayerTrack) -> str:
    """涂装/航司码：PI AIRLINE → PI LIVERY（可当变体码时）→ 呼号前三位。"""
    if tr.airline:
        return tr.airline[:4].upper()
    if tr.livery:
        return tr.livery[:4].upper()
    cs = (tr.callsign or "")[:3].upper()
    return cs if cs.isalpha() else ""


def _norm_airline_code(raw: str) -> str:
    """航司/涂装码归一化：只接受能确认是"码"的串，否则返回空交给上层回退。

    接受：
      - 整串 2~4 位字母数字（"DLH" / "CCA" / "BA"）；
      - "CCA (Air China)" 这种"码 + 括注"（取括号前的码）；
      - "CCA-1" / "CCA_1" 这种"码 + 分隔符后缀"（取分隔符前的码）。
    拒绝：
      - 描述串（"Air China" / "British Airways"）——首词会变成 AIR/BRI 这类
        伪码，宁可回退到呼号前三位的航空码；
      - swift 的内部 CSL 映射 ID（"swift_a9742"）——它不是航司/涂装码。
    """
    s = (raw or "").strip().upper()
    if not s:
        return ""
    if any(ch.isspace() for ch in s):
        head, _, rest = s.partition(" ")
        # 只有"码 + 括注"（CCA (Air China)）才认，纯描述串一律丢弃
        if not rest.lstrip().startswith("("):
            return ""
        s = head
    for sep in ("_", "-", "/"):
        if sep in s:
            s = s.split(sep, 1)[0]
            break
    return s if 2 <= len(s) <= 4 and s.isalnum() else ""


def _update_from_pbh(tr: PlayerTrack, pp: PilotPositionData,
                     overwrite_gs: bool = True) -> None:
    tr.lat = pp.lat
    tr.lon = pp.lon
    tr.alt_ft = pp.alt_ft
    if overwrite_gs:
        tr.gs_kt = pp.gs_kt
    h, b, p, on_ground = pk.decode_pbh(pp.pbh)
    tr.heading = h
    tr.bank = b
    tr.pitch = p
    # onGround 直接用遥测位（PBH bit1）；AGL 仅在快速包携带
    tr.on_ground = pp.on_ground or on_ground
    tr.agl_ft = pp.agl_ft
