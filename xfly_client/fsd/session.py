"""FSD 高层会话：连接握手、登录、位置上报、保活、文本/METAR/机型事件。
- 每 5s 上报位置 @ 包，每 60s 发送保活 ping
"""

from __future__ import annotations

import threading
import time
from typing import Callable, Optional

from . import packet as pk
from .auth import FsdAuthState
from .link import FsdLink, SERVER_CALLSIGN
from .parser import ParsedPacket, parse

STATE_DISCONNECTED = "Disconnected"
STATE_CONNECTING = "Connecting"
STATE_WAIT_LOGIN = "WaitLogin"
STATE_ONLINE = "Online"

POSITION_INTERVAL = 5.0
KEEPALIVE_INTERVAL = 60.0


class FsdSession:
    def __init__(self, host: str, port: int, callsign: str, cid: str,
                 credential: str, real_name: str, simulator_type: int,
                 auth_client_id: int = pk.CLIENT_ID,
                 auth_key: str = pk.AUTH_KEY) -> None:
        self._link = FsdLink(host, port)
        self.callsign = callsign
        self.cid = cid
        self.credential = credential
        self.real_name = real_name
        self.simulator_type = simulator_type
        self._auth = FsdAuthState()
        self._auth.init(auth_client_id, auth_key)

        self.aircraft_icao = "A320"
        self.airline_icao = ""
        self.livery_icao = ""

        self.state = STATE_DISCONNECTED

        self._lat = 0.0
        self._lon = 0.0
        self._alt_ft = 0
        self._gs_kt = 0
        self._corr_ft = 0
        self._pbh = 0
        self._xpdr_mode = "S"
        self._squawk = "2000"
        self._has_fix = False

        self._send_lock = threading.Lock()
        self._stop = threading.Event()
        self._reporter: Optional[threading.Thread] = None

        self._link.on_line = self._on_line
        self._link.on_disconnected = self._on_socket_down
        self._link.on_error = lambda exc: self._log(f"[fsd] {exc}")

        # 回调
        self.on_log: Optional[Callable[[str], None]] = None
        self.on_state_changed: Optional[Callable[[str], None]] = None
        self.on_error_text: Optional[Callable[[str], None]] = None
        self.on_text_received: Optional[Callable[[str, str], None]] = None
        self.on_metar_received: Optional[Callable[[str], None]] = None
        self.on_plane_info_requested: Optional[Callable[[str, str], None]] = None
        self.on_plane_info_received: Optional[Callable[
            [str, str, list[str], str], None]] = None
        self.on_flight_plan_received: Optional[Callable[
            [str, str, list[str], str], None]] = None
        self.on_position_packet: Optional[Callable[[ParsedPacket], None]] = None
        self.on_socket_down_event: Optional[Callable[[], None]] = None

    # ---------- 生命周期 ----------

    def connect(self, timeout: float = 5.0) -> None:
        self._set_state(STATE_CONNECTING)
        self._stop.clear()
        self._link.connect(timeout)
        self._log(f"[fsd] connected {self._link.host}:{self._link.port}")
        self._set_state(STATE_WAIT_LOGIN)
        self._send(pk.add_pilot(self.callsign, self.cid, self.credential,
                                pk.NETWORK_RATING, pk.PROTOCOL_REVISION,
                                self.simulator_type, self.real_name))

    def start_reporting(self) -> None:
        self._set_state(STATE_ONLINE)
        self._reporter = threading.Thread(
            target=self._report_loop, name="fsd-reporter", daemon=True)
        self._reporter.start()

    def disconnect(self, wait: float = 0.2) -> None:
        try:
            if self._link.connected:
                self._send(pk.delete_pilot(self.callsign, self.cid))
                time.sleep(wait)
        except Exception:
            pass
        self._stop.set()
        self._link.close()
        self._set_state(STATE_DISCONNECTED)

    # ---------- 发送 ----------

    def _send(self, line: str) -> None:
        if not self._link.connected:
            return
        with self._send_lock:
            try:
                self._link.send(line)
                self._log(f"> {line}")
            except Exception as exc:
                self._log(f"[fsd] send failed: {exc}")

    def send_text(self, to: str, message: str) -> None:
        self._send(pk.text_message(self.callsign, to, message))

    def send_frequency_text(self, mhz: float, message: str) -> None:
        self._send(pk.text_message(self.callsign, pk.freq_to_target(mhz), message))

    def request_metar(self, station: str) -> None:
        self._send(pk.metar_request(self.callsign, station))

    def send_plane_info(self, to: str) -> None:
        self._send(pk.plane_info(self.callsign, to, self.aircraft_icao,
                                 self.airline_icao, self.livery_icao))

    def send_plane_info_fsi(self, to: str) -> None:
        model = f"{self.airline_icao} {self.aircraft_icao}".strip()
        self._send(pk.plane_info_fsi(self.callsign, to, self.aircraft_icao,
                                     self.airline_icao, model))

    def send_plane_info_request(self, to: str) -> None:
        self._send(pk.plane_info_request(self.callsign, to))

    # ---------- 飞行计划 ----------

    def send_flight_plan(self, flight_type: str = "I",
                         aircraft: str | None = None,
                         cruise_speed: str = "450",
                         dep_airport: str = "", plan_time: str = "0000",
                         actual_time: str = "0000", cruise_alt: str = "FL350",
                         dest_airport: str = "",
                         enroute_h: int = 0, enroute_m: int = 0,
                         fuel_h: int = 2, fuel_m: int = 0,
                         alt_airport: str = "", remarks: str = "",
                         route: str = "") -> None:
        self._send(pk.flight_plan(
            self.callsign, flight_type,
            aircraft or self.aircraft_icao, cruise_speed,
            dep_airport, plan_time, actual_time, cruise_alt,
            dest_airport, enroute_h, enroute_m, fuel_h, fuel_m,
            alt_airport, remarks, route))

    # ---------- 飞行状态 ----------

    def update_flight_state(self, lat: float, lon: float, alt_true_ft: int,
                            gs_kt: int, pitch_deg: float, bank_deg: float,
                            heading_deg: float, corr_ft: int,
                            xpdr_mode: str, squawk: str,
                            on_ground: bool = False) -> None:
        self._lat = lat
        self._lon = lon
        self._alt_ft = alt_true_ft
        self._gs_kt = gs_kt
        self._pbh = pk.encode_pbh(pitch_deg, bank_deg, heading_deg, on_ground)
        self._corr_ft = corr_ft
        self._xpdr_mode = xpdr_mode
        self._squawk = squawk
        self._has_fix = True

    # ---------- 接收 ----------

    def _on_line(self, line: str) -> None:
        self._log(f"< {line}")
        p = parse(line)

        if p.kind == "$ZC":
            if p.fields:
                resp = self._auth.respond_to_challenge(p.fields[0])
                self._send(pk.auth_response(self.callsign, SERVER_CALLSIGN, resp))

        elif p.kind == "$ER":
            code = p.type if p.type else (p.flds[0] if p.flds else "")
            desc = ":".join(p.flds[1:]) if len(p.flds) > 1 else ""
            if self.on_error_text:
                self.on_error_text(f"服务器错误 {code}: {desc}")
            self._link.close()

        elif p.kind == "$PI":
            ts = p.flds[0] if p.flds else "0"
            if p.from_:
                self._send(pk.pong(self.callsign, p.from_, ts))

        elif p.kind == "#TM":
            # #TM<from>:<to>:<message...> 消息可含冒号 → 从原始行重建
            if self.on_text_received:
                self.on_text_received(p.from_, _join_rest(p.raw, 2))

        elif p.kind == "$AR":
            # $ARSERVER:<me>:<METAR text...>（openfsd 可能以 "METAR:" 开头）
            metar = _join_rest(p.raw, 2)
            if metar.startswith("METAR:"):
                metar = metar[6:]
            if self.on_metar_received:
                self.on_metar_received(metar)

        elif p.kind == "#SB":
            if p.type in ("PIR", "FSIPIR"):
                if self.on_plane_info_requested:
                    self.on_plane_info_requested(p.from_, p.type)
            elif p.type in ("PI", "FSIPI"):
                if self.on_plane_info_received:
                    self.on_plane_info_received(p.from_, p.type, p.flds, p.raw)

        elif p.kind == "$FP":
            # $FP<from>:<to>:<type>:... 服务器下发/请求飞行计划
            if self.on_flight_plan_received:
                self.on_flight_plan_received(p.from_, p.type, p.flds, p.raw)

        if self.on_position_packet:
            self.on_position_packet(p)

    def _on_socket_down(self) -> None:
        self._stop.set()
        if self.state != STATE_DISCONNECTED:
            self._set_state(STATE_DISCONNECTED)
            self._log("[fsd] disconnected")
            if self.on_socket_down_event:
                self.on_socket_down_event()

    # ---------- 内部 ----------

    def _report_loop(self) -> None:
        next_pos = time.monotonic() + POSITION_INTERVAL
        next_ka = time.monotonic() + KEEPALIVE_INTERVAL
        while not self._stop.is_set():
            now = time.monotonic()
            if (self._has_fix and self._link.connected
                    and now >= next_pos):
                self._send(pk.pilot_position(
                    self.callsign, self._xpdr_mode, self._squawk,
                    pk.NETWORK_RATING, self._lat, self._lon, self._alt_ft,
                    self._gs_kt, self._pbh, self._corr_ft))
                next_pos = now + POSITION_INTERVAL
            if self._link.connected and now >= next_ka:
                # $PI ping 到 SERVER 保持 NAT；服务器无 ping 处理器，
                # 用静默 $PO 回显目标的文本 ping
                self._send(pk.text_message(self.callsign, SERVER_CALLSIGN, "ping"))
                next_ka = now + KEEPALIVE_INTERVAL
            time.sleep(0.5)

    def _set_state(self, state: str) -> None:
        self.state = state
        if self.on_state_changed:
            self.on_state_changed(state)

    def _log(self, msg: str) -> None:
        if self.on_log:
            self.on_log(msg)


def _join_rest(raw: str, colons_to_skip: int) -> str:
    idx = raw.find(":")
    for _ in range(1, colons_to_skip):
        if idx < 0:
            break
        idx = raw.find(":", idx + 1)
    return raw[idx + 1:] if idx >= 0 else ""
