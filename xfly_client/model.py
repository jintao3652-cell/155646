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


_FACILITY_NAMES = {1: "FSS", 2: "DEL", 3: "GND", 4: "TWR", 5: "APP", 6: "CTR"}


def facility_name(facility: int) -> str:
    return _FACILITY_NAMES.get(facility, "OBS")


class NetworkModel:
    def __init__(self, xconnect: XConnectClient | None = None) -> None:
        self._gate = threading.Lock()
        self._atc: dict[str, AtcPositionData] = {}
        self._players: dict[str, PlayerTrack] = {}
        self._atc_last_seen: dict[str, float] = {}
        self._pl_last_seen: dict[str, float] = {}

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
        self.on_xconnect_connected: Optional[Callable[[], None]] = None
        self.on_xconnect_disconnected: Optional[Callable[[], None]] = None
        self.on_plugin_message: Optional[Callable[[dict], None]] = None
        self.on_csl_status: Optional[Callable[[dict], None]] = None

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
        session.on_text_received = lambda f, m: self.on_text_received and \
            self.on_text_received(f, m)
        session.on_metar_received = lambda m: self.on_metar_received and \
            self.on_metar_received(m)
        session.on_socket_down_event = self._on_fsd_socket_down

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

    def _clear_roster(self) -> None:
        with self._gate:
            self._atc.clear()
            self._players.clear()
            self._atc_last_seen.clear()
            self._pl_last_seen.clear()
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
                    elif key == "CSL":
                        family = val
            elif resp_type == "FSIPI" and len(fields) >= 9:
                # Fields=[0, airline, equipment, ..., model串]
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
                self._pl_last_seen[from_callsign] = time.monotonic()
        self._log(f"[fsd] plane info from {from_callsign}: {equipment} ({family or '-'})")

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
                    frequency=pk.fsd_freq_to_display(a.freq_khz),
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
                ))

        self.xconnect.send_atc(atc_list)
        self.xconnect.send_players(self.my_cid, player_list)

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

    # ---------- 工具 ----------

    def _log(self, msg: str) -> None:
        if self.on_log:
            self.on_log(msg)


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