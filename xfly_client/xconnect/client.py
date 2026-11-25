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
from dataclasses import dataclass, field
from typing import Callable, Optional

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 51001
RECONNECT_INTERVAL = 3.0
RX_LINE_LIMIT = 1 << 16


@dataclass
class FlightData:
    aircraft_icao: str = ""        # 模拟器当前机型 ICAO（插件自动识别）
    aircraft_name: str = ""        # 机型 UI 名（acf_ui_name，如 "ToLiss A321"）
                                   # 同 ICAO 不同机模靠它区分，用于机型适配表匹配
    flight_id: str = ""            # Mode-S Flight ID（byte[8]，如 N844X/KLM511）
                                   # 注意不是 .acf 文件名，也不是 hexcode
    registration: str = ""         # 本机注册号（插件读 acf_tailnum，缺失时取涂装目录名）
    registration_src: str = ""     # 尾号来源：tailnum / livery / ""（插件标注）
    engine_count: int = 0          # 发动机数量（sim/aircraft/engine/acf_num_engines）
    latitude: float = 0.0
    longitude: float = 0.0
    altitude: float = 0.0
    elevation: float = 0.0
    altitude_msl: float = 0.0      # ft
    altitude_agl: float = 0.0      # ft
    altitude_indicated: float = 0.0  # ft（座舱高度表，用于 @ 包高度修正）
    pitch: float = 0.0
    bank: float = 0.0
    heading: float = 0.0           # 磁航向 deg
    true_heading: float = 0.0
    groundspeed: float = 0.0       # kt
    indicated_airspeed: float = 0.0
    vertical_speed: float = 0.0    # fpm
    com1_freq: int = 0             # MHz*1000
    com2_freq: int = 0
    com3_freq: int = 0
    com1_standby: int = 0          # MHz*1000（不可用的机列为 0）
    com2_standby: int = 0
    com3_standby: int = 0
    # 音频面板（对齐 XVoice）：COM 一旦断电/未选中收听，频率虽在但不可用
    avionics_on: bool = False      # 航电总电门（COM 是否通电）
    # 单个 COM 电源开关（比 avionics_on 精确；缺失时插件用 avionics_on 兜底）
    com1_power: bool = False
    com2_power: bool = False
    com3_power: bool = False
    audio_sel_com1: bool = False   # 音频面板选中收听 COM1
    audio_sel_com2: bool = False   # 音频面板选中收听 COM2
    audio_sel_com3: bool = False   # 音频面板选中收听 COM3（部分机型无）
    audio_vol_com1: float = 0.0    # COM1 收听音量 [0..1]
    audio_vol_com2: float = 0.0    # COM2 收听音量 [0..1]
    # 副驾驶音频面板（双人机组/前后座；XVoice 同样采集这 4 项）
    audio_sel_com1_copilot: bool = False
    audio_sel_com2_copilot: bool = False
    audio_vol_com1_copilot: float = 0.0
    audio_vol_com2_copilot: float = 0.0

    # ---- 发射/接收选择 ----
    # 两套字段并存是历史原因，语义不同，**不要合并**：
    #   tx_com      : XConnect 私有，1-based，1/2/3=COM1/2/3，0=未选中
    #   pilot_xmit  : 两插件统一，0-based，0/1/2=COM1/2/3，-1=未选中
    # 前端 RMP 用 tx_com 高亮，控制回传用 pilot_xmit。
    tx_com: int = 0
    pilot_xmit: int = -1
    copilot_xmit: int = -1
    com_transmit: list = field(default_factory=list)  # [com1,com2,com3] 是否在发
    com_receive_all: bool = False  # 全收（所有 COM 同时接收）
    audio_panel_on: bool = True    # 音频面板供电（MSFS 侧主用；XConnect 侧看 avionics_on）
    transponder: int = 0
    # 应答机模式（规范枚举）：0=OFF 1=STBY 2=ON 3=ALT 4=TEST 5=GND
    # 插件内部对深度机模（FBW/Fenix/A220…）会改读其 LVar 并反解成这个枚举，
    # 因此**这个值可信**，可直接用于界面显示，不需要客户端自己判断机型。
    xpdr_mode: int = 0
    squark: str = "S"
    tcas_filter: int = 0           # 0=ALL 1=THRT 2=ABV 3=BLW 4=URSTR
    tcas_alert: int = 0            # 0/1 TCAS 告警
    tcas_advisory: int = 0         # 0=None 1=Traffic 2=Resolution
    xpdr_sys: int = -1             # 机型自定义：应答机 1/2 号（-1=该机型无此字段）
    xpdr_pwr: int = -1             # 机型自定义：电源 OFF/STBY/AUTO/ON
    xpdr_tcas_mode: int = -1       # 机型自定义：TCAS STBY/TA/TARA
    xpdr_tcas_alt: int = -1        # 机型自定义：ALL/ABV/BLW/THRT
    gear_deploy: float = 0.0
    flaps_ratio: float = 0.0
    throttle_ratio: float = 0.0
    parts: dict = field(default_factory=dict)   # 机模部件（ACC 配置），swift 键名


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
    altitude: float = 0.0          # ft MSL（真高）
    pitch: float = 0.0
    bank: float = 0.0
    heading: float = 0.0
    ground_speed: float = 0.0
    aircraft: str = ""             # 机型 ICAO；空 = 未识别（插件回退通用模型）
    aircraft_family: str = ""
    airline: str = ""              # 涂装/航司码（PI AIRLINE/LIVERY 或呼号前三位）
    registration: str = ""         # 注册号（尾号）：CSL 匹配第一级
    on_ground: bool = False        # 遥测 onGround（PBH bit1 / #SB VI）
    altitude_agl: float = -1.0     # ft；<0 = 未知（^/#SL/#ST 包才携带）


