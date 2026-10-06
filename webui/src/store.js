/* XFlySim-Client 全局状态与逻辑（Vue3 响应式 store）。
 * 数据来源：pywebview → Api（事件轮询 pull / 直接调用）。
 * 界面文本统一走 vue-i18n（en / zh-CN / zh-TW，语言保存在 config.json）。
 * 状态标签用 computed 从原始状态派生，切换语言即时刷新。 */
import { reactive, computed } from 'vue'
import i18n, { normalizeLocale, setLocale, getLocale } from './i18n'

const t = (k, p) => i18n.global.t(k, p)

export const store = reactive({
  api: null,
  bridgeReady: false,
  connected: false,
  busy: false,
  callsign: '', aircraft: '', airline: '',
  sim: 'xp12',
  lang: getLocale(),
  st: { com1: '———', com2: '———', com3: '———',
       com1s: '———', com2s: '———', com3s: '———',
       xpdr: '———', xpdrMode: '', xpdrPwr: '', xpdrSys: '',
       tcasMode: '', tcasAlt: '',
       gs: '—', alt: '—' },
  fsdState: 'Disconnected',   // Online / Connecting / WaitLogin / Disconnected / ...
  xpState: 'waiting',          // waiting / connected / failed / disconnected
  cslLoaded: [],
  cslMissing: [],
  roster: '',
  atcCount: 0, plrCount: 0,
  log: [],
  msgs: [],
  msgTo: '', msgText: '',
  fp: { type: 'I', aircraft: '', speed: '450', alt: 'FL350', dep: '',
        dest: '', altapt: '', plan_time: '0000', actual: '0000',
        enrh: 0, enrm: 0, fuelh: 0, fuelm: 0, remarks: '', route: '' },
  set: { real_name: '', cid: '', password: '', remember: true,
         callsign: '', airline: '',
         host: '', port: 6809, xphost: '', xpport: 51001, debug: false,
         language: getLocale() },
  overlays: { msg: false, set: false, fp: false },
  newMsg: false,
  screenMsg: null,
  _logId: 0,
  _dragging: false,
  _dragX: 0, _dragY: 0,
})

/* ---------- 状态标签（随语言即时刷新） ---------- */

export const fsdText = computed(() => {
  switch (store.fsdState) {
    case 'Online': return 'ONLINE'
    case 'Connecting':
    case 'WaitLogin': return store.fsdState.toUpperCase()
    case 'Disconnected': return 'OFFLINE'
    default: return 'ERROR'
  }
})
export const fsdTag = computed(() => {
  switch (store.fsdState) {
    case 'Online': return 'success'
    case 'Connecting':
    case 'WaitLogin': return 'warning'
    case 'Disconnected': return 'info'
    default: return 'danger'
  }
})

export const xpText = computed(() => {
  switch (store.xpState) {
    case 'connected': return t('status.simFound')
    case 'failed': return t('status.connError')
    default: return t('status.simNotFound')
  }
})
export const xpTag = computed(() => {
  switch (store.xpState) {
    case 'connected': return 'success'
    case 'failed': return 'danger'
    default: return 'info'
  }
})

const cslTotal = computed(() => store.cslLoaded.length + store.cslMissing.length)
export const cslText = computed(() => {
  if (cslTotal.value === 0) return 'CSL —'
  return `CSL ${store.cslLoaded.length}/${cslTotal.value}`
})
export const cslTag = computed(() => {
  if (cslTotal.value === 0) return 'info'
  return store.cslMissing.length ? 'danger' : 'success'
})
export const cslTitle = computed(() => {
  if (cslTotal.value === 0) return t('status.cslTitle')
  return `${t('status.cslTitle')}: ${store.cslLoaded.length}/${cslTotal.value}`
})

/** 切换界面语言（本地即刻生效，持久化由 setLanguageAndSave / saveSettings 完成）。 */
export function applyLanguage(code) {
  store.lang = setLocale(code)
  document.documentElement.lang = store.lang
  return store.lang
}

