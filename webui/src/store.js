/* XFlySim-Client 全局状态与逻辑（Vue3 响应式 store）。
 * 数据来源：pywebview → Api（事件轮询 pull / 直接调用）。 */
import { reactive } from 'vue'
import { ElMessage } from 'element-plus'

export const store = reactive({
  api: null,
  bridgeReady: false,
  connected: false,
  busy: false,
  callsign: '', aircraft: '', airline: '',
  sim: 'xp12',
  st: { com1: '———', com2: '———', xpdr: '———', gs: '—', alt: '—' },
  fsd: { text: 'OFFLINE', tag: 'info' },
  xplane: { text: '未发现模拟机', tag: 'info' },
  csl: { text: 'CSL —', tag: 'info', title: 'CSL 模型加载情况' },
  roster: '',
  log: [],
  msgs: [],
  msgTo: '', msgText: '',
  fp: { type: 'I', aircraft: '', speed: '450', alt: 'FL350', dep: '',
        dest: '', altapt: '', plan_time: '0000', actual: '0000',
        enrh: 0, enrm: 0, fuelh: 0, fuelm: 0, remarks: '', route: '' },
  set: { real_name: '', cid: '', password: '', remember: true,
         callsign: '', airline: '',
         host: '', port: 6809, xphost: '', xpport: 51001, debug: false },
  overlays: { msg: false, set: false, fp: false },
  newMsg: false,
  _logId: 0,
  _dragging: false,
  _dragX: 0, _dragY: 0,
})

let toastTimer = null

export function message(data, isErr = false) {
  if (store.api) {
    ElMessage({ message: data, type: isErr ? 'error' : 'success', duration: 2600 })
    return
  }
  // 兜底：无 Element Plus 环境
  /* toast-fallback not exist??
  const el = document.getElementById('toast-fallback')
  if (el) {
    el.textContent = String(data)
    el.classList.add('show')
    clearTimeout(toastTimer)
    toastTimer = setTimeout(() => el.classList.remove('show'), 2600)
  }
  */
  if (isErr) {
    addLog(`[消息] ${data}`, 'txt')
  } else {
    addLog(`[!] ${data}`, 'err')
  }
}

/* ---------- 初始化 ---------- */
export async function initStore() {
  try {
    const s = await store.api.get_settings()
    if (!store.callsign) store.callsign = s.callsign
    if (!store.aircraft) store.aircraft = s.aircraft_icao
    if (!store.airline) store.airline = s.airline_icao
    store.api.set_debug(!!s.debug_mode)
  } catch (e) { /* 忽略 */ }
}

/* ---------- 事件轮询 ---------- */
export async function poll() {
  if (!store.api) return
  try {
    const events = await store.api.pull()
    events.forEach(handleEvent)
  } catch (e) { /* 忽略瞬时桥接错误 */ }
}

export async function statusTick() {
  if (!store.api) return
  try {
    const s = await store.api.status()
    switch (s.fsd) {
      case 'Online': store.fsd = { text: 'ONLINE', tag: 'success' }; break
      case 'Connecting':
      case 'WaitLogin': store.fsd = { text: s.fsd.toUpperCase(), tag: 'warning' }; break
      case 'Disconnected': store.fsd = { text: 'OFFLINE', tag: 'info' }; break
      default: store.fsd = { text: 'ERROR', tag: 'danger' }; break
    }
    store.roster = (s.atc > 0 || s.players > 0) ? `ATC ${s.atc} · PLR ${s.players}` : ''
    if (s.xconnect) store.xplane = { text: '已发现模拟机', tag: 'success' }
  } catch (e) { /* 忽略 */ }
}

function handleEvent(ev) {
  switch (ev.event) {
    case 'log': addLog(ev.data, 'out'); break
    case 'text':
      addLog(`[消息] ${ev.data[0]}: ${ev.data[1]}`, 'txt')
      appendMsg(ev.data[0], ev.data[1], false)
      break
    case 'flight': updateFlight(ev.data); break
    case 'xconnect': setXconnectState(ev.data); break
    case 'csl': setCslStatus(ev.data); break
    case 'xplog': addLog('◂ ' + JSON.stringify(ev.data), 'in'); break
    case 'sys': addLog(`[!] ${ev.data}`, 'err'); break
    case 'disconnected':
      store.connected = false
      addLog('已断开连接', 'in')
      break
  }
}

