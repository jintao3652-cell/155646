"""XFlySim-Client CLI 入口。

用法示例：
  python main.py --callsign CCA1234 --cid 1001 --password xxxx --realname "张三" --aircraft A20N --airline CCA --sim xp12
  python main.py --selftest

连线必填项（缺一拒绝连线）：呼号、CID、密码、真实姓名、机型 ICAO、航司 ICAO。
飞行数据来源：X-Plane 11/12 原生 UDP RREF 协议（无需插件）。

X-Plane 设置：设置 → 数据输出 → 开启 UDP 协议（X-Plane 默认监听 49000 端口
接收 RREF 订阅，响应自动回传客户端源端口，无需额外配置）。
"""

from __future__ import annotations

import argparse
import os
import sys
import time

from xfly_client.config import AppConfig, load_config, save_config
from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import STATE_ONLINE
from xfly_client.model import NetworkModel
from xfly_client.registration import norm_registration
from xfly_client.xconnect.client import FlightData

SIM_TYPES = {"xp11": pk.SIM_TYPE_XP11, "xp12": pk.SIM_TYPE_XP12}


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
    if not _valid_icao(cfg.aircraft_icao):
        missing.append("机型 ICAO (4位)")
    if not _valid_icao(cfg.airline_icao):
        missing.append("航司 ICAO (3位)")
    return missing


def _valid_icao(v: str, length: int | None = None) -> bool:
    v = v.strip().upper()
    if not v or not v.isalnum():
        return False
    return length is None or len(v) == length


