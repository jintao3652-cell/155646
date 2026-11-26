"""XFlySim-Client pywebview 应用入口：HTML/CSS/JS 界面。

后端：NetworkModel（FSD + X-Plane UDP），通过 pywebview js_api 暴露给前端。
前端回调 pollEvent 用 api.pull() 拉取事件（日志/文本/飞行数据/状态），避免跨线程 evaluate_js。

运行：python webapp.py
打包：见 PACKAGING.md（PyInstaller，webui/dist 作为数据文件附带）。
"""

from __future__ import annotations

import os
import sys
import threading
import time
from collections import deque
from dataclasses import asdict
from typing import Optional

import webview

from xfly_client import aircraft_db
from xfly_client.bridge_launcher import BridgeLauncher
from xfly_client.config import AppConfig, load_config, save_config
from xfly_client.diagnostics import collect as collect_diagnostics
from xfly_client.diagnostics import export as export_diagnostics_file
from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import STATE_ONLINE
from xfly_client.i18n import translate
from xfly_client.logutil import RotatingFileWriter, redact
from xfly_client.model import NetworkModel
from xfly_client.registration import norm_registration
from xfly_client.simbrief import SimbriefError
from xfly_client.simbrief import import_plan as simbrief_import_plan
from xfly_client.xconnect.client import XConnectClient

# 资源根：PyInstaller 打包后在 sys._MEIPASS 临时解包目录（webui/dist 经
# --add-data 随包附带）；源码运行时为 webapp.py 所在目录。
BASE_DIR = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
WEBUI_DIST = os.path.join(BASE_DIR, "webui", "dist", "index.html")
WEBUI_URL = "file:///" + WEBUI_DIST.replace("\\", "/")
ICON_PATH = os.path.join(BASE_DIR, "assets", "icon.ico")
# FSD 收发报文落盘（只记 > 开头的发送行与 [fsd] 状态行），排查协议问题用
FSD_LOG_PATH = os.path.join(BASE_DIR, "fsd.log")
_FSD_LOG = RotatingFileWriter(FSD_LOG_PATH, max_bytes=10 * 1024 * 1024, backups=3)


def _validate(cfg: AppConfig) -> list[str]:
    missing = []
    if not cfg.callsign.strip():
        missing.append("callsign")
    if not cfg.cid.strip():
        missing.append("cid")
    if not cfg.password.strip():
        missing.append("password")
    if not cfg.real_name.strip():
        missing.append("realname")
    # 机型 ICAO 是 **2~4 位** 字母数字，不是固定 4 位。
    #
    # ICAO Doc 8643 原文："代码由最多四个字符组成"，即 2/3/4 位都合法。
    # 之前写死 `!= 4`，会把库里 **2126 条**合法代码全部拒绝：
    #     2 位 433 条（A9 Quail / B2 / C5 …）
    #     3 位 1693 条（WSP Wasp / RF9 / LCA Tejas / A20N? 不，A20N 是 4 位）
    # 用户开着这类机型时表现为"机型 ICAO（4位）缺失或无效"，
    # 但界面上明明已经识别出了机型 —— 这是个容易让人误解的假报错。
    icao = cfg.aircraft_icao.strip()
    if not (2 <= len(icao) <= 4 and icao.isalnum()):
        missing.append("aircraft")
    if len(cfg.airline_icao.strip()) != 3:
        missing.append("airline")
    return missing


