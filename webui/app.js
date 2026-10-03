/* XFlySim-Client 前端逻辑：pywebview 桥接 + 事件轮询（xPilot 风格简约 UI） */
"use strict";

let api = null;
let connected = false;
const $ = (id) => document.getElementById(id);

const el = {
  callsign: $("in-callsign"), aircraft: $("in-aircraft"), airline: $("in-airline"),
  sim: $("sel-sim"), connect: $("btn-connect"),
  fields: $("connect-fields"), status: $("status-fields"),
  fsd: $("st-fsd"), xplane: $("st-xplane"), roster: $("st-roster"),
  com1: $("st-com1"), com2: $("st-com2"),
  xpdr: $("st-xpdr"), gs: $("st-gs"), alt: $("st-alt"),
  log: $("log"), toast: $("toast"),
  msgOverlay: $("msg-overlay"), msgList: $("msg-list"),
  msgTo: $("msg-to"), msgText: $("msg-text"), msgSend: $("msg-send"),
  setOverlay: $("set-overlay"),
};

/* ---------- 事件处理 ---------- */

function handleEvent(ev) {
  switch (ev.event) {
    case "log": addLog(ev.data, "out"); break;
    case "text":
      addLog(`[消息] ${ev.data[0]}: ${ev.data[1]}`, "txt");
      appendMsg(ev.data[0], ev.data[1], false);
      break;
    case "flight": updateFlight(ev.data); break;
    case "xconnect": setXconnectState(ev.data); break;
    case "xplog": addLog("◂ " + JSON.stringify(ev.data), "in"); break;
    case "sys": addLog(`[!] ${ev.data}`, "err"); break;
    case "disconnected":
      setConnected(false);
      addLog("已断开连接", "in");
      break;
  }
}

function setXconnectState(state) {
  switch (state) {
    case "connected":
      el.xplane.textContent = "XP Online";
      el.xplane.className = "pill ok";
      break;
    case "waiting":
      el.xplane.textContent = "XP Offline";
      el.xplane.className = "pill wait";
      break;
    case "failed":
      el.xplane.textContent = "XP Offline";
      el.xplane.className = "pill dim";
      addLog("[xconnect] 无法连接插件，请检查 X-Plane 是否运行", "err");
      break;
    case "disconnected":
      el.xplane.textContent = "XP Offline";
      el.xplane.className = "pill dim";
      break;
  }
}

async function poll() {
  try {
    const events = await api.pull();
    events.forEach(handleEvent);
  } catch (e) { /* 忽略瞬时桥接错误 */ }
}

/* ---------- 日志 ---------- */

function addLog(msg, cls = "") {
  const t = new Date().toTimeString().slice(0, 8);
  const line = document.createElement("div");
  const ts = document.createElement("span");
  ts.className = "t"; ts.textContent = t;
  line.appendChild(ts);
  line.appendChild(document.createTextNode(msg));
  if (cls) line.classList.add(cls);
  el.log.appendChild(line);
  el.log.scrollTop = el.log.scrollHeight;
}

/* ---------- 状态 ---------- */

function updateFlight(fd) {
  el.com1.textContent = fmtFreq(fd.com1_freq);
  el.com2.textContent = fmtFreq(fd.com2_freq);
  el.xpdr.textContent = String(fd.transponder).padStart(4, "0");
  el.gs.textContent = `${fd.groundspeed.toFixed(0)} kn`;
  el.alt.textContent = `${fd.altitude_msl.toFixed(0)} ft`;
}

function fmtFreq(milli) {
  if (milli < 100000) milli += 100000;
  return `${Math.floor(milli / 1000)}.${String(milli % 1000).padStart(3, "0")}`;
}

async function statusTick() {
  if (!api) return;
  try {
    const st = await api.status();
    switch (st.fsd) {
      case "Online":
        el.fsd.textContent = "ONLINE";
        el.fsd.className = "pill online";
        break;
      case "Connecting":
      case "WaitLogin":
        el.fsd.textContent = st.fsd.toUpperCase();
        el.fsd.className = "pill wait";
        break;
      default:
        el.fsd.textContent = "OFFLINE";
        el.fsd.className = "pill offline";
    }
    el.roster.textContent = st.atc > 0 || st.players > 0
      ? `ATC ${st.atc} · PLR ${st.players}` : "";
    if (st.xconnect) {
      el.xplane.textContent = "XP Online";
      el.xplane.className = "pill ok";
    } else if (el.xplane.textContent !== "XP Offline") {
      el.xplane.textContent = "XP Offline";
      el.xplane.className = "pill dim";
    }
  } catch (e) { /* 忽略 */ }
}

/* ---------- 连线 / 断开 ---------- */

function setConnected(on) {
  connected = on;
  el.fields.classList.toggle("hidden", on);
  el.status.classList.toggle("hidden", !on);
  el.connect.textContent = on ? "Disconnect" : "Connect";
  el.connect.disabled = false;
}

async function doConnect() {
  if (connected) return;
  el.connect.textContent = "Connecting...";
  el.connect.disabled = true;
  try {
    const r = await api.connect(el.callsign.value, el.aircraft.value, el.airline.value);
    if (!r.ok) {
      showToast(r.error || "连接失败", true);
      setConnected(false);
      return;
    }
    setConnected(true);
    showToast(`已连线 ${el.callsign.value.trim().toUpperCase()}`);
  } catch (e) {
    showToast(`连接失败: ${e}`, true);
    setConnected(false);
  }
}

