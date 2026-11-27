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
from ..aircraft_profile import AircraftParts, SERVER_LIGHT_KEYS, SOURCE_FSD
from ..logutil import redact
from .auth import FsdAuthState
from .link import FsdLink, SERVER_CALLSIGN
from .parser import ParsedPacket, parse

STATE_DISCONNECTED = "Disconnected"
STATE_CONNECTING = "Connecting"
STATE_WAIT_LOGIN = "WaitLogin"
STATE_ONLINE = "Online"

POSITION_INTERVAL = 5.0
INTERIM_POSITION_INTERVAL = 1.0   # #SB VI 中间位置（swift 1s）
VISUAL_POSITION_INTERVAL = 0.2    # ^ 视觉位置（swift 200ms）
KEEPALIVE_INTERVAL = 60.0

# 机模配置 ACC 上报（swift CTokenBucket 同参数）
ACFG_CAPACITY = 10          # 桶容量（突发上限）
ACFG_REFILL_INTERVAL = 5.0  # 每 5s 补 1 token（≈1 条/5s）
ACFG_BROADCAST_TO = "@94836"
# **全量 ACC 同时发给服务器本体**。
#
# 为什么需要：站点的 `aircraft.engines` 只可能来自 ACC（PI 里没有 engines ——
# swift 的 `PlaneInformation::toTokens()` 只发 EQUIPMENT/AIRLINE/LIVERY 三对），
# 而 PI 我们是发给 `SERVER` 的、ACC 过去只发 `@94836` 广播地址。实测站点上
# equipment/airline/livery 有值、engines 恒 null，正好是"发给 SERVER 的通道
# 生效、只发广播的通道没被采纳"的形状。
#
# 只重播全量（登录/60s/对端请求）才走这条，增量仍只发 @94836，流量可忽略；
# 若服务器不认这个目标（回 004），`_maybe_disable_unsupported` 会自动关掉。
ACFG_SERVER_TARGET = "SERVER"

# 我们声明的客户端能力（CAPS 应答）。ACCONFIG 是 ACC 的前提：
# swift/vPilot 系只有在对方声明 ACCONFIG=1 时才给它发机模配置。
CAPABILITIES = ("ATCINFO=1", "SECPOS=1", "MODELDESC=1", "ONGOINGCOORD=1",
                "INTERIMPOS=1", "VISUPDATE=1", "ACCONFIG=1")

# 登录后机型信息（PI）与机模配置（ACC）的重发节奏。
# 原因：登录（#AP）与首条 PI/ACC 几乎同时发出，服务器此时可能还没建立好
# 该呼号的会话，会把它们丢掉；而位置 @ 是每 5s 一包，所以看起来"只有位置
# 正常、equipment/airline/gear/onGround 一直为 null"。登录确认后再补发，
# 并周期性重发，保证最终一定生效。
ACFG_INFO_RETRY_DELAYS = (2.0, 6.0, 12.0)

