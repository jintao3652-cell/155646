/* XFlySim-Client 全局状态与逻辑（Vue3 响应式 store）。
 * 数据来源：pywebview → Api（事件轮询 pull / 直接调用）。
 * 界面文本统一走 vue-i18n（en / zh-CN / zh-TW，语言保存在 config.json）。
 * 状态标签用 computed 从原始状态派生，切换语言即时刷新。 */
import { reactive, computed } from 'vue'
import { ElMessage, ElMessageBox } from 'element-plus'
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
       gs: '—', alt: '—',
       lat: null, lon: null },   // 本机位置（在线列表算距离用）
  fsdState: 'Disconnected',   // Online / Connecting / WaitLogin / Disconnected / ...
  /* XC（XConnect/X-Plane）与 MC（MConnect/MSFS）是两个同级独立的桥分类，
   * 各有自己的状态与标签，MC 不再挂在 XC 名下 */
  xpState: 'waiting',          // XC：waiting / connected / failed / disconnected
  mcState: 'waiting',          // MC：waiting / connected / failed / disconnected
  cslLoaded: [],
  cslMissing: [],
  cslSim: 'xplane',            // 状态来自哪座桥：xplane(X-CSL) / msfs(FSLTL/AIG)
  roster: '',
  atcCount: 0, plrCount: 0,
  log: [],
  msgs: [],
  msgTo: '', msgText: '',
  fp: { type: 'I', speed: '450', alt: 'FL350', dep: '',
        dest: '', altapt: '', plan_time: '0000', actual: '0000',
        enrh: 0, enrm: 0, fuelh: 0, fuelm: 0, remarks: '', route: '',
        registration: '' },
  sbUser: '',         // SimBrief 用户名 / 用户 ID（记忆上次输入）
  sbBusy: false,      // 导入请求进行中
  sbInfo: '',         // 上次导入摘要（起降场 · 机型 · 航程）
  sbError: '',        // 上次导入失败原因（面板内常驻显示，toast 消失后仍可见）
  set: { real_name: '', cid: '', password: '', remember: true,
         callsign: '', airline: '',
         server_host: '', server_port: 6809,
         xconnect_host: '', xconnect_port: 51001, debug: false,
         language: getLocale() },
  overlays: { msg: false, set: false, fp: false },
  newMsg: false,
  screenMsg: null,
  version: '',             // 当前版本（后端 get_version，右下角/更新屏共用）
  /* 在线更新：状态机由后端 UpdateManager 推送 update 事件驱动。
   * state: idle/disabled/checking/available/uptodate/downloading/
   *        restarting/error/updated（disabled = 未配置更新接口地址） */
  update: {
    state: 'idle',
    current: '', latest: '', notes: '', created: '', softwareName: '',
    percent: 0, received: 0, total: 0,
    error: '', errorCode: '', silent: false, manual: false,
    dismissed: false,      // 用户点了"稍后"，本次运行不再打扰
  },
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

/* XC（XConnect / X-Plane）状态标签 */
export const xpText = computed(() => {
  switch (store.xpState) {
    case 'connected': return t('status.xcFound')
    case 'failed': return t('status.xcError')
    default: return t('status.xcNotFound')
  }
})
export const xpTag = computed(() => {
  switch (store.xpState) {
    case 'connected': return 'success'
    case 'failed': return 'danger'
    default: return 'info'
  }
})

/* MC（MConnect / MSFS）状态标签：与 XC 同层级、互不包含 */
export const mcText = computed(() => {
  switch (store.mcState) {
    case 'connected': return t('status.mcFound')
    case 'failed': return t('status.mcError')
    default: return t('status.mcNotFound')
  }
})
export const mcTag = computed(() => {
  switch (store.mcState) {
    case 'connected': return 'success'
    case 'failed': return 'danger'
    default: return 'info'
  }
})

/* 任一座桥在线（XC 或 MC）——"模拟机已发现"的通用判定 */
export const bridgeActive = computed(() =>
  store.xpState === 'connected' || store.mcState === 'connected')

const cslTotal = computed(() => store.cslLoaded.length + store.cslMissing.length)
/* MC 与 XC 共用 csl_status 通道，但语义不同：XC 是 X-CSL 包，MC 是
 * MSFS 里注入的 AI 模型（FSLTL/AIG）。标签与提示必须按 sim 分开。 */