/** 把后端返回的错误（code/error 字段）翻译成当前语言的文案。 */
export function errText(r) {
  if (!r) return ''
  if (r.code) {
    if (r.code === 'missing_fields' && Array.isArray(r.fields)) {
      const list = r.fields.map((f) => t(`errors.field.${f}`) || f).join('\n')
      return t('errors.missingFields', { list })
    }
    const key = `errors.${r.code}`
    const mapped = t(key)
    if (mapped && mapped !== key) return mapped
  }
  return r.error || ''
}

export function message(data, isErr = false) {
  store.screenMsg = { text: data, isErr: !!isErr, id: Date.now() }
  addLog(`[${isErr ? '!' : 'i'}] ${data}`, isErr ? 'err' : 'txt')
}

/* ---------- 初始化 ---------- */
export async function initStore() {
  try {
    const s = await store.api.get_settings()
    if (!store.callsign) store.callsign = s.callsign
    if (!store.aircraft) store.aircraft = s.aircraft_icao
    if (!store.airline) store.airline = s.airline_icao
    store.api.set_debug(!!s.debug_mode)
    const loc = setLocale(normalizeLocale(s.language))
    store.lang = loc
    store.set.language = loc
    document.documentElement.lang = loc
  } catch (e) { /* 忽略 */ }
}

/* ---------- 事件轮询 ---------- */
async function pollEvent() {
  if (!store.api) return
  try {
    const events = await store.api.pull()
    events.forEach(handleEvent)
  } catch (e) { /* 忽略瞬时桥接错误 */ }
}

export async function updateStatus(s) {
  store.fsdState = s.fsd || 'Disconnected'
  store.atcCount = s.atc || 0
  store.plrCount = s.players || 0
  store.roster = (s.atc > 0 || s.players > 0) ? `ATC ${s.atc} · PLR ${s.players}` : ''
  if (s.xconnect) store.xpState = 'connected'
}

function handleEvent(ev) {
  switch (ev.event) {
    case 'log': addLog(ev.data, 'out'); break
    case 'text':
      addLog(`${t('log.msgPrefix')} ${ev.data[0]}: ${ev.data[1]}`, 'txt')
      appendMsg(ev.data[0], ev.data[1], false)
      break
    case 'flight': updateFlight(ev.data); break
    case 'xconnect': setXconnectState(ev.data); break
    case 'csl': setCslStatus(ev.data); break
    case 'xplog': addLog('◂ ' + JSON.stringify(ev.data), 'in'); break
    case 'sys': addLog(`[!] ${ev.data}`, 'err'); break
    case 'fsdstate': updateStatus(ev.data); break
    case 'disconnected':
      store.connected = false
      store.atcCount = 0
      store.plrCount = 0
      addLog(t('log.disconnected'), 'in')
      break
  }
}

function setXconnectState(state) {
  if (state === 'failed') {
    addLog(t('log.simNotRunning'), 'err')
  }
  store.xpState = state
}

function setCslStatus(d) {
  store.cslLoaded = d.loaded || []
  store.cslMissing = d.missing || []
  if (store.cslMissing.length)
    addLog(t('status.cslMissing', { list: store.cslMissing.join(', ') }), 'err')
}

/* X-Plane transponder_mode 枚举（XP12: off=0 stdby=1 on=2 alt=3 test=4
 * gnd(ModeS)=5 ta_only=6 ta/ra=7） */
const XPDR_MODES = ['OFF', 'STBY', 'ON', 'ALT', 'TEST', 'GND', 'TA', 'TARA']
/* TCAS 高度过滤 tcas_filter：0=ALL 1=THRT 2=ABV 3=BLW 4=URSTR */
const TCAS_ALT_MODES = ['ALL', 'THRT', 'ABV', 'BLW', 'URSTR']
/* 机型自定义（ToLiss AirbusFBW）：数值语义待旋钮校准，未校准时显示原始值 */
const AC_XPDR_PWR = ['OFF', 'STBY', 'AUTO', 'ON']
const AC_TCAS_MODE = ['STBY', 'TA', 'TARA']
const AC_TCAS_ALT = ['ALL', 'ABV', 'BLW', 'THRT']

