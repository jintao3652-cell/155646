/* XFlySim-Client 前端逻辑：Vue3 + Element Plus + Font Awesome
 * 数据来源：pywebview → Api（事件轮询 pull / 直接调用） */
"use strict";

const { createApp } = Vue;

createApp({
  data() {
    return {
      api: null,
      bridgeReady: false,
      connected: false,
      busy: false,
      callsign: "", aircraft: "", airline: "",
      sim: "xp12",
      st: { com1: "———", com2: "———", xpdr: "———", gs: "—", alt: "—" },
      fsd: { text: "OFFLINE", tag: "danger" },
      xplane: { text: "未发现模拟机", tag: "info" },
      csl: { text: "CSL —", tag: "info", title: "CSL 模型加载情况" },
      roster: "",
      log: [],
      msgs: [],
      msgTo: "", msgText: "",
      fp: { type: "I", aircraft: "", speed: "450", alt: "FL350", dep: "",
            dest: "", altapt: "", plan_time: "0000", actual: "0000",
            enrh: 0, enrm: 0, fuelh: 0, fuelm: 0, remarks: "", route: "" },
      set: { real_name: "", cid: "", password: "", remember: true,
             callsign: "", aircraft: "", airline: "",
             host: "", port: 6809, xphost: "", xpport: 51001, debug: false },
      overlays: { msg: false, set: false, fp: false },
      toastTimer: null,
    };
  },

  mounted() {
    window.addEventListener("pywebviewready", () => {
      this.bridgeReady = true;
      this.api = window.pywebview.api;
      this.addLog("欢迎使用 XFlySim Client", "in");
      this.init();
    });
    // 兜底：3s 未触发视为纯浏览器预览
    setTimeout(() => {
      if (!this.bridgeReady) {
        window.__previewShown = true;
        this.addLog("预览模式：未连接后端（请通过 python webapp.py 启动）", "err");
      }
    }, 3000);

    setInterval(() => this.poll(), 100);
    setInterval(() => this.statusTick(), 1000);
  },

  methods: {
    /* ---------- 初始化 ---------- */
    async init() {
      try {
        const s = await this.api.get_settings();
        if (!this.callsign) this.callsign = s.callsign;
        if (!this.aircraft) this.aircraft = s.aircraft_icao;
        if (!this.airline) this.airline = s.airline_icao;
        this.api.set_debug(!!s.debug_mode);
      } catch (e) { /* 忽略 */ }
    },

    /* ---------- 事件轮询 ---------- */
    async poll() {
      if (!this.api) return;
      try {
        const events = await this.api.pull();
        events.forEach((ev) => this.handleEvent(ev));
      } catch (e) { /* 忽略瞬时桥接错误 */ }
    },

    handleEvent(ev) {
      switch (ev.event) {
        case "log": this.addLog(ev.data, "out"); break;
        case "text":
          this.addLog(`[消息] ${ev.data[0]}: ${ev.data[1]}`, "txt");
          this.appendMsg(ev.data[0], ev.data[1], false);
          break;
        case "flight": this.updateFlight(ev.data); break;
        case "xconnect": this.setXconnectState(ev.data); break;
        case "csl": this.setCslStatus(ev.data); break;
        case "xplog": this.addLog("◂ " + JSON.stringify(ev.data), "in"); break;
        case "sys": this.addLog(`[!] ${ev.data}`, "err"); break;
        case "disconnected":
          this.connected = false;
          this.addLog("已断开连接", "in");
          break;
      }
    },

    setXconnectState(state) {
      switch (state) {
        case "connected":
          this.xplane = { text: "已发现模拟机", tag: "success" };
          break;
        case "waiting":
          this.xplane = { text: "未发现模拟机", tag: "warning" };
          break;
        case "failed":
          this.xplane = { text: "未发现模拟机", tag: "info" };
          this.addLog("[xconnect] 无法连接模拟机，请检查 X-Plane 是否运行", "err");
          break;
        case "disconnected":
          this.xplane = { text: "未发现模拟机", tag: "info" };
          break;
      }
    },

    setCslStatus(d) {
      const loaded = d.loaded || [];
      const missing = d.missing || [];
      const total = loaded.length + missing.length;
      if (total === 0) {
        this.csl = { text: "CSL —", tag: "info", title: "CSL 模型加载情况" };
        return;
      }
      this.csl = {
        text: `CSL ${loaded.length}/${total}`,
        tag: missing.length ? "warning" : "success",
        title: "已加载: " + (loaded.join(", ") || "无") +
               "\n缺失: " + (missing.join(", ") || "无"),
      };
      if (missing.length)
        this.addLog(`CSL 缺失模型: ${missing.join(", ")}（放入 plugins/XConnect/CSL/）`, "err");
    },

    updateFlight(fd) {
      this.st.com1 = this.fmtFreq(fd.com1_freq);
      this.st.com2 = this.fmtFreq(fd.com2_freq);
      this.st.xpdr = String(fd.transponder).padStart(4, "0");
      this.st.gs = `${fd.groundspeed.toFixed(0)} kn`;
      this.st.alt = `${fd.altitude_msl.toFixed(0)} ft`;
      // 模拟器识别机型：自动填充（只读显示）
      if (fd.aircraft_icao && fd.aircraft_icao !== this.aircraft) {
        this.aircraft = fd.aircraft_icao.toUpperCase();
        this.addLog(`识别机型: ${this.aircraft}`, "in");
      }
    },

    fmtFreq(milli) {
      if (milli < 100000) milli += 100000;
      return `${Math.floor(milli / 1000)}.${String(milli % 1000).padStart(3, "0")}`;
    },

    async statusTick() {
      if (!this.api) return;
      try {
        const s = await this.api.status();
        switch (s.fsd) {
          case "Online": this.fsd = { text: "ONLINE", tag: "success" }; break;
          case "Connecting":
          case "WaitLogin":
            this.fsd = { text: s.fsd.toUpperCase(), tag: "warning" }; break;
          default: this.fsd = { text: "OFFLINE", tag: "danger" }; break;
        }
        this.roster = (s.atc > 0 || s.players > 0)
          ? `ATC ${s.atc} · PLR ${s.players}` : "";
        if (s.xconnect)
          this.xplane = { text: "已发现模拟机", tag: "success" };
      } catch (e) { /* 忽略 */ }
    },

    /* ---------- 连线 / 断开 ---------- */
    onSim() { if (this.api) this.api.set_sim(this.sim); },

    async doConnect() {
      if (this.connected || this.busy) return;
      this.busy = true;
      try {
        const r = await this.api.connect(this.callsign, this.aircraft, this.airline);
        if (!r.ok) {
          this.$message({ type: "error", message: r.error || "连接失败" });
          return;
        }
        this.connected = true;
        this.$message(`已连线 ${this.callsign.trim().toUpperCase()}`);
      } catch (e) {
        this.$message({ type: "error", message: `连接失败: ${e}` });
      } finally {
        this.busy = false;
      }
    },

    async doDisconnect() {
      if (!this.connected || this.busy) return;
      this.busy = true;
      try {
        const r = await this.api.disconnect();
        if (!r.ok) this.$message({ type: "error", message: r.error || "断开失败" });
        // 按钮复位由 disconnected 事件驱动
      } catch (e) {
        this.$message({ type: "error", message: `断开失败: ${e}` });
      } finally {
        this.busy = false;
      }
    },

    /* ---------- 日志 / 消息 ---------- */
    addLog(msg, cls = "") {
      const t = new Date().toTimeString().slice(0, 8);
      this.log.push({ t, m: msg, cls });
      this.$nextTick(() => {
        const zone = document.querySelector(".main-zone");
        if (zone) zone.scrollTop = zone.scrollHeight;
      });
    },

    appendMsg(from, text, sent) {
      this.msgs.push({ from, text, sent });
    },

    async sendMsg() {
      const to = this.msgTo.trim();
      const msg = this.msgText.trim();
      if (!to || !msg) return;
      let r;
      if (to.startsWith("@")) {
        const mhz = parseFloat(to.slice(1));
        if (isNaN(mhz)) { this.$message({ type: "error", message: "频率格式错误，应为 @121.800" }); return; }
        r = await this.api.send_freq(mhz, msg);
        this.appendMsg(`@${to.slice(1)}`, msg, true);
      } else {
        r = await this.api.send_text(to.toUpperCase(), msg);
        this.appendMsg(to.toUpperCase(), msg, true);
      }
      if (!r.ok) this.$message({ type: "error", message: r.error || "发送失败" });
      this.msgText = "";
    },

    /* ---------- 设置 ---------- */
    async openSettings() {
      const s = await this.api.get_settings();
      this.set = {
        real_name: s.real_name, cid: s.cid, password: s.password,
        remember: !!s.remember_password,
        callsign: s.callsign, airline: s.airline_icao,
        host: s.server_host, port: s.server_port,
        xphost: s.xconnect_host, xpport: s.xconnect_port,
        debug: !!s.debug_mode,
      };
      this.overlays.set = true;
    },

    async saveSettings() {
      const d = {
        real_name: this.set.real_name.trim(),
        cid: this.set.cid.trim(),
        password: this.set.remember ? this.set.password : "",
        remember_password: this.set.remember,
        callsign: this.set.callsign.trim().toUpperCase(),
        airline_icao: this.set.airline.trim().toUpperCase(),
        server_host: this.set.host.trim(),
        server_port: parseInt(this.set.port, 10),
        xconnect_host: this.set.xphost.trim(),
        xconnect_port: parseInt(this.set.xpport, 10),
        debug_mode: this.set.debug,
      };
      this.api.set_debug(d.debug_mode);
      const r = await this.api.save_settings(d);
      if (r.ok) {
        this.callsign = d.callsign;
        this.airline = d.airline_icao;
        this.$message(d.debug_mode ? "已保存（调试模式已开启）" : "已保存");
        this.overlays.set = false;
      } else {
        this.$message({ type: "error", message: r.error || "保存失败" });
      }
    },

    /* ---------- 飞行计划 ---------- */
    openFlightPlan() {
      if (!this.connected) {
        this.$message({ type: "warning", message: "请先连线 FSD 再提交飞行计划" });
        return;
      }
      if (!this.fp.aircraft) this.fp.aircraft = this.aircraft;
      this.overlays.fp = true;
    },

    async submitFlightPlan() {
      const dep = this.fp.dep.trim().toUpperCase();
      const dest = this.fp.dest.trim().toUpperCase();
      if (!dep || !dest) {
        this.$message({ type: "error", message: "请填写起飞机场与目的地" });
        return;
      }
      const r = await this.api.submit_flight_plan({
        type: this.fp.type,
        aircraft: this.fp.aircraft.trim(),
        cruise_speed: this.fp.speed.trim() || "450",
        cruise_alt: this.fp.alt.trim() || "FL350",
        dep, dest,
        alt: this.fp.altapt.trim().toUpperCase(),
        plan_time: this.fp.plan_time.trim() || "0000",
        actual_time: this.fp.actual.trim() || "0000",
        enroute_h: this.fp.enrh || 0,
        enroute_m: this.fp.enrm || 0,
        fuel_h: this.fp.fuelh || 0,
        fuel_m: this.fp.fuelm || 0,
        remarks: this.fp.remarks,
        route: this.fp.route.trim(),
      });
      if (r.ok) {
        this.$message("飞行计划已提交");
        this.overlays.fp = false;
      } else {
        this.$message({ type: "error", message: r.error || "提交失败" });
      }
    },

    quit() {
      window.close();
    },
  },
}).use(ElementPlus).mount("#app");