"""XConnect 插件客户端：TCP JSON 行协议。

连接 X-Plane 内的 XConnect 插件（默认 127.0.0.1:51001）：
- 持续监听插件是否上线：连接失败时周期性重试（RECONNECT_INTERVAL）
- 接收插件推送的 flight_data（本机飞行数据，5Hz）
- 向插件推送在线玩家/管制列表 → 插件渲染 CSL 模型

报文：每条 JSON 一行（\n 结尾），字段 snake_case。
"""

from __future__ import annotations

import json
import socket
import threading
import time
from dataclasses import dataclass
from typing import Callable, Optional

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 51001
RECONNECT_INTERVAL = 3.0
RX_LINE_LIMIT = 1 << 16


@dataclass
class FlightData:
    latitude: float = 0.0
    longitude: float = 0.0
    altitude: float = 0.0
    elevation: float = 0.0
    altitude_msl: float = 0.0      # ft
    altitude_agl: float = 0.0      # ft
    pitch: float = 0.0
    bank: float = 0.0
    heading: float = 0.0           # 磁航向 deg
    true_heading: float = 0.0
    groundspeed: float = 0.0       # kt
    indicated_airspeed: float = 0.0
    vertical_speed: float = 0.0    # fpm
    com1_freq: int = 0             # MHz*1000
    com2_freq: int = 0
    transponder: int = 0
    squark: str = "S"
    gear_deploy: float = 0.0
    flaps_ratio: float = 0.0
    throttle_ratio: float = 0.0


@dataclass
class AtcEntry:
    callsign: str = ""
    frequency: str = ""
    type: str = ""
    latitude: float = 0.0
    longitude: float = 0.0
    rating: int = 0


@dataclass
class PlayerEntry:
    callsign: str = ""
    latitude: float = 0.0
    longitude: float = 0.0
    altitude: float = 0.0
    pitch: float = 0.0
    bank: float = 0.0
    heading: float = 0.0
    ground_speed: float = 0.0
    aircraft: str = "A320"
    aircraft_family: str = ""


_FD_FIELDS = set(FlightData.__dataclass_fields__)


class XConnectClient:
    def __init__(self, host: str = DEFAULT_HOST, port: int = DEFAULT_PORT) -> None:
        self.host = host
        self.port = port
        self._sock: Optional[socket.socket] = None
        self._stop = threading.Event()
        self._tx_lock = threading.Lock()
        self._rx_thread: Optional[threading.Thread] = None
        self._ever_connected = False

        self.on_connected: Optional[Callable[[], None]] = None
        self.on_disconnected: Optional[Callable[[], None]] = None
        self.on_flight_data: Optional[Callable[[FlightData], None]] = None
        self.on_message: Optional[Callable[[dict], None]] = None
        self.on_csl_status: Optional[Callable[[dict], None]] = None
        self.on_log: Optional[Callable[[str], None]] = None

    @property
    def connected(self) -> bool:
        """插件是否在线（TCP 连接已建立）。"""
        return self._sock is not None

    # ---------- 生命周期 ----------

    def start(self) -> None:
        """持续监听插件上线：连接失败自动重试。"""
        if self._rx_thread is not None and self._rx_thread.is_alive():
            return
        self._stop.clear()
        self._rx_thread = threading.Thread(
            target=self._run, name="xconnect-rx", daemon=True)
        self._rx_thread.start()

    def stop(self) -> None:
        self._stop.set()
        self._close()
        if self._rx_thread is not None and self._rx_thread.is_alive():
            self._rx_thread.join(timeout=2.0)
        self._rx_thread = None

    def _run(self) -> None:
        while not self._stop.is_set():
            try:
                sock = socket.create_connection(
                    (self.host, self.port), timeout=4.0)
            except OSError:
                if not self._ever_connected:
                    self._log("[xconnect] 未发现模拟机")
                self._stop.wait(RECONNECT_INTERVAL)
                continue
            self._ever_connected = True
            sock.settimeout(1.0)
            self._sock = sock
            self._log("[xconnect] 已发现模拟机")
            if self.on_connected:
                self.on_connected()
            self._rx_loop(sock)
            self._close()
            if self.on_disconnected:
                self.on_disconnected()
            if not self._stop.is_set():
                self._stop.wait(RECONNECT_INTERVAL)

    def _rx_loop(self, sock: socket.socket) -> None:
        buf = b""
        while not self._stop.is_set():
            try:
                data = sock.recv(32768)
            except socket.timeout:
                continue
            except OSError:
                return
            if not data:
                return
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                line = line.strip()
                if line:
                    self._handle_line(line)

    def _handle_line(self, line: str) -> None:
        try:
            doc = json.loads(line)
        except json.JSONDecodeError:
            return
        if self.on_message:
            self.on_message(doc)
        msg_type = doc.get("type", "")
        if msg_type == "csl_status":
            if self.on_csl_status:
                self.on_csl_status(doc)
            return
        if msg_type != "flight_data":
            return
        fd = FlightData(**{k: v for k, v in doc.items() if k in _FD_FIELDS})
        if self.on_flight_data:
            self.on_flight_data(fd)

    # ---------- 发送 ----------

    def _send(self, payload: str) -> None:
        sock = self._sock
        if sock is None:
            return
        data = (payload + "\n").encode("utf-8")
        with self._tx_lock:
            try:
                sock.sendall(data)
            except OSError:
                pass

    def send_players(self, my_cid: str, players: list[PlayerEntry]) -> None:
        self._send(json.dumps({
            "type": "Send:OPD:FSD9", "mycid": my_cid,
            "data": [p.__dict__ for p in players]}, ensure_ascii=False))

    def send_atc(self, atc_list: list[AtcEntry]) -> None:
        self._send(json.dumps({
            "type": "Send:ATC",
            "data": [a.__dict__ for a in atc_list]}, ensure_ascii=False))

    # ---------- 内部 ----------

    def _close(self) -> None:
        sock, self._sock = self._sock, None
        if sock is not None:
            try:
                sock.close()
            except OSError:
                pass

    def _log(self, msg: str) -> None:
        if self.on_log:
            self.on_log(msg)