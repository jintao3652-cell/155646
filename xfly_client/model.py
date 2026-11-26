"""网络数据模型：粘合 FSD 会话与 XConnect 插件客户端。

- 从 % 包构建管制列表，从 @/^/#SL/#ST 包构建玩家列表
- 应答 PIR/FSIPIR 机型查询，解析对方 PI/FSIPI 响应
- 将插件推送的飞行数据转发为 FSD 位置上报
- 定期把在线管制/玩家 roster 推送给插件 → CSL 渲染
- 插件断线（X-Plane 关闭）→ 自动断开 FSD 连接
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

from .fsd import packet as pk
from .fsd.parser import (
    AtcPositionData, PilotPositionData, ParsedPacket,
    try_get_atc_position, try_get_pilot_position,
)
from .fsd.session import FsdSession, STATE_DISCONNECTED, STATE_ONLINE
from .xconnect.client import AtcEntry, FlightData, PlayerEntry, XConnectClient

ATC_TIMEOUT = 45.0     # ATC 每 15s 重播 %，45s 无更新判离线
PLAYER_TIMEOUT = 30.0  # 玩家位置包通常 5s 一次
ROSTER_PUSH_INTERVAL = 3.0


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
    model_requested: bool = False
    aircraft: str = "A320"
    family: str = ""
    airline: str = ""   # 涂装/航司码（PI AIRLINE 优先，否则取呼号前三位）
    real_name: str = "" # 机长（服务器广播 #AP 时可得，否则未知）
    dep: str = ""       # 起飞机场（$FP）
    dest: str = ""      # 降落机场（$FP）


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
        self._pl_first_seen: dict[str, float] = {}  # 玩家首次出现（在线时长）
        self._model_req_at: dict[str, float] = {}   # 远程机型 PIR 请求节流
        self._fp_req_at: dict[str, float] = {}      # 飞行计划查询节流（起降机场）
        self._err_dedupe: dict[str, float] = {}     # $ER 同文案去重（防刷屏）

        self.fsd: Optional[FsdSession] = None
        # 插件客户端可注入共享实例（应用级监听插件上线，生命周期由调用方管理）
        self._owns_xconnect = xconnect is None
        self.xconnect: XConnectClient = (
            xconnect if xconnect is not None else XConnectClient())

        self.my_callsign = ""
        self.my_cid = ""
        self.aircraft_icao = "A320"
        self.airline_icao = ""
        self.livery_icao = ""

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
        session.on_log = self._log
        session.on_position_packet = self._on_fsd_packet
        session.on_plane_info_requested = self._on_plane_info_requested
        session.on_plane_info_received = self._on_plane_info_received
        session.on_flight_plan_received = self._on_flight_plan_received
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
        if self.on_fsd_state_changed is not None:
            self.on_fsd_state_changed(state)

    def _on_fsd_error_text(self, text: str) -> None:
        """服务器 $ER 提示（非致命错误不断链，仅显示；同文案 20s 内去重防刷屏）。"""
        now = time.monotonic()
        if now - self._err_dedupe.get(text, 0.0) >= 20.0:
            self._err_dedupe[text] = now
            self._log(f"[fsd] {text}")
        if self.on_fsd_error is not None:
            self.on_fsd_error(text)

    def _clear_roster(self) -> None:
        with self._gate:
            self._atc.clear()
            self._players.clear()
            self._atc_last_seen.clear()
            self._pl_last_seen.clear()
            self._pl_first_seen.clear()
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

        elif p.kind in ("@", "^", "#SL", "#ST"):
            pp = try_get_pilot_position(p)
            if pp is not None and pp.callsign != self.my_callsign:
                with self._gate:
                    tr = self._players.get(pp.callsign)
                    if tr is None:
                        tr = PlayerTrack(callsign=pp.callsign)
                        self._pl_first_seen[pp.callsign] = time.monotonic()
                    is_fast = p.kind != "@"
                    _update_from_pbh(tr, pp, overwrite_gs=not is_fast)
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
                self._pl_first_seen.pop(p.from_, None)
            changed = True

        elif p.kind == "#DA":
            with self._gate:
                self._atc.pop(p.from_, None)
                self._atc_last_seen.pop(p.from_, None)
            changed = True

        elif p.kind == "#AP":
            # #AP<呼号>:SERVER:<cid>:<pw>:<rating>:<rev>:<sim>:<真实姓名>
            # 部分服务器会把其他飞行员登录广播过来，可借此拿到"机长"姓名。
            if p.flds and p.from_ != self.my_callsign:
                real = p.flds[4].strip() if len(p.flds) > 4 else ""
                with self._gate:
                    tr = self._players.get(p.from_)
                    if tr is None:
                        tr = PlayerTrack(callsign=p.from_)
                        self._players[p.from_] = tr
                        self._pl_first_seen[p.from_] = time.monotonic()
                        changed = True
                    if real:
                        tr.real_name = real
                    self._pl_last_seen[p.from_] = time.monotonic()

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
        try:
            if resp_type == "PI" and fields:
                # Fields=[GEN, EQUIPMENT=A320, AIRLINE=..., ...]
                for kv in fields:
                    if "=" not in kv:
                        continue
                    key, _, val = kv.partition("=")
                    key = key.strip().upper()
                    val = val.strip()
                    if key == "EQUIPMENT" and len(val) >= 3:
                        equipment = val.upper()
                    elif key == "AIRLINE" and len(val) >= 3:
                        airline = val.upper()[:3]
                    elif key == "CSL":
                        family = val
            elif resp_type == "FSIPI" and len(fields) >= 9:
                # Fields=[0, airline, equipment, ..., model串]
                al = fields[1].strip().upper()[:3]
                if len(al) >= 3:
                    airline = al
                eq = fields[2].strip()
                if len(eq) >= 3:
                    equipment = eq.upper()
                model = fields[-1].strip()
                if model:
                    family = model
        except Exception as exc:
            self._log(f"[fsd] plane info parse error from {from_callsign}: {exc}")

        if not equipment:
            return
        with self._gate:
            tr = self._players.get(from_callsign)
            if tr is not None:
                tr.aircraft = equipment
                if family:
                    tr.family = family
                if airline:
                    tr.airline = airline
                self._pl_last_seen[from_callsign] = time.monotonic()
        self._log(f"[fsd] plane info from {from_callsign}: {equipment} "
                  f"({family or '-'}, {airline or '-'})")

    def _on_flight_plan_received(self, from_callsign: str, fp_type: str,
                                 flds: list[str], raw: str) -> None:
        """远程客户端飞行计划（$FP，服务器转发）。

        flds = [机型, 巡航速度, 起飞机场, 计划时间, 实际时间, 巡航高度, 目的地, ...]
        - 起降机场：用于在线列表展示；
        - 机型：仅当 PIR 未返回（仍为默认 A320）时做 CSL 匹配兜底。"""
        try:
            aircraft = _clean_plan_aircraft((flds[0] or "").strip().upper())
            dep = (flds[2] or "").strip().upper() if len(flds) > 2 else ""
            dest = (flds[6] or "").strip().upper() if len(flds) > 6 else ""
        except (IndexError, AttributeError):
            return
        with self._gate:
            tr = self._players.get(from_callsign)
            if tr is None:
                return
            if dep:
                tr.dep = dep
            if dest:
                tr.dest = dest
            if aircraft and tr.aircraft == "A320":
                tr.aircraft = aircraft   # 已有 PIR 结果则保留（更准确）
            self._pl_last_seen[from_callsign] = time.monotonic()
        self._log(f"[fsd] flight plan from {from_callsign}: "
                  f"{dep or '?'}->{dest or '?'} ({aircraft or '-'})")

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
                        self._pl_first_seen.pop(cs, None)
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
                ))

        self.xconnect.send_atc(atc_list)
        self.xconnect.send_players(self.my_cid, player_list)
        # 机型未知（保持默认 A320）的玩家：主动请求机型信息（#SB PIR），
        # 收到 PI 应答后 aircraft 更新，下一轮 roster 携带正确机型供插件选 CSL
        self._request_unknown_models()

    def _request_unknown_models(self, limit: int = 5) -> None:
        """远程玩家信息补齐（位置包不带机型/计划）：
        1) 机型：默认 A320 未知态 → #SB ... :PIR → PI:GEN EQUIPMENT（同本客户端的
           对端应答最准），并顺带 $CQ ... :SERVER:FP 查询计划机型兜底；
        2) 起降机场：计划为空 → $CQ ... :SERVER:FP:<呼号> → $FP 应答含起降场。
        分项节流：PIR 30s；FP 120s（服务器多无计划，避免 NoFlightPlan 刷屏）。
        每轮最多 limit 个目标。"""
        fsd = self.fsd
        if fsd is None or fsd.state != STATE_ONLINE:
            return
        now = time.monotonic()
        with self._gate:
            items = list(self._players.values())
        req = 0
        for tr in items:
            if req >= limit:
                break
            need_pir = tr.aircraft == "A320"
            need_fp = not (tr.dep and tr.dest)
            if not need_pir and not need_fp:
                continue
            acted = False
            if need_pir and now - self._model_req_at.get(tr.callsign, 0.0) >= 30.0:
                self._model_req_at[tr.callsign] = now
                fsd.send_plane_info_request(tr.callsign)
                acted = True
            if need_fp and now - self._fp_req_at.get(tr.callsign, 0.0) >= 120.0:
                self._fp_req_at[tr.callsign] = now
                fsd.request_flight_plan(tr.callsign)
                acted = True
            if acted:
                tr.model_requested = True
                req += 1

    # ---------- 在线列表（供前端 ATC/TEL 显示） ----------

    def online_atc(self) -> list[dict]:
        """在线管制列表快照（前端计算与本地飞机的距离）。"""
        with self._gate:
            items = list(self._atc.values())
        return [{
            "callsign": a.callsign,
            "frequency": pk.khz_to_display(a.freq_khz),
            "type": facility_name(a.facility),
            "latitude": a.lat,
            "longitude": a.lon,
            "rating": a.rating,
        } for a in items]

    def online_pilots(self) -> list[dict]:
        """在线飞行员列表快照（含起降机场与在线时长秒数）。"""
        now = time.monotonic()
        with self._gate:
            items = list(self._players.values())
            seen = dict(self._pl_first_seen)
        out = []
        for tr in items:
            first = seen.get(tr.callsign)
            out.append({
                "callsign": tr.callsign,
                "real_name": tr.real_name,
                "aircraft": tr.aircraft,
                "dep": tr.dep,
                "dest": tr.dest,
                "latitude": tr.lat,
                "longitude": tr.lon,
                "altitude": tr.alt_ft,
                "online_seconds": int(now - first) if first else 0,
            })
        return out

    # ---------- 插件飞行数据 → FSD 上报 ----------

    def _on_xconnect_flight_data(self, fd: FlightData) -> None:
        if self.on_local_flight_data:
            self.on_local_flight_data(fd)

        fsd = self.fsd
        if fsd is not None and fsd.state == STATE_ONLINE:
            corr_ft = round(fd.altitude_msl - fd.altitude_agl)
            mode = "N" if fd.squark == "N" else "S"
            squawk = f"{fd.transponder:04d}"
            # 插件无 on_ground 字段：以起落架放下近似（与 UDP 版一致）
            fsd.update_flight_state(
                fd.latitude, fd.longitude, round(fd.altitude_msl),
                round(fd.groundspeed), fd.pitch, fd.bank, fd.true_heading,
                corr_ft, mode, squawk, fd.gear_deploy > 0)
            # 模拟器机型变化 → 向服务器更新 equipment（#SB PI:GEN）
            icao = (fd.aircraft_icao or "").strip().upper()
            if icao and icao != self.aircraft_icao:
                self.aircraft_icao = icao
                fsd.aircraft_icao = icao
                fsd.send_aircraft_info()
            # 机模部件变化 → 增量机模配置 ACC 上报（$CQ:@94836）
            fsd.notify_aircraft_parts(fd.parts)

    # ---------- 工具 ----------

    def _log(self, msg: str) -> None:
        if self.on_log:
            self.on_log(msg)


def _derive_airline(tr: PlayerTrack) -> str:
    """涂装/航司码：优先用对方 PI 上报的 AIRLINE，否则取呼号前三位（大写）。"""
    if tr.airline:
        return tr.airline[:3].upper()
    cs = (tr.callsign or "")[:3].upper()
    return cs if cs.isalpha() else ""


def _update_from_pbh(tr: PlayerTrack, pp: PilotPositionData,
                     overwrite_gs: bool = True) -> None:
    tr.lat = pp.lat
    tr.lon = pp.lon
    tr.alt_ft = pp.alt_ft
    if overwrite_gs:
        tr.gs_kt = pp.gs_kt
    h, b, p, _ = pk.decode_pbh(pp.pbh)
    tr.heading = h
    tr.bank = b
    tr.pitch = p