function label(map, raw) {
  if (raw < 0) return '—'
  return map[raw] !== undefined ? map[raw] : `#${raw}`
}

function updateFlight(fd) {
  store.st.com1 = fmtFreq(fd.com1_freq)
  store.st.com2 = fmtFreq(fd.com2_freq)
  store.st.com3 = fmtFreq(fd.com3_freq)
  store.st.com1s = fmtFreq(fd.com1_standby)
  store.st.com2s = fmtFreq(fd.com2_standby)
  store.st.com3s = fmtFreq(fd.com3_standby)
  store.st.xpdr = String(fd.transponder).padStart(4, '0')
  store.st.xpdrMode = XPDR_MODES[fd.xpdr_mode] || (fd.squark === 'S' ? 'ON' : 'OFF')
  /* 应答机电源（优先机型自定义 STBY/AUTO/ON，否则通用枚举） */
  store.st.xpdrPwr = fd.xpdr_pwr >= 0 ? label(AC_XPDR_PWR, fd.xpdr_pwr) : store.st.xpdrMode
  /* 应答机 1/2 号选择（仅机型自定义可得） */
  store.st.xpdrSys = fd.xpdr_sys >= 0 ? String(fd.xpdr_sys + 1) : store.st.xpdrMode
  /* TCAS 工作状态：机型自定义 STBY/TA/TARA；否则由通用 ModeS 枚举推导 */
  store.st.tcasMode = fd.xpdr_tcas_mode >= 0 ? label(AC_TCAS_MODE, fd.xpdr_tcas_mode)
    : (fd.xpdr_mode === 1) ? 'STBY'
      : (fd.xpdr_mode === 6) ? 'TA'
        : (fd.xpdr_mode === 7) ? 'TARA' : '—'
  /* TCAS 高度范围：机型自定义 ALL/ABV/BLW/THRT；否则通用 tcas_filter */
  store.st.tcasAlt = fd.xpdr_tcas_alt >= 0 ? label(AC_TCAS_ALT, fd.xpdr_tcas_alt)
    : (TCAS_ALT_MODES[fd.tcas_filter] || '—')
  store.st.gs = `${fd.groundspeed.toFixed(0)} kn`
  store.st.alt = `${fd.altitude_msl.toFixed(0)} ft`
  if (fd.aircraft_icao && fd.aircraft_icao !== store.aircraft) {
    store.aircraft = fd.aircraft_icao.toUpperCase()
    addLog(t('log.aircraftDetected', { icao: store.aircraft }), 'in')
  }
}

export function fmtFreq(milli) {
  if (milli <= 0) return '———'
  return `${Math.floor(milli / 1000)}.${String(milli % 1000).padStart(3, '0')}`
}

export function addLog(msg, cls = '') {
  const time = new Date().toTimeString().slice(0, 8)
  store.log.push({ id: ++store._logId, t: time, m: msg, cls })
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
    if (!r.ok) { message(errText(r) || t('actions.connectFailed'), true); return }
    store.connected = true
    message(t('actions.connectSuccess', { callsign: store.callsign.trim().toUpperCase() }))
  } catch (e) {
    message(`${t('actions.connectFailed')}: ${e}`, true)
  } finally {
    store.busy = false
  }
}