class CliApp:
    def __init__(self, cfg: AppConfig, sim_type: int) -> None:
        self.cfg = cfg
        self.sim_type = sim_type
        self.model = NetworkModel()
        self.model.aircraft_icao = cfg.aircraft_icao.strip().upper()
        self.model.airline_icao = cfg.airline_icao.strip().upper()
        # 尾号：--registration 优先，否则自动（模拟器 → 同机型上次值 → 空）
        self.model.registration_manual = norm_registration(cfg.registration)
        self.model.registration_cache = dict(cfg.registration_cache or {})
        self.model._apply_registration("", "", self.model.aircraft_icao)
        self.model.on_log = lambda m: print(f"{m}")
        self.model.on_text_received = lambda f, m: print(f"\n[消息] {f}: {m}")
        self.model.on_fsd_unexpected_disconnect = \
            lambda: print("\n[!] FSD 连接意外断开")
        self._last_fd: FlightData | None = None

    def run(self) -> None:
        missing = _validate(self.cfg)
        if missing:
            print("连线参数缺失，拒绝连线：")
            for item in missing:
                print(f"  - 缺少 {item}")
            print("请通过命令行参数或 config.json 补齐后重试。")
            return

        print("== XFlySim-Client ==")
        print(f"  桥接插件:       XConnect {self.cfg.xconnect_port} / "
              f"MConnect {self.cfg.mconnect_port}（其一在线即可）")
        print(f"  FSD 服务器:     {self.cfg.server_host}:{self.cfg.server_port}")
        save_config(self.cfg)

        # 监听桥接插件上线：XConnect(51001) / MConnect(51002/51003/51004)
        self.model.start_xconnect(self.cfg.xconnect_host, self.cfg.xconnect_port,
                                  self.cfg.mconnect_port)
        print(f"[i] 等待模拟机上线（XConnect {self.cfg.xconnect_port} / "
              f"MConnect {self.cfg.mconnect_port} 其一即可）…")
        waited = 0.0
        while not self.model.xconnect.connected and waited < 10.0:
            time.sleep(0.5)
            waited += 0.5
        if not self.model.xconnect.connected:
            print("[!] 未发现任何在线模拟机，禁止连线 FSD。"
                  "请确认 X-Plane(XConnect) 或 MSFS(MConnect) 已运行、插件已加载。")
            return
        print(f"[i] 模拟机已上线（端口 {self.model.xconnect.active_port}），"
              "数据通道就绪")
        self.model.on_local_flight_data = self._on_flight_data

        # 连接 FSD
        self.model.start_fsd(self.cfg.server_host, self.cfg.server_port,
                             self.cfg.callsign.strip().upper(), self.cfg.cid.strip(),
                             self.cfg.password, self.cfg.real_name, self.sim_type)
        print(f"[i] 已发送登录请求，呼号 {self.cfg.callsign.strip().upper()}")

        self._help()
        try:
            while True:
                try:
                    line = input("> ").strip()
                except (EOFError, KeyboardInterrupt):
                    print()
                    break
                if not line:
                    continue
                if not self._handle(line):
                    break
        finally:
            print("正在断开连接...")
            self.model.shutdown()

    # ---------- 命令 ----------

    def _handle(self, line: str) -> bool:
        cmd, _, rest = line.partition(" ")
        cmd = cmd.lower()
        if cmd in ("/quit", "/exit", "/q"):
            return False
        if cmd == "/help":
            self._help()
        elif cmd == "/status":
            self._status()
        elif cmd == "/text":
            to, _, msg = rest.replace(":", " ", 1).partition(" ")
            if to and msg:
                self.model.fsd.send_text(to, msg)
            else:
                print("用法: /text <呼号> <消息>")
        elif cmd == "/freq":
            mhz_s, _, msg = rest.partition(" ")
            try:
                mhz = float(mhz_s)
            except ValueError:
                print("用法: /freq <频率MHz> <消息>")
                return True
            if msg:
                self.model.fsd.send_frequency_text(mhz, msg)
            else:
                print("用法: /freq <频率MHz> <消息>")
        elif cmd == "/metar":
            if rest.strip():
                self.model.fsd.request_metar(rest.strip().upper())
            else:
                print("用法: /metar <ICAO>")
        elif cmd == "/pi":
            if rest.strip():
                self.model.fsd.send_plane_info_request(rest.strip().upper())
            else:
                print("用法: /pi <呼号>")
        else:
            print(f"未知命令: {cmd}（输入 /help 查看帮助）")
        return True

    def _help(self) -> None:
        print("""
命令:
  /text <呼号> <消息>   私聊
  /freq <MHz> <消息>    频率通话 (如 121.800)
  /metar <ICAO>         查询 METAR (如 ZGGG)
  /pi <呼号>            请求对方机型信息
  /status               查看连接与飞行状态
  /help                 帮助
  /quit                 断开并退出
""".strip())

    def _status(self) -> None:
        fsd = self.model.fsd
        state = fsd.state if fsd else "无会话"
        xc = self.model.xconnect
        bridge = (f"在线[{xc.plugin or '?'}:{xc.active_port}]" if xc.connected
                  else "离线")
        print(f"FSD 状态: {state}  模拟机桥接: {bridge}")
        if state == STATE_ONLINE:
            fd = self._last_fd
            if fd is not None:
                f1 = fd.com1_freq
                f2 = fd.com2_freq
                print(f"  频率 COM1 {pk.millihertz_to_display(f1)}  "
                      f"COM2 {pk.millihertz_to_display(f2)}  "
                      f"应答机 {fd.transponder:04d}")
                print(f"  地速 {fd.groundspeed:.0f} kt  高度 "
                      f"{fd.altitude_msl:.0f} ft  "
                      f"航向 {fd.true_heading:.0f}")
        with self.model._gate:
            print(f"  在线管制 {len(self.model._atc)}  在线玩家 "
                  f"{len(self.model._players)}")

    # ---------- 飞行数据回调 ----------

    def _on_flight_data(self, fd: FlightData) -> None:
        self._last_fd = fd


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        prog="xflysim-client", description="XShare - XFlySim Client")
    parser.add_argument("--selftest", action="store_true", help="运行协议层自测")
    parser.add_argument("--host", help="FSD 服务器地址")
    parser.add_argument("--port", type=int, help="FSD 服务器端口")
    parser.add_argument("--xp-host", "--xconnect-host", dest="xp_host",
                        help="XConnect 插件地址 (默认 127.0.0.1)")
    parser.add_argument("--xp-port", "--xconnect-port", dest="xp_port",
                        type=int, help="XConnect 插件端口 (默认 51001)")
    parser.add_argument("--callsign", help="呼号，如 CCA1234")
    parser.add_argument("--cid", help="平台数字 ID")
    parser.add_argument("--password", help="平台密码")
    parser.add_argument("--realname", help="真实姓名")
    parser.add_argument("--aircraft", help="机型 ICAO，4 位 (A320/B738/C172)")
    parser.add_argument("--airline", help="航司 ICAO（涂装），如 CCA/CES")
    parser.add_argument("--registration",
                        help="注册号（尾号），如 B-1234；留空自动识别")
    parser.add_argument("--sim", choices=["xp11", "xp12"], default="xp12",
                        help="X-Plane 版本（默认 xp12）")
    args = parser.parse_args(argv)

    if args.selftest:
        from xfly_client.selftest import run_all
        return run_all()

    if sys.version_info < (3, 10):
        print("需要 Python 3.10+")
        return 1

    cfg = load_config()
    for attr, arg in (("server_host", args.host),
                      ("xconnect_host", args.xp_host)):
        if arg:
            setattr(cfg, attr, arg)
    if args.port:
        cfg.server_port = args.port
    if args.xp_port:
        cfg.xconnect_port = args.xp_port
    for attr, arg in (("callsign", args.callsign), ("cid", args.cid),
                      ("password", args.password), ("real_name", args.realname),
                      ("aircraft_icao", args.aircraft),
                      ("airline_icao", args.airline),
                      ("registration", args.registration)):
        if arg:
            setattr(cfg, attr, arg)

    bridge = None
    if cfg.auto_start_mconnect:
        from xfly_client.bridge_launcher import BridgeLauncher
        bridge = BridgeLauncher(
            os.path.dirname(os.path.abspath(__file__)),
            on_log=lambda m: print(f"[bridge] {m}"),
            toggle_labels=getattr(cfg, "msfs_toggle_labels", False),
            traffic_lib=getattr(cfg, "msfs_traffic_lib", "auto"))
        bridge.start()
    try:
        app = CliApp(cfg, SIM_TYPES[args.sim])
        app.run()
    finally:
        if bridge is not None and cfg.mconnect_stop_on_exit:
            bridge.stop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())