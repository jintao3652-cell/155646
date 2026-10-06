"""XFlySim-Client pywebview 应用入口：HTML/CSS/JS 界面。

后端：NetworkModel（FSD + X-Plane UDP），通过 pywebview js_api 暴露给前端。
前端回调 pollEvent 用 api.pull() 拉取事件（日志/文本/飞行数据/状态），避免跨线程 evaluate_js。

运行：python webapp.py
"""

from __future__ import annotations

import os
import threading
import time
from collections import deque
from dataclasses import asdict
from typing import Optional

import webview

from xfly_client.config import AppConfig, load_config, save_config
from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import STATE_ONLINE
from xfly_client.i18n import translate
from xfly_client.model import NetworkModel
from xfly_client.xconnect.client import XConnectClient

WEBUI_DIR = os.path.dirname(os.path.abspath(__file__))
WEBUI_DIST = os.path.join(WEBUI_DIR, "webui", "dist", "index.html")
WEBUI_URL = "file:///" + WEBUI_DIST.replace("\\", "/")


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
    if len(cfg.aircraft_icao.strip()) != 4:
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

        # XConnect 插件客户端：应用级生命周期，启动即持续监听插件上线
        self._xconnect = XConnectClient(
            self._cfg.xconnect_host, self._cfg.xconnect_port)
        self._xconnect.on_log = lambda m: self._push("log", self._tr(m))
        self._xconnect.on_connected = lambda: self._push("xconnect", "connected")
        self._xconnect.on_disconnected = \
            lambda: self._push("xconnect", "disconnected")
        # 未连线 FSD 也接收并显示飞行数据（连线后由 NetworkModel 接管上报）
        self._xconnect.on_flight_data = lambda fd: self._push("flight", asdict(fd))
        self._xconnect.on_csl_status = lambda d: self._push("csl", d)
        self._start_xconnect()

    def _start_xconnect(self) -> None:
        self._push("xconnect", "waiting")
        self._xconnect.start()  # 持续重连，直到插件上线

    # ---------- 事件推送（前端回调） ----------

    def _tr(self, msg: str) -> str:
        """按保存的语言翻译后端日志/提示文案。"""
        return translate(self._cfg.language, msg)

    def _push(self, event: str, payload) -> None:
        self._events.append((event, payload))
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
                if hasattr(cfg, k):
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
        model.on_log = lambda m: self._push("log", self._tr(m))
        model.on_text_received = lambda f, m: self._push("text", [f, m])
        model.on_local_flight_data = lambda fd: self._push("flight", asdict(fd))
        model.on_fsd_unexpected_disconnect = lambda: self._on_fsd_dropped(model)
        model.on_xconnect_connected = lambda: self._push("xconnect", "connected")
        model.on_xconnect_disconnected = lambda: self._push("xconnect", "disconnected")
        model.on_plugin_message = self._on_plugin_message
        model.on_csl_status = lambda d: self._push("csl", d)
        model.on_fsd_state_changed = lambda _: self._push("fsdstate", self.status())

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
        """提交飞行计划（$FP），并保存到本地配置供下次连线自动提交。"""
        model = self._model
        if not (model and model.fsd):
            return {"ok": False, "code": "not_online"}
        try:
            self._send_flight_plan(model.fsd, d)
        except ValueError:
            return {"ok": False, "code": "plan_field_error"}
        # 提交成功后保存本地
        self._cfg.flight_plan = {k: d[k] for k in self._FP_KEYS if k in d}
        save_config(self._cfg)
        return {"ok": True, "saved": True}

    # ---------- 状态 ----------

    def status(self) -> dict:
        model = self._model
        if model is None:
            return {"fsd": "Disconnected",
                    "xconnect": self._xconnect.connected,
                    "atc": 0, "players": 0}
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
            confirm_close=True)
        webview.start(debug=debug)
    finally:
        api.close()


if __name__ == "__main__":
    import sys
    run_web(debug="--debug" in sys.argv)