async function doDisconnect() {
  if (!connected) return;
  el.connect.textContent = "Disconnecting...";
  el.connect.disabled = true;
  try {
    const r = await api.disconnect();
    if (!r.ok) {
      showToast(r.error || "断开失败", true);
      setConnected(true);
    }
  } catch (e) {
    showToast(`断开失败: ${e}`, true);
    setConnected(true);
  }
}

/* ---------- 消息 ---------- */

function appendMsg(from, text, sent) {
  const div = document.createElement("div");
  div.className = `m${sent ? " sent" : ""}`;
  const b = document.createElement("b");
  b.textContent = from;
  div.appendChild(b);
  div.appendChild(document.createTextNode(`  ${text}`));
  el.msgList.appendChild(div);
  el.msgList.scrollTop = el.msgList.scrollHeight;
}

async function sendMsg() {
  const to = el.msgTo.value.trim();
  const msg = el.msgText.value.trim();
  if (!to || !msg) return;
  let r;
  if (to.startsWith("@")) {
    const mhz = parseFloat(to.slice(1));
    if (isNaN(mhz)) { showToast("频率格式错误，应为 @121.800", true); return; }
    r = await api.send_freq(mhz, msg);
    appendMsg(`@${to.slice(1)}`, msg, true);
  } else {
    r = await api.send_text(to, msg);
    appendMsg(to.toUpperCase(), msg, true);
  }
  if (!r.ok) showToast(r.error || "发送失败", true);
  el.msgText.value = "";
}

/* ---------- 设置 ---------- */

async function openSettings() {
  const s = await api.get_settings();
  $("set-realname").value = s.real_name;
  $("set-cid").value = s.cid;
  $("set-password").value = s.password;
  $("set-remember").checked = !!s.remember_password;
  $("set-callsign").value = s.callsign;
  $("set-aircraft").value = s.aircraft_icao;
  $("set-airline").value = s.airline_icao;
  $("set-host").value = s.server_host;
  $("set-port").value = s.server_port;
  $("set-xphost").value = s.xconnect_host;
  $("set-xpport").value = s.xconnect_port;
  $("set-debug").checked = !!s.debug_mode;
  el.setOverlay.classList.remove("hidden");
}

async function saveSettings() {
  const d = {
    real_name: $("set-realname").value.trim(),
    cid: $("set-cid").value.trim(),
    password: $("set-remember").checked ? $("set-password").value : "",
    remember_password: $("set-remember").checked,
    callsign: $("set-callsign").value.trim().toUpperCase(),
    aircraft_icao: $("set-aircraft").value.trim().toUpperCase(),
    airline_icao: $("set-airline").value.trim().toUpperCase(),
    server_host: $("set-host").value.trim(),
    server_port: parseInt($("set-port").value, 10),
    xconnect_host: $("set-xphost").value.trim(),
    xconnect_port: parseInt($("set-xpport").value, 10),
    debug_mode: $("set-debug").checked,
  };
  api.set_debug(d.debug_mode);
  const r = await api.save_settings(d);
  if (r.ok) {
    el.callsign.value = d.callsign;
    el.aircraft.value = d.aircraft_icao;
    el.airline.value = d.airline_icao;
    showToast(d.debug_mode ? "已保存（调试模式已开启）" : "已保存 (config.json)");
    el.setOverlay.classList.add("hidden");
  } else {
    showToast(r.error || "保存失败", true);
  }
}

/* ---------- Toast ---------- */

let toastTimer = null;
function showToast(msg, isErr = false) {
  el.toast.textContent = msg;
  el.toast.className = "toast" + (isErr ? " err" : "");
  el.toast.classList.remove("hidden");
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => el.toast.classList.add("hidden"), 2600);
}

/* ---------- 初始化 ---------- */

function init() {
  api.get_settings().then((s) => {
    if (!el.callsign.value) el.callsign.value = s.callsign;
    if (!el.aircraft.value) el.aircraft.value = s.aircraft_icao;
    if (!el.airline.value) el.airline.value = s.airline_icao;
  });

  el.connect.addEventListener("click", () => {
    if (connected) doDisconnect();
    else doConnect();
  });
  $("btn-quit").addEventListener("click", () => window.close());
  el.msgBtn = $("btn-msg");
  el.msgBtn.addEventListener("click", () => {
    el.msgOverlay.classList.remove("hidden");
    el.msgText.focus();
  });
  $("msg-close").addEventListener("click", () => el.msgOverlay.classList.add("hidden"));
  el.msgOverlay.addEventListener("click", (e) => {
    if (e.target === el.msgOverlay) el.msgOverlay.classList.add("hidden");
  });
  el.msgSend.addEventListener("click", sendMsg);
  el.msgText.addEventListener("keydown", (e) => { if (e.key === "Enter") sendMsg(); });

  $("btn-settings").addEventListener("click", openSettings);
  $("set-close").addEventListener("click", () => el.setOverlay.classList.add("hidden"));
  $("set-save").addEventListener("click", saveSettings);

  el.sim.addEventListener("change", () => api.set_sim(el.sim.value));

  setInterval(poll, 100);
  setInterval(statusTick, 1000);
}

/* ---------- 桥接就绪（pywebview 页面加载后注入） ---------- */

let bridgeReady = false;
window.__previewShown = false;

window.addEventListener("pywebviewready", () => {
  bridgeReady = true;
  api = window.pywebview.api;
  addLog("pywebview 已就绪，后端 API 可用", "in");
  init();
});

// 兜底：3 秒内 pywebviewready 未触发 → 视为纯浏览器预览（无后端）
setTimeout(() => {
  if (!bridgeReady) {
    window.__previewShown = true;
    addLog("预览模式：未连接后端（请通过 python webapp.py 启动）", "err");
  }
}, 3000);