_FD_FIELDS = set(FlightData.__dataclass_fields__)

# 机型档案能力位 → 协议分类名（与 MConnect aircraftdb.h 的 CAP_* 一致）
_CAP_BITS = {
    "xpndr_code": 0x01,
    "xpndr_mode": 0x02,
    "com_freq":   0x04,
    "com_swap":   0x08,
    "com_select": 0x10,
}


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
        # 机型能力：当前飞机能控制什么（MConnect 按实际机型探测后推送）
        # 结构见 MConnect/PROTOCOL.md 的 aircraft_caps
        self.on_aircraft_caps: Optional[Callable[[dict], None]] = None
        self.aircraft_caps: dict = {}
        self.on_log: Optional[Callable[[str], None]] = None

    def control_options(self) -> dict:
        """当前机型可用的控制选项（按分类列出真实存在的事件名）。

        返回形如 {"xpndr_code": ["XPDR_CODE_SET", ...], "com_freq": [...]}。
        客户端应据此渲染选项：**只显示这架飞机真能操作的**，不要给假选项。
        机型切换后 MConnect 会自动重推，这里的值随之更新。
        """
        opts: dict = {}
        for ev in (self.aircraft_caps.get("events") or []):
            cat = ev.get("cat")
            name = ev.get("name")
            if cat and name:
                opts.setdefault(cat, []).append(name)
        return opts

    def supports(self, category: str) -> bool:
        """当前机型是否支持某类控制。

        category: "com_freq" / "com_select" / "xpdr_code" / "xpdr_mode" /
                  "xpndr_ident"

        判断顺序（与插件侧一致）：
          1. 机型档案声明支持（profile_caps 位）→ 最可靠
          2. InputEvent 语义分类有命中
          3. 标准事件兜底
        """
        caps = self.aircraft_caps
        if not caps:
            return True   # 未收到能力报文时保持乐观（老版本插件无此报文）

        bit = _CAP_BITS.get(category)
        if bit and (int(caps.get("profile_caps", 0) or 0) & bit):
            return True

        ie = caps.get("inputev") or {}
        if int(ie.get(category, 0) or 0) > 0:
            return True

        if category.startswith("com_") or category.startswith("xpndr_"):
            std = caps.get("standard") or {}
            if std.get("com") or std.get("xpdr"):
                return True
        return False

    def profile_id(self) -> str:
        """当前命中的机型档案 ID（如 "fbw-a32nx"），未知返回 "none"。

        用于界面提示"此机型走哪套写法"以及解释为什么某操作不可用。
        """
        return self.aircraft_caps.get("profile") or "none"

    def control_backend(self) -> str:
        """当前机型写入走哪条通道：standard / inputev / wasm / "”。

        帮助界面提示用户"为什么这个按钮能点/不能点"。
        """
        caps = self.aircraft_caps
        if not caps:
            return ""
        if (caps.get("wasm") or {}).get("state") == "ready":
            return "wasm"
        if int((caps.get("inputev") or {}).get("total", 0) or 0) > 0:
            return "inputev"
        return "standard"

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
        if not isinstance(doc, dict):
            return   # 只处理对象报文，避免非预期类型在接收线程抛异常
        if self.on_message:
            self.on_message(doc)
        msg_type = doc.get("type", "")
        if msg_type == "csl_status":
            if self.on_csl_status:
                self.on_csl_status(doc)
        if msg_type == "aircraft_caps":
            # 机型能力：按实际机型给出的可选项（含真实事件名与 hash）
            self.aircraft_caps = doc
            if self.on_aircraft_caps:
                self.on_aircraft_caps(doc)
            return
        if msg_type != "flight_data":
            return
        try:
            fd = FlightData(**{k: v for k, v in doc.items() if k in _FD_FIELDS})
        except (TypeError, ValueError):
            return   # 字段类型异常不应杀死接收线程
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

    # ---------- 反向控制（写模拟器） ----------
    #
    # X-Plane 侧（XConnect）与 MSFS 侧（MConnect）都支持这两个报文，但底层机制
    # 完全不同：XConnect 直写 dataref，MConnect 走 SimConnect 事件。对客户端
    # 而言接口一致，因此这里只按协议发，不区分插件类型。
    #
    # 注意：模拟器侧是「最新有效」语义，命令可能被丢弃（队列满）或被子系统
    # 覆盖（例如机模自己又改回应答机码），因此调用后应以回读的 flight_data
    # 为准刷新界面，不要假定写入一定成功。

    def send_radio_frequency(self, com: int, freq_mhz1000: int,
                             active: bool = True) -> None:
        """设置 COM 频率（主频/备频）。

        com: 1..3；freq_mhz1000: MHz*1000（122800 = 122.800 MHz），
        与 flight_data 的 com*_freq 单位一致，可直接回填。
        """
        self._send(json.dumps({
            "type": "Send:RADIO", "com": int(com),
            "active": bool(active), "freq": int(freq_mhz1000)}))

    def send_radio_swap(self, com: int) -> None:
        """主频与备频互换。"""
        self._send(json.dumps({
            "type": "Send:RADIO", "com": int(com), "swap": True}))

    def send_radio_transmit(self, com: int) -> None:
        """选定发射台（COM1/2/3）。对应读侧 flight_data 的 pilot_xmit。

        注意：这是"音频面板选哪个台发"，不是"开始发射语音"。
        实际按下 PTT 由模拟器/机模自己处理。
        """
        self._send(json.dumps({
            "type": "Send:RADIO", "com": int(com), "xmit": True}))

    def send_radio_receive_all(self, on: bool = True) -> None:
        """设置"全收"（所有 COM 同时接收）。"""
        self._send(json.dumps({
            "type": "Send:RADIO", "recv_all": bool(on)}))

    def send_transponder_code(self, code) -> None:
        """设置应答机 4 位码（0..7 每位）。接受 "1234" 或 1234。"""
        self._send(json.dumps({
            "type": "Send:XPDR", "code": str(code)}))

    def send_transponder_state(self, state: int) -> None:
        """设置应答机模式：0=Off 1=Stby 2=Test 3=On 4=Alt 5=Ground（MSFS）。"""
        self._send(json.dumps({
            "type": "Send:XPDR", "state": int(state)}))

    def send_transponder_ident(self, on: bool = True) -> None:
        """触发（或关闭）应答机 IDENT。"""
        self._send(json.dumps({
            "type": "Send:XPDR", "ident": bool(on)}))

    def send_input_event(self, name: str, value: float = 1.0) -> None:
        """按名字触发当前机模的 InputEvent（选项来自 aircraft_caps 报文）。

        这是"按实际机型给选项"的写入端配套：界面从
        ``self.control_options()`` 拿到这架飞机真实存在的事件名，
        用户选中后原样回传即可，不需要猜命名。

        注意：多数按钮是 Push(=1) / Release(=0) 两步，只发 1 通常也能触发
        （机模自己处理复位）。
        """
        self._send(json.dumps({
            "type": "Send:INPUTEV", "name": name, "value": float(value)}))

    def send_rpn(self, rpn: str) -> None:
        """执行一段 RPN（需要 MConnect + MobiFlight WASM 模块）。

        最强通道：能改 LVar、触发 B 事件。仅在
        ``self.control_backend() == "wasm"`` 时可用。
        """
        self._send(json.dumps({"type": "Send:RPN", "rpn": rpn}))

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