const cslLabel = computed(() => store.cslSim === 'msfs'
  ? t('status.msfsAiTitle') : t('status.cslTitle'))
const cslShort = computed(() => store.cslSim === 'msfs' ? 'AI' : 'CSL')
export const cslText = computed(() => {
  if (cslTotal.value === 0) return `${cslShort.value} —`
  return `${cslShort.value} ${store.cslLoaded.length}/${cslTotal.value}`
})
export const cslTag = computed(() => {
  if (cslTotal.value === 0) return 'info'
  return store.cslMissing.length ? 'danger' : 'success'
})
export const cslTitle = computed(() => {
  if (cslTotal.value === 0) return cslLabel.value
  return `${cslLabel.value}: ${store.cslLoaded.length}/${cslTotal.value}`
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

/* 把后端 get_settings() 结果整份灌入 store.set（供设置弹窗与 LCD 设置页共用）。
 * 必须以完整对象替换，字段命名与 config（server_host/server_port/
 * xconnect_host/xconnect_port）保持一致，否则 LCD 设置页读取不到。 */
function applySettings(s) {
  store.set = {
    real_name: s.real_name || '',
    cid: s.cid || '',
    password: s.password || '',
    remember: !!s.remember_password,
    callsign: s.callsign || '',
    airline: s.airline_icao || '',
    aircraft_manual: s.aircraft_manual || '',
    server_host: s.server_host || '',
    server_port: s.server_port || 6809,
    xconnect_host: s.xconnect_host || '',
    xconnect_port: s.xconnect_port || 51001,
    debug: !!s.debug_mode,
    msfs_traffic_lib: s.msfs_traffic_lib || 'auto',
    language: normalizeLocale(s.language),
  }
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
    /* 版本号与更新状态：后端在构造时已自动查过一次更新，这里补一次快照，
     * 避免"检查结果早于前端挂载"时界面空白。 */
    try {
      const v = await store.api.get_version()
      if (v && v.version) {
        store.version = v.version
        if (!store.update.current) store.update.current = v.version
      }
    } catch (e) { /* 忽略 */ }
    try {
      applyUpdateState(await store.api.update_state())
    } catch (e) { /* 忽略 */ }
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
  /* XC / MC 同级独立：谁在线点亮谁，互不覆盖 */
  if (s.xconnect) store.xpState = 'connected'
  if (s.mconnect) store.mcState = 'connected'
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
    case 'log': {
      /* 后端统一打 [DEBUG|INFO|WARN|ERROR] 标签；标签只用于着色，正文不改。
       * 原始 FSD 报文（> 发送 / < 接收）只在调试模式显示。 */
      const raw = String(ev.data == null ? '' : ev.data)
      const lv = raw.match(/^\[(DEBUG|INFO|WARN|ERROR)\]\s*/)
      const level = lv ? lv[1] : ''
      const body = lv ? raw.slice(lv[0].length) : raw
      if (!store.set.debug && (body.startsWith('> ') || body.startsWith('< '))) break
      const isErr = /错误|失败|error|failed|exception/i.test(body)
      const isWarn = /断开|断线|离线|未发现|未连接|未上线|超时|timeout|disconnect|offline|not found/i.test(body)
      let cls = 'out'
      if (body.startsWith('< ')) cls = 'in'
      else if (body.startsWith('> ')) cls = 'out'
      else if (level === 'ERROR') cls = 'err'
      else if (level === 'WARN') cls = 'warn'
      else if (level === 'DEBUG') cls = 'debug'
      else if (body.startsWith('[fsd]')) cls = isErr ? 'err' : isWarn ? 'warn' : 'txt'
      /* [xconnect]/[mconnect]/[bridge] 三类桥日志同级着色（探测未定归属时用 [bridge]） */
      else if (body.startsWith('[xconnect]') || body.startsWith('[mconnect]') ||
               body.startsWith('[bridge]'))
        cls = isErr ? 'err' : isWarn ? 'warn' : 'txt'
      addLog(raw, cls)
      break
    }
    case 'text':
      addLog(`${t('log.msgPrefix')} ${ev.data[0]}: ${ev.data[1]}`, 'txt')
      appendMsg(ev.data[0], ev.data[1], false)
      break
    case 'flight': updateFlight(ev.data); break
    case 'registration':
      store.reg = ev.data.reg || ''
      store.regSrc = ev.data.src || ''
      break
    case 'xconnect': setBridgeState('xpState', ev.data); break
    case 'mconnect': setBridgeState('mcState', ev.data); break
    case 'csl': setCslStatus(ev.data); break
    case 'update': applyUpdateState(ev.data); break
    case 'xplog': addLog('◂ ' + JSON.stringify(ev.data), 'in'); break
    case 'sys': addLog(`[!] ${ev.data}`, 'err'); break
    case 'fsdstate': updateStatus(ev.data); break
    case 'disconnected':
      store.connected = false
      store.atcCount = 0
      store.plrCount = 0
      addLog(t('log.disconnected'), 'warn')
      break
  }
}

/** 桥状态写入：XC→store.xpState，MC→store.mcState（两个同级分类互不覆盖）。 */
function setBridgeState(key, state) {
  if (key === 'mcState' && state === 'failed') {
    addLog(t('log.mcNotRunning'), 'warn')
  } else if (key === 'xpState' && state === 'failed') {
    addLog(t('log.simNotRunning'), 'warn')
  }
  store[key] = state
}

function setCslStatus(d) {
  store.cslSim = d.sim === 'msfs' ? 'msfs' : 'xplane'
  store.cslLoaded = d.loaded || []
  store.cslMissing = d.missing || []
  if (store.cslMissing.length) {
    const key = store.cslSim === 'msfs'
      ? 'status.msfsAiMissing' : 'status.cslMissing'
    addLog(t(key, { list: store.cslMissing.join(', ') }), 'err')
  }
}

/* ---------- 在线更新 ----------
 *
 * 后端 xfly_client/update_service.py 的 UpdateManager 把状态推成 update 事件：
 *   checking → available | uptodate（→ downloading → restarting）| error
 * 界面只在 LCD 主屏右下角提示 + 独立的更新小屏里操作，不弹大对话框。 */

/** 合并一次状态快照；保留 dismissed 等纯前端字段。 */
function applyUpdateState(d) {
  if (!d || typeof d !== 'object') return
  const prev = store.update.state
  store.update = { ...store.update, ...d, percent: Number(d.percent) || 0 }
  if (d.current) store.version = d.current
  if (d.state === prev) return
  if (d.state === 'available') {
    store.update.dismissed = false      // 发现新版本重新提醒
    addLog(t('update.logAvailable', { version: d.latest || '' }), 'in')
  } else if (d.state === 'updated') {
    const text = t('update.logUpdated', { version: d.latest || '' })
    addLog(text, 'in')
    toast(text, 'success', 4000)
  } else if (d.state === 'restarting') {
    toast(t('update.restarting'), 'success', 3000)
    /* 助手已在等本进程退出：留一点时间让提示画出来，再关窗 */
    setTimeout(() => { try { store.api.win_close() } catch (e) { /* 忽略 */ } }, 900)
  } else if (d.state === 'error' && d.manual) {
    message(errText({ code: d.errorCode, error: d.error }) || t('update.failed'), true)
  }
}

/** LCD 主屏是否要显示"有更新"角标（点了催办过就不再闪）。 */
export const updateAvailable = computed(() =>
  ['available', 'downloading', 'restarting'].includes(store.update.state) &&
  !store.update.dismissed)

/** 下载进度百分比（后端只给 received/total 时也能算）。 */
export const updatePercent = computed(() => {
  const u = store.update
  if (u.state !== 'downloading') return 0
  if (u.percent > 0) return u.percent
  if (u.total > 0) return Math.floor((u.received * 100) / u.total)
  return 0
})

/** 检查更新：manual=true 时失败会提示，启动自动检查失败则静默。 */
export async function checkUpdate(manual = true) {
  if (!store.api) return
  const r = await store.api.check_update(!!manual)
  if (r && r.ok === false) {
    if (r.code === 'update_no_url') {
      if (manual) message(t('update.noUrl'), true)
    } else {
      message(errText(r) || t('update.failed'), true)
    }
  }
}

/** 下载 + 校验 + 拉起更新助手；助手起来后应用会自动退出并重启。 */
export async function startUpdate() {
  if (!store.api) return
  const u = store.update
  if (['downloading', 'restarting'].includes(u.state)) return
  try {
    await ElMessageBox.confirm(
      t('update.confirm', { version: u.latest || '' }),
      t('update.confirmTitle'),
      {
        confirmButtonText: t('update.confirmOk'),
        cancelButtonText: t('update.later'),
        type: 'warning',
        closeOnClickModal: false,
      },
    )
  } catch (e) {
    return   /* 取消 */
  }
  const r = await store.api.apply_update()
  if (!r || !r.ok) message(errText(r) || t('update.failed'), true)
}

/** 稍后再说：本次运行不再提醒（下次启动会重新检查）。 */
export function dismissUpdate() {
  store.update.dismissed = true
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

/* ---------- 在线列表工具（距离 / 时长） ---------- */

/** 大圆距离（海里）；任一坐标为无效值返回 null。 */
export function haversineNm(lat1, lon1, lat2, lon2) {
  const ok = (v) => typeof v === 'number' && isFinite(v)
  if (!ok(lat1) || !ok(lon1) || !ok(lat2) || !ok(lon2)) return null
  const R = 3440.065   // 地球半径（海里）
  const r = (d) => (d * Math.PI) / 180
  const dLat = r(lat2 - lat1)
  const dLon = r(lon2 - lon1)
  const a = Math.sin(dLat / 2) ** 2 +
    Math.cos(r(lat1)) * Math.cos(r(lat2)) * Math.sin(dLon / 2) ** 2
  return 2 * R * Math.asin(Math.min(1, Math.sqrt(a)))
}

/** 与目标（ATC/飞行员）的距离文本，如 "12.3nm"；无本机位置/坐标时 "—"。 */
export function distText(lat, lon) {
  const d = haversineNm(store.st.lat, store.st.lon, lat, lon)
  if (d == null) return '—'
  return d < 100 ? `${d.toFixed(1)}nm` : `${d.toFixed(0)}nm`
}

/** 在线时长文本：H:MM:SS（<1h 时 M:SS）。 */
export function fmtDuration(sec) {
  sec = Math.max(0, Math.floor(sec || 0))
  const h = Math.floor(sec / 3600)
  const m = Math.floor((sec % 3600) / 60)
  const s = sec % 60
  const mm = String(m).padStart(2, '0')
  const ss = String(s).padStart(2, '0')
  return h > 0 ? `${h}:${mm}:${ss}` : `${m}:${ss}`
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
    aircraft_manual: s.aircraft_manual || '',
    // 字段名必须与模板 v-model / saveSettings() 一致。
    // 旧代码用 host/port/xphost/xpport，打开设置后保存会在
    // `server_host.trim()` 处抛错，导致手填机型等全部设置都没写入。
    // 配置缺字段时回填默认值，避免设置页出现空输入框后被误保存为空。
    server_host: s.server_host || 'ol.xflysim.com',
    server_port: s.server_port || 6809,
    xconnect_host: s.xconnect_host || '127.0.0.1',
    xconnect_port: s.xconnect_port || 51001,
    debug: !!s.debug_mode,
    msfs_labels: !!s.msfs_toggle_labels,
    traffic_lib: s.msfs_traffic_lib || 'auto',
    language: normalizeLocale(s.language),
  }
  // 记住打开时的开关状态：保存时对比变化，变化才下发 toggle
  store.msfsLabelsPrev = !!s.msfs_toggle_labels
  store.trafficLibPrev = s.msfs_traffic_lib || 'auto'
  store.overlays.set = true
}

export async function exportDiagnostics() {
  if (!store.api) return
  try {
    const r = await store.api.export_diagnostics()
    if (r && r.ok) {
      message(t('settings.diagnosticsSaved', { path: r.path }))
    } else {
      message(t('settings.diagnosticsFailed'), true)
    }
  } catch (e) {
    message(t('settings.diagnosticsFailed'), true)
  }
}

export async function saveSettings() {
  const s = store.set
  // 兼容旧内存状态（早期 openSettings 写的是 host/port/xphost/xpport）：
  // 否则任一字段 undefined 会在 .trim() 抛错，整次保存静默失败。
  if (s.server_host == null && s.host != null) s.server_host = s.host
  if (s.server_port == null && s.port != null) s.server_port = s.port
  if (s.xconnect_host == null && s.xphost != null) s.xconnect_host = s.xphost
  if (s.xconnect_port == null && s.xpport != null) s.xconnect_port = s.xpport
  // 手填机型必须是 2~4 位字母数字（与后端校验一致）；非法值宁可拦下来提示，
  // 也不要静默忽略后退回自动识别，否则用户会看到"我填了却说没填"。
  const acManual = (s.aircraft_manual || '').trim().toUpperCase()
  if (acManual && !/^[A-Z0-9]{2,4}$/.test(acManual)) {
    message(`${t('errors.field.aircraft')}: ${acManual}`, true)
    return
  }
  const d = {
    real_name: s.real_name.trim(),
    cid: s.cid.trim(),
    password: s.remember ? s.password : '',
    remember_password: s.remember,
    callsign: s.callsign.trim().toUpperCase(),
    airline_icao: s.airline.trim().toUpperCase(),
    aircraft_manual: acManual,
    server_host: s.server_host.trim(),
    server_port: parseInt(s.server_port, 10),
    xconnect_host: s.xconnect_host.trim(),
    xconnect_port: parseInt(s.xconnect_port, 10),
    debug_mode: store.set.debug,
    msfs_toggle_labels: !!store.set.msfs_labels,
    msfs_traffic_lib: store.set.traffic_lib || 'auto',
    language: normalizeLocale(store.set.language),
  }
  store.api.set_debug(d.debug_mode)
  const r = await store.api.save_settings(d)
  if (r.ok) {
    store.callsign = d.callsign
    store.airline = d.airline_icao
    // 手填机型立即生效到工具栏；清空则保留当前自动识别值，
    // 等下一帧插件数据推来时恢复（后端已按 aircraft_manual 处理覆盖）
    if (d.aircraft_manual) store.aircraft = d.aircraft_manual
    onCallsignInput()
    applyLanguage(d.language)
    // MSFS 铭牌开关：保存时与打开前状态不同 → 立即下发 toggle，不再等重启
    const labelsNow = !!store.set.msfs_labels
    if (labelsNow !== !!store.msfsLabelsPrev) {
      try {
        const lr = await store.api.toggle_msfs_labels()
        if (lr && lr.ok) {
          store.msfsLabelsPrev = labelsNow
          // 成功日志由后端统一推送并落盘，前端不再重复打印
        } else if (lr && lr.code === 'not_connected') {
          message(t('log.msfsLabelsNotConnected'), true)
        } else {
          message(t('log.msfsLabelsFailed'), true)
        }
      } catch (e) {
        message(t('log.msfsLabelsFailed'), true)
      }
    }
    // MSFS AI 模型库：保存时变了就立即下发（仅 MConnect；XC 固定 X-CSL）
    const libNow = store.set.traffic_lib || 'auto'
    if (libNow !== store.trafficLibPrev) {
      try {
        const rr = await store.api.set_msfs_traffic_lib(libNow)
        if (rr && rr.ok) {
          store.trafficLibPrev = libNow
          // 成功日志由后端统一推送并落盘，前端不再重复打印
        } else if (rr && (rr.code === 'not_connected' || rr.code === 'not_mconnect')) {
          message(t('log.msfsTrafficLibNotConnected'), true)
        } else {
          message(t('log.msfsTrafficLibFailed'), true)
        }
      } catch (e) {
        message(t('log.msfsTrafficLibFailed'), true)
      }
    }
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
    const okText = store.regManual
      ? t('flightPlan.submittedReg', { reg: store.regManual })
      : t('flightPlan.submitted')
    message(okText)
    /* 抽屉关闭后 LCD 主屏会被新页面盖住，额外给一个轻提示确保用户看到成功 */
    toast(okText, 'success')
    store.overlays.fp = false
  } else {
    const why = errText(r) || t('flightPlan.submitFailed')
    message(why, true)
    toast(why, 'error', 3500)
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
export async function winClose() {
  if (!store.api) return
  /* 不用 pywebview 的 confirm_close：它是原生对话框，在 420x577 小窗口里
   * 英文文案换行后会被裁掉（"Do you really want to" 少了 exit）。改用
   * 应用内确认框，文案完整、可随语言切换、还能和面板风格一致。 */
  try {
    await ElMessageBox.confirm(
      t('actions.exitMessage'),
      t('actions.exitTitle'),
      {
        confirmButtonText: t('actions.exitConfirm'),
        cancelButtonText: t('settings.cancel'),
        type: 'warning',
        closeOnClickModal: false,
      },
    )
  } catch (e) {
    return   /* 取消 */
  }
  try { store.api.win_close() } catch (e) { /* 忽略关闭异常 */ }
}

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