function setXconnectState(state) {
  switch (state) {
    case 'connected': store.xplane = { text: '已发现模拟机', tag: 'success' }; break
    case 'waiting': store.xplane = { text: '未发现模拟机', tag: 'info' }; break
    case 'failed':
      store.xplane = { text: '连接错误', tag: 'danger' }
      addLog('[xconnect] 无法连接模拟机，请检查 X-Plane 是否运行', 'err')
      break
    case 'disconnected': store.xplane = { text: '未发现模拟机', tag: 'info' }; break
  }
}

function setCslStatus(d) {
  const loaded = d.loaded || []
  const missing = d.missing || []
  const total = loaded.length + missing.length
  if (total === 0) {
    store.csl = { text: 'CSL —', tag: 'info', title: 'CSL 模型加载情况' }
    return
  }
  store.csl = {
    text: `CSL ${loaded.length}/${total}`,
    tag: missing.length ? 'danger' : 'success',
    title: '已加载: ' + (loaded.join(', ') || '无') +
           '\n缺失: ' + (missing.join(', ') || '无'),
  }
  if (missing.length)
    addLog(`CSL 缺失模型: ${missing.join(', ')}（放入 plugins/XConnect/CSL/）`, 'err')
}

function updateFlight(fd) {
  store.st.com1 = fmtFreq(fd.com1_freq)
  store.st.com2 = fmtFreq(fd.com2_freq)
  store.st.xpdr = String(fd.transponder).padStart(4, '0')
  store.st.gs = `${fd.groundspeed.toFixed(0)} kn`
  store.st.alt = `${fd.altitude_msl.toFixed(0)} ft`
  if (fd.aircraft_icao && fd.aircraft_icao !== store.aircraft) {
    store.aircraft = fd.aircraft_icao.toUpperCase()
    addLog(`识别机型: ${store.aircraft}`, 'in')
  }
}

export function fmtFreq(milli) {
  if (milli < 100000) milli += 100000
  return `${Math.floor(milli / 1000)}.${String(milli % 1000).padStart(3, '0')}`
}

export function addLog(msg, cls = '') {
  const t = new Date().toTimeString().slice(0, 8)
  store.log.push({ id: ++store._logId, t, m: msg, cls })
  store.newMsg = true
}

export function appendMsg(from, text, sent) {
  store.msgs.push({ from, text, sent })
  store.newMsg = true
}

export function clearNewMsg() {
  store.newMsg = false
}

/* ---------- 连线 / 断开 ---------- */
export async function doConnect() {
  if (store.connected || store.busy) return
  store.busy = true
  try {
    const r = await store.api.connect(store.callsign, store.aircraft, store.airline)
    if (!r.ok) { message(r.error || '连接失败', true); return }
    store.connected = true
    message(`已连线 ${store.callsign.trim().toUpperCase()}`)
  } catch (e) {
    message(`连接失败: ${e}`, true)
  } finally {
    store.busy = false
  }
}

export async function doDisconnect() {
  if (!store.connected || store.busy) return
  store.busy = true
  try {
    const r = await store.api.disconnect()
    if (!r.ok) message(r.error || '断开失败', true)
  } catch (e) {
    message(`断开失败: ${e}`, true)
  } finally {
    store.busy = false
  }
}

export function onSim() {
  if (store.api) store.api.set_sim(store.sim)
}

export async function sendMsg() {
  const to = store.msgTo.trim()
  const msg = store.msgText.trim()
  if (!to || !msg) return
  let r
  if (to.startsWith('@')) {
    const mhz = parseFloat(to.slice(1))
    if (isNaN(mhz)) { message('频率格式错误，应为 @121.800', true); return }
    r = await store.api.send_freq(mhz, msg)
    appendMsg(`@${to.slice(1)}`, msg, true)
  } else {
    r = await store.api.send_text(to.toUpperCase(), msg)
    appendMsg(to.toUpperCase(), msg, true)
  }
  if (!r.ok) message(r.error || '发送失败', true)
  store.msgText = ''
}

/* ---------- 设置 ---------- */
export async function openSettings() {
  const s = await store.api.get_settings()
  store.set = {
    real_name: s.real_name, cid: s.cid, password: s.password,
    remember: !!s.remember_password,
    callsign: s.callsign, airline: s.airline_icao,
    host: s.server_host, port: s.server_port,
    xphost: s.xconnect_host, xpport: s.xconnect_port,
    debug: !!s.debug_mode,
  }
  store.overlays.set = true
}

