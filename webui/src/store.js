/* XFlySim-Client 全局状态与逻辑（Vue3 响应式 store）。
 * 数据来源：pywebview → Api（事件轮询 pull / 直接调用）。
 * 界面文本统一走 vue-i18n（en / zh-CN / zh-TW，语言保存在 config.json）。
 * 状态标签用 computed 从原始状态派生，切换语言即时刷新。 */
import { reactive, computed } from 'vue'
import { ElMessage } from 'element-plus'
import i18n, { normalizeLocale, setLocale, getLocale } from './i18n'

const t = (k, p) => i18n.global.t(k, p)

export const store = reactive({
  api: null,
  bridgeReady: false,
  connected: false,
  busy: false,
  callsign: '', aircraft: '', airline: '',
  reg: '',            // 生效注册号（尾号）：手动 > 模拟器 > 上次记录
  regSrc: '',         // manual / tailnum / livery / cache / ''
  regManual: '',      // 用户手填值（设置/飞行计划），空 = 自动
  sim: 'xp12',
  lang: getLocale(),
  st: { com1: '———', com2: '———', com3: '———',
       com1s: '———', com2s: '———', com3s: '———',
       /* 音频面板（来自 flight_data）：断电/未选中收听时 RMP 应提示不可用 */
       avionicsOn: false, com1Pwr: false, com2Pwr: false, com3Pwr: false,
       audSel1: false, audSel2: false, audSel3: false,
       hasCom3Sel: false,   /* 该机模是否提供 audio_sel_com3（决定 COM3 可用性判定） */
       audVol1: 0, audVol2: 0,
       audSel1Cp: false, audSel2Cp: false, audVol1Cp: 0, audVol2Cp: 0,
       txCom: 0, pilotXmit: -1,
       xpdr: '———', xpdrMode: '', xpdrModeVal: 0, xpdrPwr: '', xpdrSys: '',
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
        enrh: 0, enrm: 0, fuelh: 0, fuelm: 0, remarks: '', route: '',
        registration: '' },
  sbUser: '',         // SimBrief 用户名 / 用户 ID（记忆上次输入）
  sbBusy: false,      // 导入请求进行中
  sbInfo: '',         // 上次导入摘要（起降场 · 机型 · 航程）
  sbError: '',        // 上次导入失败原因（面板内常驻显示，toast 消失后仍可见）
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
    /* 后端用 snake_case（simbrief_no_plan），语言包用 camelCase（simbriefNoPlan） */
    const camel = String(r.code).replace(/_([a-z0-9])/g, (_, c) => c.toUpperCase())
    for (const k of [camel, r.code]) {
      const key = `errors.${k}`
      const mapped = t(key)
      if (mapped && mapped !== key) return mapped
    }
  }
  return r.error || ''
}

export function message(data, isErr = false) {
  store.screenMsg = { text: data, isErr: !!isErr, id: Date.now() }
  addLog(`[${isErr ? '!' : 'i'}] ${data}`, isErr ? 'err' : 'txt')
}

/** 轻提示（右上角浮层，2 秒后自动消失）。只做反馈，不进 LCD、不进日志。 */
/** 轻提示（浮层，2 秒后自动消失）。只做反馈，不进 LCD、不进日志。
 *
 *  zIndex 必须高于 el-overlay（2000），否则在 FPL/设置等全屏抽屉打开时
 *  提示会被抽屉整块盖住，用户完全看不到。
 */
export function toast(data, type = 'success', duration = 2000) {
  ElMessage({
    message: data,
    type,
    duration,
    showClose: false,
    grouping: true,
    customClass: 'xp-toast',
    offset: 8,
    zIndex: 5000,
  })
}

/** 航司由呼号前三位自动派生（如 CCA1501 → CCA）。 */
export function onCallsignInput() {
  store.airline = (store.callsign || '').replace(/[^A-Za-z0-9]/g, '').slice(0, 3).toUpperCase()
}