class Api:
    def __init__(self) -> None:
        # 注意：所有状态均为私有属性（下划线开头）。pywebview 会递归遍历
        # js_api 对象的公开属性，若暴露 model/window 等含 .NET/COM 对象的
        # 属性会触发跨线程 COM 访问异常（pythonnet 3.x 行为）。
        self._cfg: AppConfig = load_config()
        self._model: Optional[NetworkModel] = None
        self._events: deque[tuple[str, object]] = deque()
        self._sim_type = pk.SIM_TYPE_XP12
        self._debug: bool = self._cfg.debug_mode
        # 最近一次插件上报的机型名 / 引擎数（connect() 兜底推断用）
        self._last_ac_name: str = ""
        self._last_eng: int = 0

        # MConnect 自动启动（无窗口；仅当 exe 存在且未运行时拉起）
        self._bridge = BridgeLauncher(
            os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
            on_log=lambda m: self._push("log", self._tr(m)))
        if self._cfg.auto_start_mconnect:
            self._bridge.start()

        # XConnect 插件客户端：应用级生命周期，启动即持续监听插件上线
        self._xconnect = XConnectClient(
            self._cfg.xconnect_host, self._cfg.xconnect_port)
        self._xconnect.on_log = lambda m: self._push("log", self._tr(m))
        self._xconnect.on_connected = lambda: self._push("xconnect", "connected")
        self._xconnect.on_disconnected = \
            lambda: self._push("xconnect", "disconnected")
        # 未连线 FSD 也接收并显示飞行数据（连线后由 NetworkModel 接管上报）
        #
        # ⚠️ **这里必须自己做机型推断**，不能把插件原值直接推给前端。
        #
        # 曾经踩过的死锁：
        #   ① 有些机模（F-14 等）`acf_ICAO` 是空串，插件按设计不猜，原样上报 "";
        #   ② 这一行把 "" 直推前端 → `store.aircraft` 被写成 "" （store.js 里
        #      "空值也是合法状态" 那条注释）；
        #   ③ 用户点连接 → 后端 `_validate` 发现 aircraft 为空 → 拒绝；
        #   ④ 连不上 FSD → `NetworkModel` 永远不会创建 → 而机型推断恰好只写在
        #      `NetworkModel._on_xconnect_flight_data` 里 → **推断永远不执行**。
        # 于是界面报「机型 ICAO 缺失」，但用户明明已经开着 F-14 了。
        #
        # 修法：在推送前补一次推断，与 NetworkModel 里的逻辑保持一致。
        # （两处都做是有意为之：这条路径是"未连线预览"，那条是"连线后上报"，
        #  谁都无法保证对方一定先跑过。）
        self._xconnect.on_flight_data = self._push_flight
        self._xconnect.on_csl_status = lambda d: self._push("csl", d)
        self._start_xconnect()

    def _push_flight(self, fd) -> None:
        """推送飞行数据到前端；机型缺失时先按机型名推断补上。"""
        # 记下原始机型名与引擎数，供 connect() 在必要时兜底重推
        self._last_ac_name = getattr(fd, "aircraft_name", "") or ""
        self._last_eng = int(getattr(fd, "engine_count", 0) or 0)
        raw = (getattr(fd, "aircraft_icao", "") or "").strip()
        if not raw:
            guessed = aircraft_db.guess_designator(
                self._last_ac_name, self._last_eng)
            if guessed:
                fd.aircraft_icao = guessed
                # 同时回写配置，否则下次 _validate 仍拿旧值
                if self._cfg.aircraft_icao != guessed:
                    self._cfg.aircraft_icao = guessed
        self._push("flight", asdict(fd))

    def _start_xconnect(self) -> None:
        self._push("xconnect", "waiting")
        self._xconnect.start()  # 持续重连，直到插件上线

    # ---------- 诊断 ----------

    def _diagnostics_extra(self) -> dict:
        xc = self._xconnect
        extra = {
            "xconnect": {
                "connected": bool(xc and xc.connected),
                "plugin": getattr(xc, "plugin", ""),
                "plugin_version": getattr(xc, "plugin_version", ""),
                "protocol": getattr(xc, "protocol_version", 0),
                "capabilities": sorted(getattr(xc, "capabilities", set()) or []),
            }
        }
        m = self._model
        if m is not None:
            extra["model"] = {
                "players": len(m._players),
                "atc": len(m._atc),
                "fsd_state": m.fsd.state if m.fsd else None,
            }
        return extra

    def diagnostics(self) -> dict:
        """返回可展示/复制的诊断 JSON（日志尾部已脱敏）。"""
        return collect_diagnostics(log_path=FSD_LOG_PATH,
                                   extra=self._diagnostics_extra())

    def export_diagnostics(self) -> dict:
        """导出诊断 JSON 到程序目录，返回路径。"""
        name = f"xflysim-diagnostics-{time.strftime('%Y%m%d-%H%M%S')}.json"
        path = os.path.join(BASE_DIR, name)
        try:
            p = export_diagnostics_file(path=path, log_path=FSD_LOG_PATH,
                                        extra=self._diagnostics_extra())
            return {"ok": True, "path": p}
        except OSError as exc:
            return {"ok": False, "error": str(exc)}

    # ---------- 事件推送（前端回调） ----------

    def _tr(self, msg: str) -> str:
        """按保存的语言翻译后端日志/提示文案。"""
        return translate(self._cfg.language, msg)

    def _push(self, event: str, payload) -> None:
        # 日志/UI 回显前先脱敏（#AP 密码等），落盘同样使用脱敏后的内容
        if event == "log" and isinstance(payload, str):
            payload = redact(payload)
        self._events.append((event, payload))
        # 关键报文落盘，方便事后排查（UI 日志关掉就丢了）。
        # 记：> 发送行、< 接收行、[fsd] 状态/错误行；不记 flight（5Hz 刷爆）。
        if event in ("log", "fsdstate") or event == "xconnect":
            try:
                if event == "log" and isinstance(payload, str):
                    s = payload.strip()
                    if not (s.startswith(">") or s.startswith("<")
                            or s.startswith("[fsd]")):
                        s = ""
                else:
                    s = f"<{event}> {payload}"
                if s:
                    _FSD_LOG.write(f"{time.strftime('%H:%M:%S')} {redact(s)}\n")
            except OSError:
                pass
        if len(webview.windows):
            webview.windows[0].evaluate_js("pollEvent()")

    def pull(self) -> list[dict]:
        out: list[dict] = []
        while self._events:
            e, p = self._events.popleft()
            out.append({"event": e, "data": p})
        return out

    # ---------- 设置 ----------

    def get_settings(self) -> dict:
        return asdict(self._cfg)

    def save_settings(self, d: dict) -> dict:
        try:
            cfg = self._cfg
            for k, v in d.items():
                if not hasattr(cfg, k):
                    continue
                if k in ("server_port", "xconnect_port"):
                    v = int(v)      # 前端键盘输入可能为字符串
                setattr(cfg, k, v)
            cfg.server_host = cfg.server_host.strip() or "ol.xflysim.com"
            cfg.xconnect_host = cfg.xconnect_host.strip() or "127.0.0.1"
            if not (1 <= cfg.server_port <= 65535) or \
                    not (1 <= cfg.xconnect_port <= 65535):
                return {"ok": False, "code": "port_range"}
            save_config(cfg)
            self._cfg = load_config()
            return {"ok": True}
        except ValueError:
            return {"ok": False, "code": "port_number"}

    def set_sim(self, version: str) -> None:
        self._sim_type = pk.SIM_TYPE_XP12 if version == "xp12" else pk.SIM_TYPE_XP11

    def set_debug(self, enabled: bool) -> None:
        """调试模式：开启后前端可查看插件原始报文。"""
        self._debug = bool(enabled)

    # ---------- 连线 / 断开 ----------

    def connect(self, callsign: str, aircraft: str, airline: str) -> dict:
        if self._model is not None:
            return {"ok": False, "code": "already_connected"}
        if not self._xconnect.connected:
            return {"ok": False, "code": "plugin_offline"}
        # 航司由呼号前三位决定（如 CCA1501 → CCA），覆盖手动填写
        callsign = callsign.strip().upper()
        aircraft = aircraft.strip().upper()
        airline = callsign[:3] or airline.strip().upper()
        # 机型兜底：前端传来的值不可用时，用**最后一次插件原始数据**重新推断。
        #
        # 为什么需要这层：前端 `store.aircraft` 可能因为某些时序问题仍是空值
        # （比如刚切机模、推送还没到达，或者前端被清空后没来得及刷新）。
        # 只依赖前端传值会让"明明识别得出来"的机型卡在校验上。
        # 这里拿 `self._last_fd_*` 兜底，保证只要插件读到过机型名就能连。
        if not (2 <= len(aircraft) <= 4 and aircraft.isalnum()):
            fb = aircraft_db.guess_designator(
                getattr(self, "_last_ac_name", "") or "",
                int(getattr(self, "_last_eng", 0) or 0))
            if fb:
                aircraft = fb
        self._cfg.callsign = callsign
        self._cfg.aircraft_icao = aircraft
        self._cfg.airline_icao = airline
        missing = _validate(self._cfg)
        if missing:
            return {"ok": False, "code": "missing_fields", "fields": missing}
        save_config(self._cfg)
        self._cfg = load_config()

        # 注入应用级共享插件客户端（保持监听插件上线，断开 FSD 不影响）
        model = NetworkModel(xconnect=self._xconnect)
        model.aircraft_icao = self._cfg.aircraft_icao
        model.airline_icao = self._cfg.airline_icao
        # 尾号：先按「手动填写 > 同机型上次记录」定值，首包 PI 即可带上；
        # 连线后插件每帧上报 acf_tailnum / 涂装目录，自动覆盖为模拟器值
        model.registration_manual = norm_registration(self._cfg.registration)
        model.registration_cache = dict(self._cfg.registration_cache or {})
        model.on_registration_resolved = self._on_registration_resolved
        model.on_log = lambda m: self._push("log", self._tr(m))
        model.on_text_received = lambda f, m: self._push("text", [f, m])
        model.on_local_flight_data = lambda fd: self._push("flight", asdict(fd))
        model.on_fsd_unexpected_disconnect = lambda: self._on_fsd_dropped(model)
        model.on_xconnect_connected = lambda: self._push("xconnect", "connected")
        model.on_xconnect_disconnected = lambda: self._push("xconnect", "disconnected")
        model.on_plugin_message = self._on_plugin_message
        model.on_csl_status = lambda d: self._push("csl", d)
        model.on_fsd_state_changed = lambda _: self._push("fsdstate", self.status())
        model._apply_registration("", "", model.aircraft_icao)

        sim_type = self._sim_type
        cfg = self._cfg
        self._model = model

        def worker() -> None:
            model.start_fsd(cfg.server_host, cfg.server_port,
                            cfg.callsign, cfg.cid.strip(), cfg.password,
                            cfg.real_name, sim_type)
            self._push("log", self._tr(f"[i] 已发送登录请求，呼号 {cfg.callsign}"))
            # 存在已保存的飞行计划 → 连线后自动提交
            fp = cfg.flight_plan
            if fp and fp.get("dest") and model.fsd:
                try:
                    time.sleep(1.0)  # 等待服务器完成 #AP 认证
                    self._send_flight_plan(model.fsd, fp)
                    self._push("log",
                               self._tr(f"[i] 已自动提交已保存飞行计划 "
                                        f"({fp.get('dep', '')}→{fp.get('dest', '')})"))
                except Exception as exc:
                    self._push("sys", self._tr(f"自动提交飞行计划失败: {exc}"))

        threading.Thread(target=worker, daemon=True).start()
        return {"ok": True}

    def disconnect(self) -> dict:
        model, self._model = self._model, None
        if model is None:
            return {"ok": True}

        def worker() -> None:
            # 仅断开 FSD；X-Plane 数据源为应用级共享，保持监听
            model.stop_fsd()
            self._push("disconnected", None)

        threading.Thread(target=worker, daemon=True).start()
        return {"ok": True}

    def _on_fsd_dropped(self, model: NetworkModel) -> None:
        """FSD 意外断连（含 X-Plane 断开联动）：清理 FSD，复位前端按钮。"""
        if self._model is model:
            self._model = None

        def worker() -> None:
            model.stop_fsd()

        threading.Thread(target=worker, daemon=True).start()
        self._push("sys", self._tr("FSD 连接意外断开"))
        self._push("disconnected", None)

    def _on_plugin_message(self, doc: dict) -> None:
        """调试模式开启时，把插件原始报文推给前端。"""
        if self._debug:
            self._push("xplog", doc)

    def _on_registration_resolved(self, reg: str, src: str, icao: str) -> None:
        """尾号解析结果回调：缓存同机型的自动值 + 通知前端刷新显示。

        只缓存模拟器读到的值（src=tailnum/livery）——手动值不缓存，否则用户清空
        输入后会被旧值回填，"手动 → 自动" 的降级就失效了。
        """
        if reg and src in ("tailnum", "livery") and icao:
            cache = dict(self._cfg.registration_cache or {})
            if cache.get(icao) != reg:
                cache[icao] = reg
                self._cfg.registration_cache = cache
                save_config(self._cfg)
        self._push("registration", {"reg": reg, "src": src})

    # ---------- 消息 / METAR / 机型 ----------

    def send_text(self, to: str, msg: str) -> dict:
        model = self._model
        if model and model.fsd and to.strip() and msg.strip():
            model.fsd.send_text(to.strip().upper(), msg)
            return {"ok": True}
        return {"ok": False, "code": "not_connected_or_empty"}

    def send_freq(self, mhz: float, msg: str) -> dict:
        model = self._model
        if model and model.fsd and msg.strip():
            model.fsd.send_frequency_text(float(mhz), msg)
            return {"ok": True}
        return {"ok": False, "code": "not_connected_or_msg_empty"}

    # ---------- 模拟器反写（RMP 面板按键 → X-Plane） ----------
    #
    # 与 send_freq 的区别：send_freq 发的是 **FSD 文本**（`#TM` 频率消息），
    # 这里是把动作**写进模拟器**（XConnect 插件的 Send:RADIO / Send:XPDR）。
    # 两者互不替代：前者告诉服务器"我在守这个频率"，后者真正改座舱里的无线电。

    def xp_set_xpdr_code(self, code: str) -> dict:
        """设置应答机 4 位码（每位 0-7）。"""
        xc = self._xconnect
        if xc is None:
            return {"ok": False, "code": "xconnect_unavailable"}
        s = str(code).strip()
        if len(s) != 4 or any(c not in "01234567" for c in s):
            return {"ok": False, "code": "bad_squawk"}
        xc.send_transponder_code(s)
        return {"ok": True}

    def xp_set_xpdr_mode(self, mode: int) -> dict:
        """设置应答机模式：0=Off 1=Stby 2=On(A) 3=Alt(C) 4=Test 5=GND
        6=TA only 7=TA/RA。

        这与 X-Plane `transponder_mode` 枚举一致（共 8 个值）。
        插件优先发 sim/transponder/transponder_* 命令，机模面板会跟着动。
        """
        xc = self._xconnect
        if xc is None:
            return {"ok": False, "code": "xconnect_unavailable"}
        m = int(mode)
        if m < 0 or m > 7:
            return {"ok": False, "code": "bad_mode"}
        xc.send_transponder_state(m)
        return {"ok": True}

    def xp_xpdr_ident(self) -> dict:
        """触发应答机 IDENT（瞬时脉冲，无法"取消"）。"""
        xc = self._xconnect
        if xc is None:
            return {"ok": False, "code": "xconnect_unavailable"}
        xc.send_transponder_ident(True)
        return {"ok": True}

    def xp_set_com(self, com: int, mhz: float, active: bool = True) -> dict:
        """设置 COM 频率。com=1..3，mhz 单位 MHz（121.8），active 区分主/备频。"""
        xc = self._xconnect
        if xc is None:
            return {"ok": False, "code": "xconnect_unavailable"}
        c = int(com)
        if c < 1 or c > 3:
            return {"ok": False, "code": "bad_com"}
        mhz1000 = int(round(float(mhz) * 1000))
        # COM 波段 118.000~136.975 MHz。上界是 136975 不是 137000 ——
        # 137.000 是波段预留边界，8.33kHz 间隔下最高合法值就是 136.975。
        if mhz1000 < 118000 or mhz1000 > 136975:
            return {"ok": False, "code": "freq_out_of_band"}
        xc.send_radio_frequency(c, mhz1000, bool(active))
        return {"ok": True}

    def xp_swap_com(self, com: int) -> dict:
        """主备频率互换（走 X-Plane 命令，机模面板会同步）。"""
        xc = self._xconnect
        if xc is None:
            return {"ok": False, "code": "xconnect_unavailable"}
        c = int(com)
        if c < 1 or c > 3:
            return {"ok": False, "code": "bad_com"}
        xc.send_radio_swap(c)
        return {"ok": True}

    def request_metar(self, icao: str) -> dict:
        model = self._model
        if model and model.fsd and icao.strip():
            model.fsd.request_metar(icao.strip().upper())
            return {"ok": True}
        return {"ok": False, "code": "not_connected"}

    def request_pi(self, callsign: str) -> dict:
        model = self._model
        if model and model.fsd and callsign.strip():
            model.fsd.send_plane_info_request(callsign.strip().upper())
            return {"ok": True}
        return {"ok": False, "code": "not_connected"}

    _FP_KEYS = ("type", "aircraft", "cruise_speed", "cruise_alt", "dep",
                "dest", "alt", "plan_time", "actual_time", "enroute_h",
                "enroute_m", "fuel_h", "fuel_m", "remarks", "route")

    def _send_flight_plan(self, fsd, fp: dict) -> None:
        fsd.send_flight_plan(
            flight_type=str(fp.get("type", "I")).upper(),
            aircraft=str(fp.get("aircraft") or self._cfg.aircraft_icao).upper(),
            cruise_speed=str(fp.get("cruise_speed", "450")),
            dep_airport=str(fp.get("dep", "")).upper(),
            plan_time=str(fp.get("plan_time", "0000")),
            actual_time=str(fp.get("actual_time", "0000")),
            cruise_alt=str(fp.get("cruise_alt", "FL350")).upper(),
            dest_airport=str(fp.get("dest", "")).upper(),
            enroute_h=int(fp.get("enroute_h", 0)),
            enroute_m=int(fp.get("enroute_m", 0)),
            fuel_h=int(fp.get("fuel_h", 0)),
            fuel_m=int(fp.get("fuel_m", 0)),
            alt_airport=str(fp.get("alt", "")).upper(),
            remarks=str(fp.get("remarks", "")),
            route=str(fp.get("route", "")).upper(),
        )

    def submit_flight_plan(self, d: dict) -> dict:
        """提交飞行计划（$FP），并保存到本地配置供下次连线自动提交。

        表单里的「注册号」不进入 $FP 报文（FSD 计划无此字段），而是作为本机尾号的
        手动值：非空 → 覆盖自动识别结果并立即重发 `#SB PI`；清空 → 回到自动识别。
        """
        model = self._model
        if not (model and model.fsd):
            return {"ok": False, "code": "not_online"}
        try:
            self._send_flight_plan(model.fsd, d)
        except ValueError:
            return {"ok": False, "code": "plan_field_error"}
        # 提交成功后保存本地
        self._cfg.flight_plan = {k: d[k] for k in self._FP_KEYS if k in d}
        reg = norm_registration(str(d.get("registration", "")))
        self._cfg.registration = reg
        save_config(self._cfg)
        try:
            model.set_registration_manual(reg)
        except Exception as exc:      # 尾号失败不影响计划提交
            self._push("sys", self._tr(f"设置注册号失败: {exc}"))
        return {"ok": True, "saved": True, "registration": model.registration}

    # ---------- SimBrief 导入 ----------

    def _remember_simbrief_user(self, user: str) -> None:
        """记住 SimBrief 用户名/ID。

        关键：**失败也要记**。之前只在成功时保存，导致网络不通 / 用户名打错时
        输入框下次打开就是空的，看起来像"根本没缓存"。
        """
        user = (user or "").strip()
        if not user or user == self._cfg.simbrief_user:
            return
        self._cfg.simbrief_user = user
        try:
            save_config(self._cfg)
        except OSError as exc:
            self._push("sys", self._tr(f"保存 SimBrief 用户名失败: {exc}"))

    def simbrief_import(self, username: str) -> dict:
        """按 SimBrief 用户名/用户 ID 取最新 OFP，返回表单字段（不自动提交）。

        只做「填表」：用户核对/修改后自己点提交，避免拿到一份旧计划就误发 $FP。
        未连线也可用（离线准备计划是常规流程）。
        """
        user = (username or "").strip()
        self._remember_simbrief_user(user)      # 先记输入，失败也不丢
        try:
            plan = simbrief_import_plan(user, timeout=10.0)
        except SimbriefError as exc:
            return {"ok": False, "code": exc.code, "error": exc.detail,
                    "username": user}
        return {"ok": True, "username": user, "plan": plan}

    # ---------- 状态 ----------

    def status(self) -> dict:
        model = self._model
        if model is None:
            reg = norm_registration(self._cfg.registration)
            return {"fsd": "Disconnected",
                    "xconnect": self._xconnect.connected,
                    "atc": 0, "players": 0,
                    "reg": reg, "reg_src": "manual" if reg else ""}
        fsd = model.fsd
        with model._gate:
            atc = len(model._atc)
            players = len(model._players)
        return {
            "fsd": fsd.state if fsd else "Disconnected",
            "online": fsd is not None and fsd.state == STATE_ONLINE,
            "xconnect": self._xconnect.connected,
            "atc": atc,
            "players": players,
            "reg": model.registration,
            "reg_src": model.registration_source,
        }

    # ---------- 窗口控制（frameless 自绘标题栏） ----------

    def win_minimize(self) -> None:
        try:
            webview.windows[0].minimize()
        except Exception:
            pass

    def win_maximize(self) -> None:
        """切换最大化/还原。"""
        try:
            webview.windows[0].toggle_fullscreen()
        except Exception:
            pass

    def win_close(self) -> None:
        try:
            webview.windows[0].destroy()
        except Exception:
            pass

    def close(self) -> None:
        """应用退出：断开 FSD 并停止插件监听。"""
        model, self._model = self._model, None
        if model is not None:
            model.shutdown()
        self._xconnect.stop()
        if self._cfg.mconnect_stop_on_exit:
            self._bridge.stop()