# 是否在 LIVERY 中附带当前实际尾号，例如 `SWIFT_A685-D-AIKI`。
# swift 的完整格式是 `SWIFT_L<涂装库id>A<机型库id>M<模型库id>`；本项目没有
# swift 的涂装/模型数据库，算不出真实的 L/M，只能给出 `A<id>`。
# `A<id>` 段必须保留（服务器据此反查机型），尾号附在其后便于站点/他人识别
# 实际飞机。若某台服务器对 LIVERY 校验很严导致 aircraft 段丢失，把这里改成
# False 即回到纯 `SWIFT_A<id>`。
LIVERY_INCLUDE_REGISTRATION = True


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
        self.registration = ""      # 本机尾号（PI 的 LIVERY/REG= 扩展字段）
        self.engines = 0            # 发动机数量（PI 的 ENGINES= 扩展键；0 = 不发送）

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
        self._heading_deg = 0.0
        self._alt_agl_ft = -1.0
        self._vs_fpm = 0.0
        self._on_ground = False
        self._gear_deploy = 0.0

        # 服务器不一定支持 ^ / #SB VI；收到 004 Syntax error 时按最近发送的
        # 报文类型自动关闭，避免 5Hz 错误刷屏。
        self._visual_pos_disabled = False
        self._interim_pos_disabled = False
        self._last_visual_sent = 0.0
        self._last_interim_sent = 0.0
        self._last_syntax_log_at = 0.0

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
        self.on_no_flight_plan: Optional[Callable[[], None]] = None
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
        # 服务器是否接受"直发 SERVER 的 ACC"（收到 004 自动关，见
        # `_maybe_disable_unsupported`）
        self._acfg_server_ok = True
        self._last_acc_server_sent = 0.0
        self._last_pi_audit = ""     # [report] 自检行去重

        # 机型信息/机模配置的重发调度（见 ACFG_INFO_RETRY_DELAYS）
        self._info_next_at = 0.0
        self._info_retries = 0
        self._info_acked = False   # 是否已收到服务器消息（判定登录完成）
        # 最近一次向我们发 PIR 的呼号（通常是服务器采集器 atis_collector）。
        # XFlySim 服务器只从对它的 PIR 应答里采纳机型；切换机模后需要把新 PI
        # 也发给它，否则服务器记录会一直停在登录时的旧机型。
        self._last_pi_requester = ""

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
        self._info_next_at = 0.0
        self._info_retries = 0
        self._info_acked = False
        self._last_pi_requester = ""
        self._acfg_server_ok = True
        self._last_acc_server_sent = 0.0
        self._last_pi_audit = ""
        # 新连接重新探测（服务器可能不同）
        self._visual_pos_disabled = False
        self._interim_pos_disabled = False
        self._last_visual_sent = 0.0
        self._last_interim_sent = 0.0
        self._last_syntax_log_at = 0.0
        self._send(pk.add_pilot(self.callsign, self.cid, self.credential,
                                pk.NETWORK_RATING, pk.PROTOCOL_REVISION,
                                self.simulator_type, self.real_name))

    def start_reporting(self) -> None:
        self._set_state(STATE_ONLINE)
        # 2 秒后再发首轮 PI/ACC（此时服务器多半已完成登录），随后按退避重发
        self._info_next_at = time.monotonic() + ACFG_INFO_RETRY_DELAYS[0]
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
        if not line or not self._link.connected:
            return   # 空报文不发送（plane_info 机型未知时返回空串）
        if line.startswith("^"):
            self._last_visual_sent = time.monotonic()
        elif line.startswith("#SB") and ":VI:" in line:
            self._last_interim_sent = time.monotonic()
        with self._send_lock:
            try:
                self._link.send(line)
                self._log(f"> {redact(line)}")
            except Exception as exc:
                self._log(f"[fsd] send failed: {exc}")

    def send_text(self, to: str, message: str) -> None:
        self._send(pk.text_message(self.callsign, to, message))

    def send_frequency_text(self, mhz: float, message: str) -> None:
        self._send(pk.text_message(self.callsign, pk.freq_to_target(mhz), message))

    def request_metar(self, station: str) -> None:
        self._send(pk.metar_request(self.callsign, station))

    def _effective_livery(self) -> str:
        """swift 格式的 LIVERY（必要时附加实际尾号）。

        `A<机型库id>` 段是服务器反查机型的唯一依据，必须保留；本项目没有
        swift 的涂装/模型数据库，真实的 `L<...>`/`M<...>` id 无法生成，
        因此在 A 段后附加当前尾号（可用 LIVERY_INCLUDE_REGISTRATION 关闭）。
        """
        from .. import aircraft_db
        lv = self.livery_icao or aircraft_db.swift_livery(self.aircraft_icao)
        reg = (self.registration or "").strip()
        if lv and reg and LIVERY_INCLUDE_REGISTRATION:
            return f"{lv}-{reg}"
        return lv

    def _ensure_engines(self) -> int:
        """`engines` 为 0 时用 swift 机型库台数兜底。

        桥（XConnect/MConnect）在"机型还没加载完"的帧里会报
        `engine_count=0`，旧代码只在 `eng > 0` 时才写回 → 站点 `engines`
        永远缺这一项。机型库里的台数是权威静态值，比"不发"安全得多。
        """
        if self.engines > 0:
            return self.engines
        try:
            from .. import aircraft_db
            rec = aircraft_db.lookup(self.aircraft_icao)
            n = int((rec or {}).get("engines") or 0)
        except Exception:
            n = 0
        if n > 0:
            self.engines = n
        return self.engines

    def _pi_line(self, to: str) -> str:
        self._ensure_engines()
        airline = self.airline_icao
        if not airline:
            # 线级最后一道防线：配置/界面没给航司时按呼号前缀补。
            # 缺 AIRLINE 的 PI 会让站点 aircraft.airline 恒 null。
            from ..report_identity import airline_from_callsign
            airline = airline_from_callsign(self.callsign)
        line = pk.plane_info(self.callsign, to, self.aircraft_icao,
                             airline, self._effective_livery(),
                             self.registration, self.engines)
        self._audit_pi(line)
        return line

    def _audit_pi(self, line: str) -> None:
        """机型上报自检：内容变化时打一行 `[report]`，便于和站点 JSON 对照。

        只在内容变化时打（三个目标地址共用同一内容），切换机模/尾号才出现，
        不会刷屏。缺失项会带 `⚠ missing=` 前缀，下次出问题一眼定位是哪个字段。
        """
        body = line.split(":PI:GEN:", 1)[1] if ":PI:GEN:" in line else line
        if not body or body == self._last_pi_audit:
            return
        self._last_pi_audit = body
        pairs = [kv for kv in body.split(":") if "=" in kv]
        missing = [k for k in ("EQUIPMENT", "AIRLINE", "LIVERY", "ENGINES")
                   if not any(p.startswith(k + "=") for p in pairs)]
        # 带呼号：`tools/verify_site_aircraft.py` 靠它把本地发放与站点记录对齐
        text = f"[report] {self.callsign} PI " + " ".join(pairs)
        if missing:
            text += f" ⚠ missing={','.join(missing)}"
        self._log(text)

    def send_capabilities(self, to: str) -> None:
        """CAPS 应答：声明支持 ACC（`ACCONFIG=1`）。

        swift/vPilot 系的约定是"对方声明 ACCONFIG=1 才给它发机模配置"；
        我们不声明，对端就没必要发、服务器也可能据此决定是否采纳我们发的
        ACC（`aircraft.engines` 的唯一来源）。
        """
        self._send(pk.caps_response(self.callsign, to, CAPABILITIES))

    def send_plane_info(self, to: str) -> None:
        """PIR 应答（PI:GEN）。

        ⚠️ 必须带 swift 格式的 `LIVERY=SWIFT_A<id>`：XFlySim 服务器按**位置**
        用第 3 个键（LIVERY）反查机型。若 livery 为空，后面的 `ENGINES=`
        会被顶到 LIVERY 的位置，服务器解析不出机型 → **整条 aircraft 段被
        丢弃**（equipment/airline/livery/engines 全 null）。
        这正是 `atis_collector`（服务器采集器）请求 PIR 后我方应答导致
        equipment 一直为 null 的原因。
        """
        self._send(self._pi_line(to))

    def send_plane_info_fsi(self, to: str) -> None:
        # 末位 model 串是自由描述（"DLH A359 B-1234"）：带上尾号，让 FSInn 系
        # 客户端（以及回读该串的我们）也能拿到注册号
        model = f"{self.airline_icao} {self.aircraft_icao} {self.registration}".strip()
        self._send(pk.plane_info_fsi(self.callsign, to, self.aircraft_icao,
                                     self.airline_icao, model))

    def send_plane_info_request(self, to: str) -> None:
        """机型信息请求（swift/xPilot 系应答 PI:GEN，含机型+航司+涂装）。"""
        self._send(pk.plane_info_request(self.callsign, to))

    def send_plane_info_request_fsi(self, to: str) -> None:
        """FSInn 系机型信息请求（应答 FSIPI，含航司+机型+模型串）。"""
        self._send(pk.plane_info_request_fsi(self.callsign, to))

    def send_aircraft_info(self, to: str = "SERVER") -> None:
        """机型/设备信息推送（#SB PI:GEN），模拟器机型变化时向服务器更新 equipment。

        `LIVERY` 用 **swift 格式**（`SWIFT_A<id>`），因为 XFlySim 服务器
        是从这个字段反查机型的 —— 见 `packet.plane_info` 的说明。
        尾号单独走 `REG=`，不再占用 LIVERY。

        这里**每次现算**而不是缓存一份，是为了避免机型变化时
        `LIVERY` 与 `EQUIPMENT` 不同步（曾因此让服务器把整条 PI 丢掉）。

        ⚠️ XFlySim 服务器**只从对服务器采集器（atis_collector）的 PIR 应答**
        里采纳 aircraft 信息；直接发给 `SERVER` 的 PI 不会更新站点记录。
        因此这里在发给 SERVER 的同时，也发给最近一次发 PIR 的采集器和
        `@94836` 广播地址——切换机模后才能让服务器把 equipment 更新过来
        （否则会一直停在登录时的旧机型）。
        """
        self._send(self._pi_line(to))
        if to.upper() == "SERVER":
            self._send(self._pi_line(ACFG_BROADCAST_TO))
            req = (self._last_pi_requester or "").strip()
            if req and req.upper() not in ("SERVER", ACFG_BROADCAST_TO.upper()):
                self._send(self._pi_line(req))

    def request_aircraft_config(self, target: str) -> None:
        """向对端请求全量机模配置（$CQ...:ACC:{"request":"full"}）。

        对方会回一条带 is_full_data=true 的 ACC；用于晚加入时补齐
        远端灯光/襟翼/起落架等部件（增量广播可能错过了）。
        """
        self._send(pk.aircraft_config_request(self.callsign, target))

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
                            on_ground: bool = False,
                            alt_agl_ft: float = -1.0, vs_fpm: float = 0.0,
                            gear_deploy: float = 0.0) -> None:
        self._lat = lat
        self._lon = lon
        self._alt_ft = alt_true_ft
        self._gs_kt = gs_kt
        self._pbh = pk.encode_pbh(pitch_deg, bank_deg, heading_deg, on_ground)
        self._corr_ft = corr_ft
        self._xpdr_mode = xpdr_mode
        self._squawk = squawk
        self._heading_deg = heading_deg
        self._alt_agl_ft = alt_agl_ft
        self._vs_fpm = vs_fpm
        self._on_ground = on_ground
        self._gear_deploy = gear_deploy
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

    # 兼容旧调用点；唯一真源在 aircraft_profile。
    _SERVER_LIGHT_KEYS = frozenset(SERVER_LIGHT_KEYS)
    _BOOL_KEYS = frozenset({
        "gear_down", "spoilers_out", "on_ground", "is_full_data",
    })

    @staticmethod
    def _norm_parts(parts) -> dict:
        """模拟器原始部件 → FSD ACC 增量 schema。

        归一化在 aircraft_profile.AircraftParts 完成：三态布尔、9 灯别名、
        1..n 发、flaps 0..100、NaN/Inf 丢弃；这里只做服务器 6 灯裁剪。
        显式带 is_full_data=False，与 swift toIncrementalJson() 契约一致。
        """
        return AircraftParts.from_raw(parts, source=SOURCE_FSD).to_incremental_json()

    @staticmethod
    def _acfg_dump(payload: dict) -> Optional[str]:
        try:
            return json.dumps(payload, separators=(",", ":"), allow_nan=False)
        except ValueError:
            return None

    @staticmethod
    def _acfg_swift_shape(cfg: dict, engine_count: int = 0) -> dict:
        """ACC 的线上形状：标准顶层 + 一份 swift 形状的冗余副本。

        权威格式是 VATSIM ACC 文档写的那七个顶层键（`is_full_data` /
        `lights` / `engines` / `gear_down` / `flaps_pct` / `spoilers_out` /
        `on_ground`）——swift 与 vPilot 都这么发，对端只认这些。

        额外再放一份 `config.aircraft.engines`：swift 的
        `CAircraftConfiguration::toJson()` 把模型（含引擎）放在 `aircraft`
        键下，XFlySim 服务器实测会读这个位置。未知键对宽容解析器无害，对
        位置敏感的部分也只会多一层，所以两个位置同时给，最大化命中率。

        `engine_count` > 0 且 `engines` 缺失时（桥当前帧没给引擎状态）
        按台数补一份"全关"掩码 —— 站点 `aircraft.engines` 缺键就是 null，
        宁可先报全关让下一帧覆盖，也不要留空。
        """
        out = dict(cfg)
        eng = out.get("engines")
        if not isinstance(eng, dict) or not eng:
            n = int(engine_count or 0)
            if n > 0:
                from ..report_identity import engine_mask
                eng = engine_mask(n)
                out["engines"] = eng
        if isinstance(eng, dict) and eng:
            ac = dict(out.get("aircraft") or {})
            ac["engines"] = eng
            out["aircraft"] = ac
        return out

    @staticmethod
    def _flatten_swift_acfg(cfg: dict) -> dict:
        """swift 形状 → 内部扁平形状（接收侧）。

        swift 客户端的 engines 嵌在 config.aircraft.engines、襟翼键是
        flapsDown（0..100 百分比）；内部用顶层 engines / flaps_pct。
        灯光的裸键名（beacon/landing…）由 AircraftParts 别名表处理，
        这里不重复。不修改入参。"""
        out = dict(cfg)
        ac = out.get("aircraft")
        if isinstance(ac, dict) and "engines" in ac and "engines" not in out:
            out["engines"] = ac["engines"]
        if "flaps_pct" not in out and "flapsDown" in out:
            out["flaps_pct"] = out["flapsDown"]
        return out

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
        payload = self._acfg_dump(
            {"config": self._acfg_swift_shape(diff, self._ensure_engines())})
        if payload is None:
            return
        # 第 ② 级诊断：组装后的 ACC JSON（与插件 raw/anim 日志对照）
        self._log(f"[parts] acc json: {payload} is_full_data=false")
        self._acfg_last_sent = norm
        self._acfg_full = norm
        self._send(pk.aircraft_config(self.callsign, ACFG_BROADCAST_TO, payload))

    def _send_full_aircraft_config(self, to: str) -> None:
        """对端请求全量（{"request":"full"}）→ 应答完整配置（is_full_data=true）。"""
        if not self._acfg_full:
            return
        full = dict(self._acfg_full)
        full["is_full_data"] = True
        payload = self._acfg_dump(
            {"config": self._acfg_swift_shape(full, self._ensure_engines())})
        if payload is None:
            return
        self._send(pk.aircraft_config(self.callsign, to, payload))

    def _acfg_targets(self) -> tuple[str, ...]:
        """全量 ACC 的发送目标：广播地址 + 服务器本体。

        服务器本体那条是站点 `aircraft.engines` 的关键（见 `ACFG_SERVER_TARGET`
        的说明）；被服务器拒绝（004）后自动剔除，只留广播。
        """
        if self._acfg_server_ok:
            return (ACFG_BROADCAST_TO, ACFG_SERVER_TARGET)
        return (ACFG_BROADCAST_TO,)

    # ---------- 接收 ----------

    def _on_line(self, line: str) -> None:
        self._log(f"< {redact(line)}")
        p = parse(line)

        # 收到服务器消息（欢迎 #TM / 采集器 PIR / 心跳 #DL 等）说明登录已完成：
        # 立即补发一次 PI + 全量 ACC（登录瞬间发的可能已被服务器丢弃）。
        if (self.state == STATE_ONLINE and not self._info_acked
                and p.kind in ("#TM", "#SB", "#DL", "#AP", "#DA")):
            self._info_acked = True
            self._info_next_at = 0.0
            self._info_retries = len(ACFG_INFO_RETRY_DELAYS)

        if p.kind == "$ZC":
            if p.fields:
                resp = self._auth.respond_to_challenge(p.fields[0])
                self._send(pk.auth_response(self.callsign, SERVER_CALLSIGN, resp))

        elif p.kind == "$ER":
            raw = p.type if p.type else (p.flds[0] if p.flds else "")
            desc = ":".join(p.flds[1:]) if len(p.flds) > 1 else ""
            code = pk.resolve_error_code(raw)
            if self._maybe_disable_unsupported(code):
                return
            # 其它 004 语法错误也节流，避免刷爆 UI/日志
            if code == pk.ERR_SYNTAX:
                now = time.monotonic()
                if now - self._last_syntax_log_at < 5.0:
                    return
                self._last_syntax_log_at = now
            if self.on_error_text:
                self.on_error_text(f"服务器错误 {raw}: {desc}")
            # 仅致命错误断链；NoFlightPlan(8)/NoSuchCallsign(7) 等非致命保持连接
            if code < 0 or code in pk.FATAL_ERROR_CODES:
                self._link.close()
            elif code == pk.ERR_NO_FLIGHT_PLAN:
                if self.on_no_flight_plan:
                    self.on_no_flight_plan()

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
                if p.from_:
                    self._last_pi_requester = p.from_
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

        elif p.kind == "$CQ" and p.type == "CAPS":
            # 服务器/对端询问能力：必须声明 ACCONFIG=1，否则对方不会给我们
            # 发机模配置，服务器也可能据此不采纳我们发的 ACC（engines 的
            # 唯一来源，见 ACFG_SERVER_TARGET）。
            if p.from_:
                self.send_capabilities(p.from_)

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
                self.on_aircraft_config_received(
                    sender, self._flatten_swift_acfg(cfg))

    def _maybe_disable_unsupported(self, code: int) -> bool:
        """004 Syntax error 与最近发送的 ^ / VI 相关联 → 关闭对应报文。

        服务器（如当前 XFlySim legacy）不支持 ^ 时会 5Hz 回语法错误；
        自动降级后仍保留 @（5s）与 VI（1s）。
        """
        if code != pk.ERR_SYNTAX:
            return False
        now = time.monotonic()
        if (not self._visual_pos_disabled
                and now - self._last_visual_sent < 1.5):
            self._visual_pos_disabled = True
            self._log("[fsd] 服务器不支持 ^ 视觉位置，已自动关闭（保留 @/VI）")
            return True
        if (not self._interim_pos_disabled
                and now - self._last_interim_sent < 1.5):
            self._interim_pos_disabled = True
            self._log("[fsd] 服务器不支持 #SB VI，已自动关闭（保留 @）")
            return True
        # ACC 直发 SERVER 被拒（服务器不认这个目标）→ 永久退回只发广播。
        # 放在 ^ / VI 之后判定，并额外要求最近没有发过 ^/VI：否则登录时
        # 同时发出的 ^ 引发的 004 会被误记到 ACC 头上。
        if (getattr(self, "_acfg_server_ok", True)
                and now - getattr(self, "_last_acc_server_sent", 0.0) < 2.0
                and now - self._last_visual_sent > 1.5
                and now - self._last_interim_sent > 1.5):
            self._acfg_server_ok = False
            self._log("[fsd] 服务器不接受直发 SERVER 的 ACC，已退回仅广播"
                      "（站点 engines 可能需要服务器侧支持）")
            return True
        return False

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
        next_interim = time.monotonic() + INTERIM_POSITION_INTERVAL
        next_vis = time.monotonic() + VISUAL_POSITION_INTERVAL
        next_ka = time.monotonic() + KEEPALIVE_INTERVAL
        while not self._stop.is_set():
            now = time.monotonic()
            # swift 的 ^ 视觉位置：200ms 一次（不判断服务器能力，先发）
            if (self._has_fix and self._link.connected
                    and not self._visual_pos_disabled and now >= next_vis):
                h = math.radians(self._heading_deg)
                gs_ms = self._gs_kt * 0.514444
                agl = self._alt_agl_ft if self._alt_agl_ft >= 0.0 else 0.0
                nose = 1.0 if (self._on_ground or self._gear_deploy > 0.5) else 0.0
                self._send(pk.visual_position(
                    self.callsign, self._lat, self._lon,
                    float(self._alt_ft), agl, self._pbh,
                    gs_ms * math.sin(h), self._vs_fpm * 0.00508,
                    -gs_ms * math.cos(h), 0.0, 0.0, 0.0, nose))
                next_vis = now + VISUAL_POSITION_INTERVAL
            # swift 的 #SB VI 中间位置：1s 一次，给对端更密的插值样本
            if (self._has_fix and self._link.connected
                    and not self._interim_pos_disabled and now >= next_interim):
                self._send(pk.interim_position(
                    self.callsign, self._lat, self._lon, self._alt_ft,
                    self._gs_kt, self._pbh))
                next_interim = now + INTERIM_POSITION_INTERVAL
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
            # 登录后补发机型信息 + 机模配置，并周期性重发
            if self._link.connected and now >= self._info_next_at:
                self._resend_aircraft_info()
                delay = (ACFG_INFO_RETRY_DELAYS[self._info_retries]
                         if self._info_retries < len(ACFG_INFO_RETRY_DELAYS)
                         else 60.0)
                self._info_retries += 1
                self._info_next_at = now + delay
            time.sleep(0.05)   # 20Hz，保证 200ms 视觉位置定时精度

    def _resend_aircraft_info(self) -> None:
        """补发机型信息（PI）并重播一次全量机模配置（ACC）。

        登录（#AP）与首条 PI/ACC 几乎同时发出，服务器在会话就绪前可能丢弃；
        位置 @ 每 5s 有包所以正常，但 equipment/gear/lights 会一直是 null。
        """
        self.send_aircraft_info()
        self._broadcast_full_aircraft_config()

    def _broadcast_full_aircraft_config(self) -> None:
        """全量机模配置重播：广播地址 **+ 服务器本体**。

        服务器本体那条是站点 `aircraft.engines` 的唯一可能来源（PI 里没有
        engines；见 `ACFG_SERVER_TARGET`）。每个目标各吃一个令牌，令牌不足
        时按顺序优先广播地址。
        """
        if not self._acfg_full or not self._link.connected:
            return
        full = dict(self._acfg_full)
        full["is_full_data"] = True       # 重播必须整块覆盖，不能当增量
        full["engines"] = (full.get("engines")
                           or self._engine_mask_or_none())
        payload = self._acfg_dump(
            {"config": self._acfg_swift_shape(full, self._ensure_engines())})
        if payload is None:
            return
        sent_server = False
        for target in self._acfg_targets():
            if not self._acfg_take_token():
                break
            self._send(pk.aircraft_config(self.callsign, target, payload))
            if target == ACFG_SERVER_TARGET:
                sent_server = True
        if sent_server:
            self._last_acc_server_sent = time.monotonic()
            self._log("[parts] full ACC -> SERVER（engines="
                      f"{len(full.get('engines') or {})} 台）")

    def _engine_mask_or_none(self) -> Optional[dict]:
        n = self._ensure_engines()
        if n <= 0:
            return None
        from ..report_identity import engine_mask
        return engine_mask(n)

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
