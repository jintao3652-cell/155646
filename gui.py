"""XFlySim-Client Tkinter 深色界面（免第三方依赖）。

主 bar：呼号 / 机型 / 航司 + 连线/断开 + 设置 + 消息面板。
状态栏：FSD / X-Plane 数据状态、频率、应答机、地速、高度。
消息面板：文本收发（私聊/频率）。
飞行数据：X-Plane 11/12 原生 UDP RREF 协议（无需插件）。
"""

from __future__ import annotations

import queue
import threading
import tkinter as tk
from tkinter import messagebox, ttk

from xfly_client.config import AppConfig, load_config, save_config
from xfly_client.fsd import packet as pk
from xfly_client.fsd.session import STATE_ONLINE
from xfly_client.xconnect.client import FlightData, XConnectClient

BG = "#1e1e1e"
BG2 = "#2b2b2b"
FG = "#e6e6e6"
FG_DIM = "#9a9a9a"
ACCENT = "#2d6fd3"
BAD = "#c0392b"
GOOD = "#27ae60"

SIM_TYPES = {"X-Plane 12": pk.SIM_TYPE_XP12, "X-Plane 11": pk.SIM_TYPE_XP11}


class XflyGui:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.cfg = load_config()
        self.model = None
        self._last_fd: FlightData | None = None
        self._q: queue.Queue = queue.Queue()
        self._msg_win = None
        self._settings_win = None

        root.title("XFlySim Client - XFlySim全球连线飞行平台")
        root.configure(bg=BG)
        root.geometry("960x120")
        root.minsize(760, 110)
        self._style = ttk.Style(root)
        try:
            self._style.theme_use("clam")
        except tk.TclError:
            pass

        self._build_bar()
        self._build_status()
        root.after(100, self._pump)

        # XConnect 插件客户端：应用级生命周期，启动即持续监听插件上线
        self._xconnect = XConnectClient(
            self.cfg.xconnect_host, self.cfg.xconnect_port)
        self._xconnect.on_log = lambda m: self._post("log", m)
        # 未连线也显示飞行数据（连线后由 NetworkModel 接管）
        self._xconnect.on_flight_data = lambda fd: self._post("flight", fd)
        self._start_xconnect()

    def _start_xconnect(self) -> None:
        self._post("xconnect", "waiting")
        self._xconnect.start()  # 持续重连，直到插件上线

    # ---------- 组件 ----------

    def _style_widgets(self, *widgets: tk.Widget) -> None:
        for w in widgets:
            if isinstance(w, tk.Entry):
                w.configure(bg=BG2, fg=FG, insertbackground=FG,
                            relief=tk.FLAT, highlightthickness=1,
                            highlightbackground="#3d3d3d")
            elif isinstance(w, (tk.Button, tk.Label)):
                w.configure(bg=BG, fg=FG)

    def _build_bar(self) -> None:
        bar = tk.Frame(self.root, bg=BG)
        bar.pack(fill=tk.X, padx=8, pady=6)

        tk.Label(bar, text="XFlySim-Client", bg=BG, fg=ACCENT,
                 font=("Segoe UI", 11, "bold")).pack(side=tk.LEFT, padx=(0, 12))

        self.callsign_var = tk.StringVar(value=self.cfg.callsign)
        self.aircraft_var = tk.StringVar(value=self.cfg.aircraft_icao)
        self.airline_var = tk.StringVar(value=self.cfg.airline_icao)
        self.sim_var = tk.StringVar(value="X-Plane 12")

        for label, var, width, tool in (
                ("呼号", self.callsign_var, 10, "如 CCA1234"),
                ("机型", self.aircraft_var, 7, "由模拟器自动识别"),
                ("航司ICAO", self.airline_var, 7, "涂装，如 CCA")):
            tk.Label(bar, text=label, bg=BG, fg=FG_DIM,
                     font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=(6, 3))
            e = tk.Entry(bar, textvariable=var, width=width,
                         font=("Consolas", 10),
                         **({"state": "readonly"} if var is self.aircraft_var else {}))
            self._style_widgets(e)
            e.pack(side=tk.LEFT)
            e.bind("<Return>", lambda _e: self._connect())

        sim_cb = ttk.Combobox(bar, textvariable=self.sim_var, width=12,
                              values=list(SIM_TYPES), state="readonly")
        sim_cb.pack(side=tk.LEFT, padx=(8, 0))

        self._connect_btn = tk.Button(bar, text="✦ 连线", fg="#fff",
                                      bg=ACCENT, activebackground="#245bb3",
                                      activeforeground="#fff", relief=tk.FLAT,
                                      font=("Segoe UI", 10, "bold"),
                                      command=self._connect)
        self._connect_btn.pack(side=tk.LEFT, padx=10)

        tk.Button(bar, text="⚙ 设置", fg=FG, bg=BG2, relief=tk.FLAT,
                  activebackground="#383838", activeforeground=FG,
                  font=("Segoe UI", 10), command=self._open_settings
                  ).pack(side=tk.LEFT, padx=(0, 6))
        tk.Button(bar, text="💬 消息", fg=FG, bg=BG2, relief=tk.FLAT,
                  activebackground="#383838", activeforeground=FG,
                  font=("Segoe UI", 10), command=self._open_messages
                  ).pack(side=tk.LEFT, padx=(0, 6))
        tk.Button(bar, text="⏻ 断开", fg=FG, bg=BG2, relief=tk.FLAT,
                  activebackground="#383838", activeforeground=FG,
                  font=("Segoe UI", 10), command=self._disconnect
                  ).pack(side=tk.LEFT)

    def _build_status(self) -> None:
        frame = tk.Frame(self.root, bg=BG2, height=34)
        frame.pack(fill=tk.X, side=tk.BOTTOM)
        frame.pack_propagate(False)

        self.fsd_state_var = tk.StringVar(value="FSD: —")
        self.xplane_state_var = tk.StringVar(value="插件: —")
        self.freq_var = tk.StringVar(value="COM1 ——  COM2 ——")
        self.xpdr_var = tk.StringVar(value="应答机 ————")
        self.speed_var = tk.StringVar(value="地速 — kn")
        self.alt_var = tk.StringVar(value="高度 — ft")

        tk.Label(frame, textvariable=self.fsd_state_var, bg=BG2, fg=FG_DIM,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=8)
        tk.Label(frame, textvariable=self.xplane_state_var, bg=BG2, fg=FG_DIM,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=8)
        tk.Label(frame, textvariable=self.freq_var, bg=BG2, fg=FG,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=8)
        tk.Label(frame, textvariable=self.xpdr_var, bg=BG2, fg=FG,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=8)
        tk.Label(frame, textvariable=self.speed_var, bg=BG2, fg=FG,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=8)
        tk.Label(frame, textvariable=self.alt_var, bg=BG2, fg=FG,
                 font=("Consolas", 9)).pack(side=tk.LEFT, padx=8)

    # ---------- 连线 / 断开 ----------

    def _connect(self) -> None:
        if self.model is not None:
            return
        callsign = self.callsign_var.get().strip().upper()
        aircraft = self.aircraft_var.get().strip().upper()
        airline = self.airline_var.get().strip().upper()

        missing = []
        if not callsign:
            missing.append("呼号")
        # 机型 ICAO 是 **2~4 位**，不是固定 4 位（ICAO Doc 8643：
        # "代码由最多四个字符组成"）。写死 4 位会拒掉库里 2126 条
        # 合法代码（2 位 433 条、3 位 1693 条），表现为"机型 ICAO
        # （等待模拟器识别）"—— 但模拟器其实已经识别出来了。
        if not (2 <= len(aircraft) <= 4 and aircraft.isalnum()):
            missing.append("机型 ICAO（2~4 位，等待模拟器识别）")
        if len(airline) != 3 or not airline.isalnum():
            missing.append("航司 ICAO（3 位）")
        if not self.cfg.real_name.strip():
            missing.append("真实姓名（设置）")
        if not self.cfg.cid.strip():
            missing.append("CID（设置）")
        if not self.cfg.password.strip():
            missing.append("密码（设置）")
        if missing:
            messagebox.showerror(
                "拒绝连线", "以下必填项缺失或无效：\n" + "\n".join(f"  · {m}" for m in missing))
            return

        self.cfg.callsign = callsign
        self.cfg.aircraft_icao = aircraft
        self.cfg.airline_icao = airline
        save_config(self.cfg)

        if not self._xconnect.connected:
            messagebox.showerror(
                "拒绝连线", "XConnect 插件未上线（未连接 51001），禁止连线 FSD。\n"
                           "请确认 X-Plane 已运行且 XConnect 插件已安装。")
            return

        from xfly_client.model import NetworkModel
        from xfly_client.registration import norm_registration
        self.model = NetworkModel(xconnect=self._xconnect)
        self.model.aircraft_icao = aircraft
        self.model.airline_icao = airline
        # 尾号：手动填写 > 模拟器（插件上报）> 同机型上次记录 > 空
        self.model.registration_manual = norm_registration(self.cfg.registration)
        self.model.registration_cache = dict(self.cfg.registration_cache or {})
        self.model._apply_registration("", "", aircraft)
        self.model.on_log = lambda m: self._post("log", m)
        self.model.on_text_received = lambda f, m: self._post("text", f, m)
        self.model.on_local_flight_data = lambda fd: self._post("flight", fd)
        self.model.on_fsd_unexpected_disconnect = self._on_fsd_dropped
        self.model.on_xconnect_connected = \
            lambda: self._post("xconnect", "connected")
        self.model.on_xconnect_disconnected = \
            lambda: self._post("xconnect", "disconnected")

        sim_type = SIM_TYPES[self.sim_var.get()]

        def worker() -> None:
            self.model.start_fsd(self.cfg.server_host, self.cfg.server_port,
                                 callsign, self.cfg.cid.strip(), self.cfg.password,
                                 self.cfg.real_name, sim_type)

        threading.Thread(target=worker, daemon=True).start()
        self._connect_btn.configure(state=tk.DISABLED,
                                    text="连结中...")

    def _disconnect(self) -> None:
        if self.model is None:
            return

        def worker() -> None:
            self.model.stop_fsd()
            self.model = None
            self._post("disconnected")

        self._connect_btn.configure(text="断开中...")
        threading.Thread(target=worker, daemon=True).start()

    def _on_fsd_dropped(self) -> None:
        """FSD 意外断连（含 X-Plane 断开联动）：清理 FSD，复位按钮。
        X-Plane 数据源为应用级共享，保持监听。"""
        model, self.model = self.model, None

        def worker() -> None:
            if model is not None:
                model.stop_fsd()

        threading.Thread(target=worker, daemon=True).start()
        self._post("sys", "FSD 连接意外断开")
        self._post("disconnected")

    # ---------- 设置 / 消息窗口 ----------

    def _open_settings(self) -> None:
        if self._settings_win is not None and self._settings_win.winfo_exists():
            self._settings_win.lift()
            return
        win = tk.Toplevel(self.root, bg=BG)
        win.title("设置")
        win.geometry("440x400")
        win.transient(self.root)

        cfg = self.cfg
        vars_: dict[str, tk.StringVar] = {}
        groups: list[tk.LabelFrame] = []

        def add_group(title: str, fields: list[tuple[str, str]]) -> None:
            frame = tk.LabelFrame(win, text=title, bg=BG, fg=FG_DIM,
                                  font=("Segoe UI", 9, "bold"),
                                  highlightbackground="#3d3d3d")
            frame.grid(row=len(groups), column=0, columnspan=2,
                       sticky="ew", padx=10, pady=6)
            groups.append(frame)
            for i, (label, attr) in enumerate(fields):
                tk.Label(frame, text=label, bg=BG, fg=FG_DIM,
                         font=("Segoe UI", 10)).grid(row=i, column=0,
                                                     sticky="w", padx=8, pady=4)
                v = tk.StringVar(value=str(getattr(cfg, attr)))
                e = tk.Entry(frame, textvariable=v, width=22,
                             show="*" if attr == "password" else None)
                self._style_widgets(e)
                e.grid(row=i, column=1, padx=8, pady=4)
                vars_[attr] = v

        add_group("账号", [
            ("真实姓名", "real_name"),
            ("CID", "cid"),
            ("密码", "password"),
        ])
        add_group("飞行信息", [
            ("呼号", "callsign"),
            ("机型 ICAO", "aircraft_icao"),
            ("航司 ICAO", "airline_icao"),
            ("注册号(空=自动)", "registration"),
        ])
        add_group("服务器", [
            ("FSD 服务器", "server_host"),
            ("FSD 端口", "server_port"),
            ("插件地址", "xconnect_host"),
            ("插件端口", "xconnect_port"),
        ])

        remember = tk.BooleanVar(value=cfg.remember_password)
        tk.Checkbutton(win, text="记住密码", variable=remember, bg=BG, fg=FG,
                       activebackground=BG, activeforeground=FG,
                       selectcolor=BG2, font=("Segoe UI", 9)
                       ).grid(row=3, column=0, columnspan=2, sticky="w", padx=12)

        tip = tk.Label(win, text="提示: 客户端连接 X-Plane 内的 XConnect 插件\n"
                                 "（默认 127.0.0.1:51001），插件离线时禁止连线",
                       bg=BG, fg="#c9a227", font=("Segoe UI", 8),
                       justify=tk.LEFT)
        tip.grid(row=4, column=0, columnspan=2, sticky="w", padx=12)

        def save() -> None:
            cfg.real_name = vars_["real_name"].get().strip()
            cfg.cid = vars_["cid"].get().strip()
            if remember.get():
                cfg.password = vars_["password"].get()
            else:
                cfg.password = ""
            cfg.remember_password = remember.get()

            cfg.callsign = vars_["callsign"].get().strip().upper()
            cfg.aircraft_icao = vars_["aircraft_icao"].get().strip().upper()
            cfg.airline_icao = vars_["airline_icao"].get().strip().upper()
            cfg.registration = vars_["registration"].get().strip().upper()

            cfg.server_host = vars_["server_host"].get().strip() or "ol.xflysim.com"
            cfg.xconnect_host = vars_["xconnect_host"].get().strip() or "127.0.0.1"
            try:
                cfg.server_port = int(vars_["server_port"].get())
                cfg.xconnect_port = int(vars_["xconnect_port"].get())
            except ValueError:
                messagebox.showerror("设置", "端口必须为数字", parent=win)
                return
            if not (1 <= cfg.server_port <= 65535 and
                    1 <= cfg.xconnect_port <= 65535):
                messagebox.showerror("设置", "端口必须在 1-65535 范围内", parent=win)
                return

            self.cfg = load_config()
            for attr in ("callsign", "real_name", "cid", "password",
                         "aircraft_icao", "airline_icao", "registration",
                         "remember_password",
                         "server_host", "server_port",
                         "xconnect_host", "xconnect_port"):
                if hasattr(cfg, attr):
                    setattr(self.cfg, attr, getattr(cfg, attr))
            save_config(self.cfg)
            self.callsign_var.set(self.cfg.callsign)
            self.aircraft_var.set(self.cfg.aircraft_icao)
            self.airline_var.set(self.cfg.airline_icao)
            messagebox.showinfo("设置", "已保存 (config.json)", parent=win)
            win.destroy()

        tk.Button(win, text="保存", fg="#fff", bg=ACCENT, relief=tk.FLAT,
                  font=("Segoe UI", 10, "bold"), command=save
                  ).grid(row=5, column=0, columnspan=2, pady=10)
        win.protocol("WM_DELETE_WINDOW", win.destroy)
        self._settings_win = win

    def _open_messages(self) -> None:
        if self._msg_win is not None and self._msg_win.winfo_exists():
            self._msg_win.lift()
            return
        win = tk.Toplevel(self.root, bg=BG)
        win.title("消息")
        win.geometry("520x400")
        win.transient(self.root)

        hist = tk.Text(win, bg=BG2, fg=FG, relief=tk.FLAT, wrap=tk.WORD,
                       state=tk.DISABLED, font=("Segoe UI", 10))
        hist.pack(fill=tk.BOTH, expand=True, padx=8, pady=(8, 4))

        row = tk.Frame(win, bg=BG)
        row.pack(fill=tk.X, padx=8, pady=4)
        tk.Label(row, text="收件(呼号/频率)", bg=BG, fg=FG_DIM,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT)
        to_var = tk.StringVar(value="")
        to_entry = tk.Entry(row, textvariable=to_var, width=14, font=("Consolas", 10))
        self._style_widgets(to_entry)
        to_entry.pack(side=tk.LEFT, padx=6)

        msg_var = tk.StringVar()
        msg_entry = tk.Entry(row, textvariable=msg_var, font=("Segoe UI", 10))
        self._style_widgets(msg_entry)
        msg_entry.pack(side=tk.LEFT, fill=tk.X, expand=True)

        def send_msg(_e=None) -> None:
            to = to_var.get().strip()
            msg = msg_var.get().strip()
            if not (to and msg and self.model and self.model.fsd):
                return
            if to.startswith("@"):
                try:
                    mhz = float(to[1:])
                    self.model.fsd.send_frequency_text(mhz, msg)
                    self._append_msg(hist, f"→ 频率 {to}  {msg}")
                except ValueError:
                    self._append_msg(hist, "[!] 频率格式错误，应为 @121.800")
            else:
                target = to.upper()
                self.model.fsd.send_text(target, msg)
                self._append_msg(hist, f"→ {target}  {msg}")
            msg_var.set("")

        tk.Button(row, text="发送", fg="#fff", bg=ACCENT, relief=tk.FLAT,
                  font=("Segoe UI", 10, "bold"), command=send_msg).pack(side=tk.LEFT)
        msg_entry.bind("<Return>", send_msg)

        win.protocol("WM_DELETE_WINDOW", win.destroy)
        self._msg_win = win
        self._hist_widgets = (hist,)

    # ---------- 线程 → UI ----------

    def _post(self, kind: str, *args) -> None:
        self._q.put((kind, args))

    def _pump(self) -> None:
        try:
            while True:
                kind, args = self._q.get_nowait()
                if kind == "log":
                    self._show_log(args[0])
                elif kind == "text":
                    self._show_text(args[0], args[1])
                elif kind == "xconnect":
                    self._set_xconnect_state(args[0])
                elif kind == "flight":
                    self._last_fd = args[0]
                    icao = getattr(args[0], "aircraft_icao", "")
                    if icao and icao != self.aircraft_var.get():
                        self.aircraft_var.set(icao)
                        self.cfg.aircraft_icao = icao
                elif kind == "sys":
                    self._show_log(f"[!] {args[0]}")
                elif kind == "disconnected":
                    self.fsd_state_var.set("FSD: —")
                    self._connect_btn.configure(state=tk.NORMAL, text="✦ 连线")
                    self._show_log("已断开连接")
        except queue.Empty:
            pass
        self._update_flight_ui()
        self.root.after(100, self._pump)

    def _show_log(self, msg: str) -> None:
        print(msg)

    def _set_xconnect_state(self, state: str) -> None:
        if state == "connected":
            self.xplane_state_var.set("插件: 已上线")
        elif state == "waiting":
            self.xplane_state_var.set("插件: 等待上线…")
        elif state == "failed":
            self.xplane_state_var.set("插件: 未上线")
            self._show_log("[xconnect] 无法连接插件，请检查 X-Plane 是否运行")
        elif state == "disconnected":
            self.xplane_state_var.set("插件: 离线")

    def _show_text(self, fr: str, msg: str) -> None:
        print(f"[消息] {fr}: {msg}")
        if self._msg_win is not None and self._msg_win.winfo_exists():
            hist = self._hist_widgets[0]
            self._append_msg(hist, f"{fr}: {msg}")

    @staticmethod
    def _append_msg(hist: tk.Text, line: str) -> None:
        hist.configure(state=tk.NORMAL)
        hist.insert(tk.END, line + "\n")
        hist.configure(state=tk.DISABLED)
        hist.see(tk.END)

    def _update_flight_ui(self) -> None:
        model = self.model
        if model is None:
            return
        fsd = model.fsd
        self.fsd_state_var.set(f"FSD: {fsd.state if fsd else '—'}")
        # 插件在线状态（监听插件是否上线）
        if self._xconnect.connected:
            self.xplane_state_var.set("插件: 已上线")
        elif self.xplane_state_var.get() not in ("插件: —", "插件: 未上线"):
            self.xplane_state_var.set("插件: 离线")
        online = fsd is not None and fsd.state == STATE_ONLINE
        fd = self._last_fd
        if online and fd is not None:
            self.freq_var.set(f"COM1 {pk.millihertz_to_display(fd.com1_freq)}  "
                              f"COM2 {pk.millihertz_to_display(fd.com2_freq)}")
            self.xpdr_var.set(f"应答机 {fd.transponder:04d}")
            self.speed_var.set(f"地速 {fd.groundspeed:.0f} kn")
            self.alt_var.set(f"高度 {fd.altitude_msl:.0f} ft")


def run_gui() -> None:
    root = tk.Tk()
    XflyGui(root)
    root.mainloop()


if __name__ == "__main__":
    run_gui()