def _find_windows_by_pid(pid: int) -> list:
    """按进程 PID 枚举顶层窗口（frameless 无标题，不能用标题查找）。
    返回 (hwnd, visible, width, height) 列表。"""
    import ctypes
    from ctypes import wintypes

    out = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _lparam):
        wp = wintypes.DWORD()
        ctypes.windll.user32.GetWindowThreadProcessId(hwnd, ctypes.byref(wp))
        if wp.value == pid:
            r = wintypes.RECT()
            ctypes.windll.user32.GetWindowRect(hwnd, ctypes.byref(r))
            out.append({
                "hwnd": hwnd,
                "vis": bool(ctypes.windll.user32.IsWindowVisible(hwnd)),
                "w": r.right - r.left,
                "h": r.bottom - r.top,
            })
        return True

    ctypes.windll.user32.EnumWindows(cb, 0)
    return out


def _set_window_icon_hwnd(hwnd, ico: str) -> None:
    """WM_SETICON 直接写窗柄图标（绕过后端差异；任务管理器按窗口图标显示）。

    - x64 下必须显式声明 argtypes/restype，否则句柄被 32 位截断失效；
    - WinForms 显示/主题变化时会重建窗口句柄，需调用方周期性重设。"""
    import ctypes
    from ctypes import wintypes
    try:
        user32 = ctypes.windll.user32
        user32.LoadImageW.restype = ctypes.c_void_p
        user32.SendMessageW.restype = ctypes.c_void_p
        user32.SendMessageW.argtypes = [
            wintypes.HWND, wintypes.UINT, ctypes.c_void_p, ctypes.c_void_p]
        # IMAGE_ICON=1, LR_LOADFROMFILE=0x10
        big = ctypes.c_void_p(user32.LoadImageW(None, ico, 1, 32, 32, 0x10))
        small = ctypes.c_void_p(user32.LoadImageW(None, ico, 1, 16, 16, 0x10))
        # WM_SETICON=0x0080, ICON_SMALL=0, ICON_BIG=1
        if small.value:
            user32.SendMessageW(hwnd, 0x0080, ctypes.c_void_p(0), small)
        if big.value:
            user32.SendMessageW(hwnd, 0x0080, ctypes.c_void_p(1), big)
    except Exception:
        pass