/* ---------- 初始化 ---------- */
export async function initStore() {
  try {
    const s = await store.api.get_settings()
    if (!store.callsign) store.callsign = s.callsign
    if (!store.aircraft) store.aircraft = s.aircraft_icao
    if (!store.airline) store.airline = s.airline_icao
    onCallsignInput()
    /* 注册号：有手填值就用（显示为手填），否则等插件读模拟器 */
    store.regManual = s.registration || ''
    store.fp.registration = store.regManual
    if (store.regManual) { store.reg = store.regManual; store.regSrc = 'manual' }
    store.set.real_name = s.real_name || ''
    store.sbUser = s.simbrief_user || ''
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
  if (s.reg !== undefined) { store.reg = s.reg || ''; store.regSrc = s.reg_src || '' }
}

/** 生效注册号显示文本（未知显示 —）。 */
export const regText = computed(() => store.reg || '—')
/** 注册号来源的本地化文本。 */
export const regSrcText = computed(() => {
  if (!store.regSrc) return ''
  const key = `regSrc.${store.regSrc}`
  const v = t(key)
  return v === key ? store.regSrc : v
})

function handleEvent(ev) {
  switch (ev.event) {
    case 'log': addLog(ev.data, 'out'); break
    case 'text':
      addLog(`${t('log.msgPrefix')} ${ev.data[0]}: ${ev.data[1]}`, 'txt')
      appendMsg(ev.data[0], ev.data[1], false)
      break
    case 'flight': updateFlight(ev.data); break
    case 'registration':
      store.reg = ev.data.reg || ''
      store.regSrc = ev.data.src || ''
      break
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
  /* 音频面板：航电断电时频率虽在但不可用（对齐 XVoice 的 PowerOn 判定） */
  store.st.avionicsOn = !!fd.avionics_on
  /* 单个 COM 电源：比 avionics_on 更精确（部分机型可单独关某个 COM）。
   * 插件在 dataref 缺失时会用 avionics_on 兜底，所以这里可直接用。 */
  store.st.com1Pwr = fd.com1_power === undefined ? !!fd.avionics_on : !!fd.com1_power
  store.st.com2Pwr = fd.com2_power === undefined ? !!fd.avionics_on : !!fd.com2_power
  store.st.com3Pwr = fd.com3_power === undefined ? !!fd.avionics_on : !!fd.com3_power
  store.st.audSel1 = !!fd.audio_sel_com1
  store.st.audSel2 = !!fd.audio_sel_com2
  store.st.audSel3 = !!fd.audio_sel_com3
  store.st.hasCom3Sel = !!fd.has_audio_sel_com3
  store.st.audVol1 = Number(fd.audio_vol_com1) || 0
  store.st.audVol2 = Number(fd.audio_vol_com2) || 0
  store.st.audSel1Cp = !!fd.audio_sel_com1_copilot
  store.st.audSel2Cp = !!fd.audio_sel_com2_copilot
  store.st.audVol1Cp = Number(fd.audio_vol_com1_copilot) || 0
  store.st.audVol2Cp = Number(fd.audio_vol_com2_copilot) || 0
  store.st.txCom = Number(fd.tx_com) || 0
  store.st.pilotXmit = Number(fd.pilot_xmit ?? -1)
  store.st.xpdr = String(fd.transponder).padStart(4, '0')
  store.st.xpdrMode = XPDR_MODES[fd.xpdr_mode] || (fd.squark === 'S' ? 'ON' : 'OFF')
  /* 原始枚举值：控制面板高亮当前模式要用（不能靠字符串反查，因为
   * 机型自定义字段可能把它覆盖成 STBY/AUTO/ON 之类） */
  store.st.xpdrModeVal = Number(fd.xpdr_mode) || 0
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
  /* 机型：后端已在 Python 侧做过推断（有些机模 acf 里没有 ICAO，
   * 会用 swift 机型库按名字反查），这里直接采用，不再自己判断空值。
   * 注意**不能**写成 `if (fd.aircraft_icao && ...)` —— 那会在机模缺 ICAO
   * 时跳过更新，工具栏一直显示上一架飞机的机型（实测切 F-14 后仍显示
   * MD82）。空值也是合法状态，照常写入。 */
  const icao = (fd.aircraft_icao || '').toUpperCase()
  if (icao !== store.aircraft) {
    store.aircraft = icao
    if (icao) addLog(t('log.aircraftDetected', { icao }), 'in')
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
    onCallsignInput()
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

/* ---------- 模拟器反写（RMP 面板 → X-Plane） ----------
 *
 * 与 sendMsg 里 @频率 的区别：那条走 FSD 文本（告诉服务器我在守哪个频率），
 * 这里真正改座舱里的无线电/应答机（XConnect 插件的 Send:RADIO / Send:XPDR）。
 * 写入后不直接改本地显示 —— 以回读的 flight_data 为准，避免机模覆盖导致假象。 */

/* 应答机模式：与 X-Plane transponder_mode 枚举一致（共 8 个）
 * 0=Off 1=Stby 2=On(Mode A) 3=Alt(Mode C) 4=Test 5=GND 6=TA only 7=TA/RA */
export const XPDR_MODE_LIST = [
  { v: 0, l: 'OFF' }, { v: 1, l: 'STBY' }, { v: 2, l: 'ON' },
  { v: 3, l: 'ALT' }, { v: 4, l: 'TEST' }, { v: 5, l: 'GND' },
  { v: 6, l: 'TA' }, { v: 7, l: 'TA/RA' },
]

export async function xpdrIdent() {
  if (!store.api) return false
  const r = await store.api.xp_xpdr_ident()
  if (!r.ok) { message(errText(r) || t('actions.setFailed'), true); return false }
  return true
}

export async function setXpdrMode(mode) {
  if (!store.api) return false
  const r = await store.api.xp_set_xpdr_mode(Number(mode) || 0)
  if (!r.ok) { message(errText(r) || t('actions.setFailed'), true); return false }
  return true
}

export async function setXpdrCode(code) {
  if (!store.api) return false
  const s = String(code || '')
  if (!/^[0-7]{4}$/.test(s)) { message(t('actions.squawkInvalid'), true); return false }
  const r = await store.api.xp_set_xpdr_code(s)
  if (!r.ok) { message(errText(r) || t('actions.setFailed'), true); return false }
  return true
}

export async function setComFreq(com, mhz, active = true) {
  if (!store.api) return false
  const r = await store.api.xp_set_com(Number(com), Number(mhz), !!active)
  if (!r.ok) { message(errText(r) || t('actions.setFailed'), true); return false }
  return true
}

export async function swapCom(com) {
  if (!store.api) return false
  const r = await store.api.xp_swap_com(Number(com))
  if (!r.ok) { message(errText(r) || t('actions.setFailed'), true); return false }
  return true
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
    onCallsignInput()
    applyLanguage(d.language)
    message(store.set.debug ? t('actions.savedDebug') : t('actions.saved'))
    store.overlays.set = false
  } else {
    message(errText(r) || t('actions.saveFailed'), true)
  }
}

/* ---------- 账号页单独保存（不覆盖服务器等其它设置） ---------- */
function saveAccountPayload(s) {
  return {
    real_name: store.set.real_name.trim(),
    cid: store.set.cid.trim(),
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
  }
}

async function saveAccountPatch(extra = {}) {
  const s = await store.api.get_settings()
  const d = { ...saveAccountPayload(s), ...extra }
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

/* 服务器地址/端口单独保存（LCD 设置 → Server 屏）：
   必须以 store.set 中的最新值覆盖保存的旧值，且端口转数字。 */
export async function saveServerPatch() {
  const s = await store.api.get_settings()
  const d = {
    ...saveAccountPayload(s),
    server_host: store.set.server_host.trim(),
    server_port: Number(store.set.server_port) || s.server_port,
  }
  const r = await store.api.save_settings(d)
  if (!r.ok) message(errText(r) || t('actions.saveFailed'), true)
  return r.ok
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
  if (!store.fp.registration) store.fp.registration = store.regManual
  store.overlays.fp = true
}

/* ---------- SimBrief 导入 ---------- */

/** 把后端解析好的计划填进表单（只覆盖有值的字段，空值保留用户已填内容）。 */
function applySimbriefPlan(p) {
  const put = (key, val) => {
    if (val === undefined || val === null) return
    const s = String(val).trim()
    if (s) store.fp[key] = s
  }
  const rule = String(p.type || '').trim().toUpperCase()
  if (['I', 'V', 'S', 'D'].includes(rule)) store.fp.type = rule
  put('aircraft', p.aircraft)
  put('registration', p.registration)
  put('speed', p.cruise_speed)
  put('alt', p.cruise_alt)
  put('dep', p.dep)
  put('dest', p.dest)
  put('altapt', p.alt)
  put('plan_time', p.plan_time)
  put('actual', p.actual_time)
  put('remarks', p.remarks)
  put('route', p.route)
  store.fp.enrh = Number(p.enroute_h) || 0
  store.fp.enrm = Number(p.enroute_m) || 0
  store.fp.fuelh = Number(p.fuel_h) || 0
  store.fp.fuelm = Number(p.fuel_m) || 0
}

/** 按 SimBrief 用户名/ID 导入最新一份计划（填入表单，不自动提交）。
 *
 *  失败反馈走两条路：
 *  ① `toast(..., 'error')` —— 浮层，在 FPL 全屏抽屉之上（z-index 5000）；
 *  ② `store.sbError` —— 面板内常驻一行红字，toast 消失后仍能回看失败原因。
 *  早期只用 `message()`（写 LCD 主屏），但 LCD 被抽屉整块盖住，用户看到的效果
 *  就是"转圈 3 秒后没反应"。
 */
export async function importSimbrief() {
  if (!store.api || store.sbBusy) return
  const user = (store.sbUser || '').trim()
  if (!user) {
    store.sbError = t('flightPlan.sbNeedUser')
    toast(store.sbError, 'error', 3000)
    return
  }
  store.sbBusy = true
  store.sbError = ''
  try {
    const r = await store.api.simbrief_import(user)
    if (!r || !r.ok) {
      const why = (r && (errText(r) || r.error)) || t('flightPlan.sbFailed')
      store.sbError = why
      toast(why, 'error', 3500)
      addLog(`[!] SimBrief: ${why}`, 'err')
      return
    }
    const p = r.plan || {}
    applySimbriefPlan(p)
    store.sbUser = r.username || user
    const dur = `${Number(p.enroute_h) || 0}:${String(Number(p.enroute_m) || 0).padStart(2, '0')}`
    store.sbInfo = [p.dep && p.dest ? `${p.dep} → ${p.dest}` : '',
                    p.aircraft || '', dur].filter(Boolean).join(' · ')
    // 成功只用轻提示（2s 自动消失），不占用 LCD 主屏
    const okText = t('flightPlan.sbImported',
                     { dep: p.dep || '—', dest: p.dest || '—' })
    toast(okText, 'success')
    addLog(`[i] ${okText}`, 'txt')
  } catch (e) {
    const why = `${t('flightPlan.sbFailed')}: ${e}`
    store.sbError = why
    toast(why, 'error', 3500)
    addLog(`[!] ${why}`, 'err')
  } finally {
    store.sbBusy = false
  }
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
    registration: store.fp.registration.trim().toUpperCase(),
  })
  if (r.ok) {
    /* 手填值（可能为空 = 恢复自动）立即反映到界面 */
    store.regManual = store.fp.registration.trim().toUpperCase()
    if (store.regManual) { store.reg = store.regManual; store.regSrc = 'manual' }
    message(store.regManual
      ? t('flightPlan.submittedReg', { reg: store.regManual })
      : t('flightPlan.submitted'))
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