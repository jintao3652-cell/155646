"""XFlySim-Client pywebview 应用入口：HTML/CSS/JS 界面。

后端：NetworkModel（FSD + X-Plane UDP），通过 pywebview js_api 暴露给前端。
前端轮询 api.pull() 拉取事件（日志/文本/飞行数据/状态），避免跨线程 evaluate_js。

运行：python webapp.py
"""

from __future__ import annotations

import os
import threading
from collections import deque
from dataclasses import asdict
from typing import Optional

import webview

from xfly_client.config import AppConfig, load_config, save_config
from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import STATE_ONLINE
from xfly_client.model import NetworkModel
from xfly_client.xconnect.client import XConnectClient

WEBUI_URL = "file:///" + os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "webui", "index.html"
).replace("\\", "/")


def _validate(cfg: AppConfig) -> list[str]:
    missing = []
    if not cfg.callsign.strip():
        missing.append("呼号 callsign")
    if not cfg.cid.strip():
        missing.append("CID")
    if not cfg.password.strip():
        missing.append("密码 password")
    if not cfg.real_name.strip():
        missing.append("真实姓名 realname")
    if len(cfg.aircraft_icao.strip()) != 4:
        missing.append("机型 ICAO (4位)")
    if len(cfg.airline_icao.strip()) != 3:
        missing.append("航司 ICAO (3位)")
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

        # XConnect 插件客户端：应用级生命周期，启动即持续监听插件上线
        self._xconnect = XConnectClient(
            self._cfg.xconnect_host, self._cfg.xconnect_port)
        self._xconnect.on_log = lambda m: self._push("log", m)
        self._xconnect.on_connected = lambda: self._push("xconnect", "connected")
        self._xconnect.on_disconnected = \
            lambda: self._push("xconnect", "disconnected")
        self._start_xconnect()

    def _start_xconnect(self) -> None:
        self._push("xconnect", "waiting")
        self._xconnect.start()  # 持续重连，直到插件上线

    # ---------- 事件推送（前端轮询） ----------

    def _push(self, event: str, payload) -> None:
        self._events.append((event, payload))

    def pull(self) -> list[dict]:
        out: list[dict] = []
        try:
            while self._events:
                e, p = self._events.popleft()
                out.append({"event": e, "data": p})
        except IndexError:
            pass
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
            cfg.server_host = cfg.server_host.strip() or "fsd.flyisfp.com"
            cfg.xconnect_host = cfg.xconnect_host.strip() or "127.0.0.1"
            if not (1 <= cfg.server_port <= 65535) or \
                    not (1 <= cfg.xconnect_port <= 65535):
                return {"ok": False, "error": "端口必须在 1-65535 范围内"}
            save_config(cfg)
            self._cfg = load_config()
            return {"ok": True}
        except ValueError:
            return {"ok": False, "error": "端口必须为数字"}

    def set_sim(self, version: str) -> None:
        self._sim_type = pk.SIM_TYPE_XP12 if version == "xp12" else pk.SIM_TYPE_XP11

    # ---------- 连线 / 断开 ----------

    def connect(self, callsign: str, aircraft: str, airline: str) -> dict:
        if self._model is not None:
            return {"ok": False, "error": "已在连线中"}
        if not self._xconnect.connected:
            return {"ok": False,
                    "error": "XConnect 插件未上线（未连接 51001），禁止连线 FSD"}
        self._cfg.callsign = callsign.strip().upper()
        self._cfg.aircraft_icao = aircraft.strip().upper()
        self._cfg.airline_icao = airline.strip().upper()
        missing = _validate(self._cfg)
        if missing:
            return {"ok": False, "error": "必填项缺失或无效：\n" + "\n".join(missing)}
        save_config(self._cfg)
        self._cfg = load_config()

        # 注入应用级共享插件客户端（保持监听插件上线，断开 FSD 不影响）
        model = NetworkModel(xconnect=self._xconnect)
        model.aircraft_icao = self._cfg.aircraft_icao
        model.airline_icao = self._cfg.airline_icao
        model.on_log = lambda m: self._push("log", m)
        model.on_text_received = lambda f, m: self._push("text", [f, m])
        model.on_local_flight_data = lambda fd: self._push("flight", asdict(fd))
        model.on_fsd_unexpected_disconnect = lambda: self._on_fsd_dropped(model)
        model.on_xconnect_connected = lambda: self._push("xconnect", "connected")
        model.on_xconnect_disconnected = lambda: self._push("xconnect", "disconnected")

        sim_type = self._sim_type
        cfg = self._cfg
        self._model = model

        def worker() -> None:
            model.start_fsd(cfg.server_host, cfg.server_port,
                            cfg.callsign, cfg.cid.strip(), cfg.password,
                            cfg.real_name, sim_type)
            self._push("log", f"[i] 已发送登录请求，呼号 {cfg.callsign}")

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
        self._push("sys", "FSD 连接意外断开")
        self._push("disconnected", None)

    # ---------- 消息 / METAR / 机型 ----------

    def send_text(self, to: str, msg: str) -> dict:
        model = self._model
        if model and model.fsd and to.strip() and msg.strip():
            model.fsd.send_text(to.strip().upper(), msg)
            return {"ok": True}
        return {"ok": False, "error": "未连接或参数为空"}

    def send_freq(self, mhz: float, msg: str) -> dict:
        model = self._model
        if model and model.fsd and msg.strip():
            model.fsd.send_frequency_text(float(mhz), msg)
            return {"ok": True}
        return {"ok": False, "error": "未连接或消息为空"}

    def request_metar(self, icao: str) -> dict:
        model = self._model
        if model and model.fsd and icao.strip():
            model.fsd.request_metar(icao.strip().upper())
            return {"ok": True}
        return {"ok": False, "error": "未连接"}

    def request_pi(self, callsign: str) -> dict:
        model = self._model
        if model and model.fsd and callsign.strip():
            model.fsd.send_plane_info_request(callsign.strip().upper())
            return {"ok": True}
        return {"ok": False, "error": "未连接"}

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

    def close(self) -> None:
        """应用退出：断开 FSD 并停止插件监听。"""
        model, self._model = self._model, None
        if model is not None:
            model.shutdown()
        self._xconnect.stop()


def run_web(debug: bool = False) -> None:
    from _noise import suppress_noise
    suppress_noise()
    api = Api()
    try:
        webview.create_window(
            "XFlySim-Client - ISFP 连飞",
            WEBUI_URL,
            width=980, height=460, resizable=True,
            js_api=api, background_color="#16181c",
            confirm_close=True)
        webview.start(debug=debug)
    finally:
        api.close()


if __name__ == "__main__":
    import sys
    run_web(debug="--debug" in sys.argv)