def _apply_window_icon_async(ico: str) -> None:
    """运行期间持续把图标写到本进程窗口（幂等）。

    webview.start 阻塞；WinForms 会重建句柄，故每 2s 重设一次，
    覆盖句柄重建与后端差异。daemon 线程随进程退出。
    """

    def worker() -> None:
        while True:
            wins = _find_windows_by_pid(os.getpid())
            if wins:
                order = [w for w in wins
                         if w["vis"] and w["w"] >= 400 and w["h"] >= 500]
                for w in (order or wins):
                    _set_window_icon_hwnd(w["hwnd"], ico)
            time.sleep(2.0)

    threading.Thread(target=worker, daemon=True).start()


def run_web(debug: bool = False) -> None:
    from _noise import suppress_noise
    suppress_noise()
    if not os.path.exists(WEBUI_DIST):
        print(f"[!] 未找到前端构建产物：\n    {WEBUI_DIST}\n"
              "    请先构建：\n"
              "        cd webui\n"
              "        npm install\n"
              "        npm run build")
        raise SystemExit(1)
    api = Api()
    try:
        webview.create_window(
            "XShare - XFlySim Client",
            WEBUI_URL,
            width=420, height=577.5, resizable=False,
            js_api=api, background_color="#16181c",
            frameless=True, easy_drag=False,
            confirm_close=False)   # 退出确认由前端 ElMessageBox 完成（原生框会裁字）
        if os.path.exists(ICON_PATH):
            _apply_window_icon_async(ICON_PATH)
        webview.start(debug=debug,
                      icon=ICON_PATH if os.path.exists(ICON_PATH) else None)
    finally:
        api.close()


if __name__ == "__main__":
    import sys
    run_web(debug="--debug" in sys.argv)
