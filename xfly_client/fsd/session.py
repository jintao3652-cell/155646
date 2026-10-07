"""FSD 高层会话：连接握手、登录、位置上报、保活、文本/METAR/机型事件。
- 每 5s 上报位置 @ 包，每 60s 发送保活 ping
"""

from __future__ import annotations

import json
import math
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

# 机模配置 ACC 上报（swift CTokenBucket 同参数）
ACFG_CAPACITY = 10          # 桶容量（突发上限）
ACFG_REFILL_INTERVAL = 5.0  # 每 5s 补 1 token（≈1 条/5s）
ACFG_BROADCAST_TO = "@94836"


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
        self.on_aircraft_config_received: Optional[Callable[[str, dict], None]] = None

        # 机模配置 ACC：上次发送（全量基准）、令牌桶、全量快照
        self._acfg_last_sent: Optional[dict] = None
        self._acfg_full: Optional[dict] = None
        self._acfg_tokens = ACFG_CAPACITY
        self._acfg_refill_at = time.monotonic()

    # ---------- 生命周期 ----------

    def connect(self, timeout: float = 5.0) -> None:
        self._set_state(STATE_CONNECTING)
        self._stop.clear()
        self._link.connect(timeout)
        self._log(f"[fsd] connected {self._link.host}:{self._link.port}")
        self._set_state(STATE_WAIT_LOGIN)
        # 复位机模配置上报状态（新会话重新累计）
        self._acfg_last_sent = None
        self._acfg_full = None
        self._acfg_tokens = ACFG_CAPACITY
        self._acfg_refill_at = time.monotonic()
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

    def send_aircraft_info(self, to: str = "SERVER") -> None:
        """机型/设备信息推送（#SB PI:GEN），模拟器机型变化时向服务器更新 equipment。"""
        self._send(pk.plane_info(self.callsign, to, self.aircraft_icao,
                                 self.airline_icao, self.livery_icao))

    def request_flight_plan(self, target: str) -> None:
        """向服务器查询某呼号的缓存飞行计划（$CQ ...:SERVER:FP:<target>），
        服务器以 $FP 应答（含机型），用于远程玩家机型识别兜底。"""
        self._send(pk.client_query_fp(self.callsign, target))

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

    # ---------- 机模配置 ACC 上报（swift 机制） ----------

    def _acfg_take_token(self) -> bool:
        """令牌桶限速：容量 10、每 5s 补 1 token（懒补充）。"""
        now = time.monotonic()
        self._acfg_tokens = min(
            ACFG_CAPACITY,
            self._acfg_tokens
            + int((now - self._acfg_refill_at) / ACFG_REFILL_INTERVAL))
        self._acfg_refill_at = now
        if self._acfg_tokens <= 0:
            return False
        self._acfg_tokens -= 1
        return True

    @staticmethod
    def _norm_parts(parts) -> dict:
        """规范化机模部件：浮点四舍五入到 1 位、丢弃 NaN/Inf（保证 JSON 合法、
        消除 5Hz 帧间 N1 抖动导致的无效 diff）。"""
        out: dict = {}
        for k, v in parts.items():
            if isinstance(v, dict):
                out[k] = FsdSession._norm_parts(v)
            elif isinstance(v, float):
                if math.isfinite(v):
                    out[k] = round(v, 1)
                # 非有限值丢弃，不进入 diff
            else:
                out[k] = v
        return out

    @staticmethod
    def _acfg_dump(payload: dict) -> Optional[str]:
        try:
            return json.dumps(payload, separators=(",", ":"), allow_nan=False)
        except ValueError:
            return None

    def notify_aircraft_parts(self, parts: Optional[dict]) -> None:
        """飞行数据中的机模部件变化 → 增量 ACC 上报（$CQ:@94836）。

        与 swift sendIncrementalAircraftConfig() 对应：先比较、再令牌桶限速、
        只发变化字段，末次值作为增量基准。规范化后比较/发送，NaN 帧不产生上报。"""
        if not parts or not self._link.connected or self.state != STATE_ONLINE:
            return
        norm = self._norm_parts(parts)
        if not norm or norm == self._acfg_last_sent:
            return
        if not self._acfg_take_token():
            return
        prev = self._acfg_last_sent or {}
        diff = {k: v for k, v in norm.items() if prev.get(k) != v}
        if not diff:
            return
        payload = self._acfg_dump({"config": diff})
        if payload is None:
            return
        self._acfg_last_sent = norm
        self._acfg_full = norm
        self._send(pk.aircraft_config(self.callsign, ACFG_BROADCAST_TO, payload))

    def _send_full_aircraft_config(self, to: str) -> None:
        """对端请求全量（{"request":"full"}）→ 应答完整配置（is_full_data=true）。"""
        if not self._acfg_full:
            return
        full = dict(self._acfg_full)
        full["is_full_data"] = True
        payload = self._acfg_dump({"config": full})
        if payload is None:
            return
        self._send(pk.aircraft_config(self.callsign, to, payload))

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

        elif p.kind == "$CQ" and p.type == "ACC":
            # 机模配置：JSON 可能被冒号切开，重拼后解析
            self._on_aircraft_config_query(p.from_, ":".join(p.flds))

        if self.on_position_packet:
            self.on_position_packet(p)

    def _on_aircraft_config_query(self, sender: str, raw_json: str) -> None:
        try:
            obj = json.loads(raw_json)
        except ValueError:
            return
        if obj == {"request": "full"}:
            # 请求全量（对端希望拿到我方完整机模配置）
            self._send_full_aircraft_config(sender)
        elif isinstance(obj, dict) and self.on_aircraft_config_received:
            cfg = obj.get("config", obj)
            if isinstance(cfg, dict):
                self.on_aircraft_config_received(sender, cfg)

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