export async function doDisconnect() {
  if (!store.connected || store.busy) return
  store.busy = true
  try {
    const r = await store.api.disconnect()
    if (!r.ok) message(errText(r) || t('actions.disconnectFailed'), true)
  } catch (e) {
    message(`${t('actions.disconnectFailed')}: ${e}`, true)
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
    if (isNaN(mhz)) { message(t('msg.freqFormatError'), true); return }
    r = await store.api.send_freq(mhz, msg)
    appendMsg(`@${to.slice(1)}`, msg, true)
  } else {
    r = await store.api.send_text(to.toUpperCase(), msg)
    appendMsg(to.toUpperCase(), msg, true)
  }
  if (!r.ok) message(errText(r) || t('actions.sendFailed'), true)
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
    language: normalizeLocale(s.language),
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
    language: normalizeLocale(store.set.language),
  }
  store.api.set_debug(d.debug_mode)
  const r = await store.api.save_settings(d)
  if (r.ok) {
    store.callsign = d.callsign
    store.airline = d.airline_icao
    applyLanguage(d.language)
    message(store.set.debug ? t('actions.savedDebug') : t('actions.saved'))
    store.overlays.set = false
  } else {
    message(errText(r) || t('actions.saveFailed'), true)
  }
}

/* ---------- 账号页单独保存（不覆盖服务器等其它设置） ---------- */
async function saveAccountPatch(extra = {}) {
  const s = await store.api.get_settings()
  const d = {
    real_name: store.set.real_name.trim(),
    remember_password: !!store.set.remember,
    callsign: store.set.callsign.trim().toUpperCase(),
    password: store.set.password,
    airline_icao: s.airline_icao,
    server_host: s.server_host,
    server_port: s.server_port,
    xconnect_host: s.xconnect_host,
    xconnect_port: s.xconnect_port,
    debug_mode: !!s.debug_mode,
    language: normalizeLocale(s.language),
    ...extra,
  }
  const r = await store.api.save_settings(d)
  if (r.ok) {
    store.callsign = d.callsign
    message(t('actions.accountSaved'))
    return true
  }
  message(errText(r) || t('actions.saveFailed'), true)
  return false
}

export async function saveAccountSettings() {
  try { await saveAccountPatch() } catch (e) { /* 忽略 */ }
}

/* ---------- 语言（LCD 设置页调用：切换并持久化） ---------- */
export async function setLanguageAndSave(code) {
  const loc = normalizeLocale(code)
  applyLanguage(loc)
  store.set.language = loc
  try {
    await saveAccountPatch({ language: loc })
  } catch (e) { /* 忽略 */ }
}

/* ---------- 飞行计划 ---------- */
export function openFlightPlan() {
  if (!store.connected) {
    message(t('flightPlan.connectFirst'), true)
    return
  }
  if (!store.fp.aircraft) store.fp.aircraft = store.aircraft
  store.overlays.fp = true
}

export async function submitFlightPlan() {
  const dep = store.fp.dep.trim().toUpperCase()
  const dest = store.fp.dest.trim().toUpperCase()
  if (!dep || !dest) { message(t('flightPlan.fillAirports'), true); return }
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
    message(t('flightPlan.submitted'))
    store.overlays.fp = false
  } else {
    message(errText(r) || t('flightPlan.submitFailed'), true)
  }
}

/* ---------- 调试 / 工具 ---------- */
export function toggleDebug() {
  const next = !store.set.debug
  store.set.debug = next
  if (store.api) store.api.set_debug(next)
  message(next ? t('actions.debugOn') : t('actions.debugOff'))
}

export function scrollLog(dir) {
  const bar = document.querySelector('.log-pane .el-scrollbar__wrap')
  if (bar) bar.scrollTop += dir * 140
}

/* ---------- 窗口控制（frameless） ---------- */
export function winMin() { if (store.api) store.api.win_minimize() }
export function winMax() { if (store.api) store.api.win_maximize() }
export function winClose() { if (store.api) store.api.win_close() }

export function noop() {}

/* ---------- 启动（mounted 调用） ---------- */
export function start() {
  window.addEventListener('pywebviewready', () => {
    store.bridgeReady = true
    store.api = window.pywebview.api
    addLog(t('log.welcome'), 'in')
    initStore()
  })
  setTimeout(() => {
    if (!store.bridgeReady) {
      window.__previewShown = true
      addLog(t('log.previewMode'), 'err')
    }
  }, 3000)

  window.pollEvent = pollEvent;
}