export async function saveSettings() {
  const d = {
    real_name: store.set.real_name.trim(),
    cid: store.set.cid.trim(),
    password: store.set.remember ? store.set.password : '',
    remember_password: store.set.remember,
    callsign: store.set.callsign.trim().toUpperCase(),
    airline_icao: store.set.airline.trim().toUpperCase(),
    server_host: store.set.host.trim(),
    server_port: parseInt(store.set.port, 10),
    xconnect_host: store.set.xphost.trim(),
    xconnect_port: parseInt(store.set.xpport, 10),
    debug_mode: store.set.debug,
  }
  store.api.set_debug(d.debug_mode)
  const r = await store.api.save_settings(d)
  if (r.ok) {
    store.callsign = d.callsign
    store.airline = d.airline_icao
    message(d.debug_mode ? '已保存（调试模式已开启）' : '已保存')
    store.overlays.set = false
  } else {
    message(r.error || '保存失败', true)
  }
}

/* ---------- 飞行计划 ---------- */
export function openFlightPlan() {
  if (!store.connected) {
    message('请先连线 FSD 再提交飞行计划', true)
    return
  }
  if (!store.fp.aircraft) store.fp.aircraft = store.aircraft
  store.overlays.fp = true
}

export async function submitFlightPlan() {
  const dep = store.fp.dep.trim().toUpperCase()
  const dest = store.fp.dest.trim().toUpperCase()
  if (!dep || !dest) { message('请填写起飞机场与目的地', true); return }
  const r = await store.api.submit_flight_plan({
    type: store.fp.type,
    aircraft: store.fp.aircraft.trim(),
    cruise_speed: store.fp.speed.trim() || '450',
    cruise_alt: store.fp.alt.trim() || 'FL350',
    dep, dest,
    alt: store.fp.altapt.trim().toUpperCase(),
    plan_time: store.fp.plan_time.trim() || '0000',
    actual_time: store.fp.actual.trim() || '0000',
    enroute_h: store.fp.enrh || 0,
    enroute_m: store.fp.enrm || 0,
    fuel_h: store.fp.fuelh || 0,
    fuel_m: store.fp.fuelm || 0,
    remarks: store.fp.remarks,
    route: store.fp.route.trim(),
  })
  if (r.ok) {
    message('飞行计划已提交')
    store.overlays.fp = false
  } else {
    message(r.error || '提交失败', true)
  }
}

/* ---------- 调试 / 工具 ---------- */
export function toggleDebug() {
  const next = !store.set.debug
  store.set.debug = next
  if (store.api) store.api.set_debug(next)
  message(next ? '调试模式已开启（显示插件原始报文）' : '调试模式已关闭')
}

export function scrollLog(dir) {
  const bar = document.querySelector('.log-pane .el-scrollbar__wrap')
  if (bar) bar.scrollTop += dir * 140
}

/* ---------- 窗口控制（frameless） ---------- */
export function dragStart(e) {
  if (e.button !== 0) return
  store._dragging = true
  store._dragX = e.screenX
  store._dragY = e.screenY
}

export function dragMove(e) {
  if (!store._dragging) return
  const dx = e.screenX - store._dragX
  const dy = e.screenY - store._dragY
  if (dx === 0 && dy === 0) return
  store._dragX = e.screenX
  store._dragY = e.screenY
  if (store.api) store.api.move_by(dx, dy)
}

export function dragEnd() { store._dragging = false }

export function winMin() { if (store.api) store.api.win_minimize() }
export function winMax() { if (store.api) store.api.win_maximize() }
export function winClose() { if (store.api) store.api.win_close() }

export function noop() {}

/* ---------- 启动（mounted 调用） ---------- */
export function start() {
  window.addEventListener('pywebviewready', () => {
    store.bridgeReady = true
    store.api = window.pywebview.api
    addLog('欢迎使用 XFlySim Client', 'in')
    initStore()
  })
  setTimeout(() => {
    if (!store.bridgeReady) {
      window.__previewShown = true
      addLog('预览模式：未连接后端（请通过 python webapp.py 启动）', 'err')
    }
  }, 3000)

  window.addEventListener('mousemove', dragMove)
  window.addEventListener('mouseup', dragEnd)
  setInterval(poll, 100)
  setInterval(statusTick, 1000)
}