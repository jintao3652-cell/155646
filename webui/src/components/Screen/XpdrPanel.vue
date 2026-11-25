<script setup>
  /* 无线电 / 应答机控制台（客户端 → X-Plane 反写）。
   *
   * 覆盖四类输入：
   *   1. COM 主频 / 备频（1..3）—— 直写 dataref hz_833
   *   2. 主备互换 —— 走 sim/radios/comN_standy_flip 命令（机模面板同步）
   *   3. 应答机码 4 位（八进制）—— 写 transponder_code
   *   4. 应答机模式 / IDENT —— 走 sim/transponder/transponder_* 命令
   *
   * 设计原则：**只发指令，不做本地乐观更新**。写入后一律以回读的
   * flight_data 为准 —— 机模可能覆盖我们的写入（ToLiss/Fenix 自己管理
   * 应答机，swift/MobiFlight 也是"写入不保证生效"的语义）。
   */
  import { computed, ref, watch } from 'vue'
  import { useI18n } from 'vue-i18n'
  import {
    store, XPDR_MODE_LIST, setXpdrMode, setXpdrCode, xpdrIdent,
    setComFreq, swapCom,
  } from '../../store.js'

  const { t } = useI18n()
  const props = defineProps({
    modelValue: { type: Boolean, default: false },
    /* 打开时聚焦哪个标签：'com' | 'xpdr' */
    tab: { type: String, default: 'com' },
    /* 预选 COM 与主/备频（点哪个频率格就开哪个） */
    initCom: { type: Number, default: 1 },
    initStby: { type: Boolean, default: false },
  })
  const emit = defineEmits(['update:modelValue'])

  const visible = computed({
    get: () => props.modelValue,
    set: (v) => emit('update:modelValue', v),
  })

  const cur = ref('com')      // 当前标签
  const busy = ref(false)

  /* ---------- 输入缓冲 ----------
   * 频率：6 位数字（121800 形式）
   * 应答机：4 位八进制 */
  const freqBuf = ref('')
  const codeBuf = ref('')
  const activeCom = ref(1)
  const activeIsStby = ref(false)

  /* 打开/切换标签时回填当前值，避免从空白开始 */
  watch(visible, (v) => {
    if (!v) return
    cur.value = props.tab === 'xpdr' ? 'xpdr' : 'com'
    /* 用打开时的预选值，而不是上次停留的位置 */
    activeCom.value = props.initCom
    activeIsStby.value = props.initStby
    fillBufs()
  })
  watch(cur, () => fillBufs())

  function fillBufs() {
    codeBuf.value = (store.st.xpdr || '').replace(/[^0-7]/g, '').slice(0, 4)
    pickCom(activeCom.value, activeIsStby.value)
  }

  /* 取某 COM 的当前频率（MHz*1000 → 6 位字符串） */
  function curFreq(com, stby) {
    const v = stby ? store.st[`com${com}s`] : store.st[`com${com}`]
    const digits = String(v || '').replace(/[^0-9]/g, '')
    return digits.length === 6 ? digits : ''
  }

  function pickCom(com, stby) {
    activeCom.value = com
    activeIsStby.value = stby
    freqBuf.value = curFreq(com, stby)
  }

  function pressCom(d) {
    if (freqBuf.value.length >= 6) return
    freqBuf.value += d
  }
  function backspaceCom() {
    freqBuf.value = freqBuf.value.slice(0, -1)
  }

  /* 频率校验：118000..136975（COM 波段）。8.33 间隔允许 5 结尾。 */
  const freqValid = computed(() => {
    const s = freqBuf.value
    if (s.length !== 6) return false
    const n = Number(s)
    return n >= 118000 && n <= 136975
  })
  const freqDisplay = computed(() => {
    const s = freqBuf.value.padEnd(6, '-')
    return `${s.slice(0, 3)}.${s.slice(3)}`
  })

  async function commitFreq() {
    if (!freqValid.value || busy.value) return
    busy.value = true
    try {
      await setComFreq(activeCom.value, Number(freqBuf.value) / 1000,
                       !activeIsStby.value)
    } finally { busy.value = false }
  }

  async function doSwap(com) {
    if (busy.value) return
    busy.value = true
    try { await swapCom(com) } finally { busy.value = false }
  }

  /* ---------- 应答机 ---------- */
  function pressCode(d) {
    if (codeBuf.value.length >= 4) return
    codeBuf.value += d
  }
  function backspaceCode() {
    codeBuf.value = codeBuf.value.slice(0, -1)
  }
  const codeDisplay = computed(() =>
    codeBuf.value.padEnd(4, '-').split('').join(' '))

  async function commitCode() {
    if (codeBuf.value.length !== 4 || busy.value) return
    busy.value = true
    try { await setXpdrCode(codeBuf.value) } finally { busy.value = false }
  }

  async function pickMode(v) {
    if (busy.value) return
    busy.value = true
    try { await setXpdrMode(v) } finally { busy.value = false }
  }

  async function doIdent() {
    if (busy.value) return
    busy.value = true
    try { await xpdrIdent() } finally { busy.value = false }
  }

  const curMode = computed(() => Number(store.st.xpdrModeVal) || 0)

  /* 数字键盘布局（频率与应答机共用外观） */
  const digits = ['1', '2', '3', '4', '5', '6', '7', '8', '9']
</script>

<template>
  <el-dialog v-model="visible" :title="t('rad.title')" width="380px" align-center
             append-to-body :close-on-click-modal="true">
    <el-tabs v-model="cur">
      <!-- ==================== COM 频率 ==================== -->
      <el-tab-pane :label="t('rad.tabCom')" name="com">
        <!-- 选择目标：哪个 COM、主频还是备频 -->
        <div class="sel">
          <button v-for="c in [1,2,3]" :key="c" class="sel-b"
                  :class="{ on: activeCom === c && !activeIsStby }"
                  @click="pickCom(c, false)">COM{{ c }}</button>
          <button v-for="c in [1,2,3]" :key="'s'+c" class="sel-b stby"
                  :class="{ on: activeCom === c && activeIsStby }"
                  @click="pickCom(c, true)">COM{{ c }}<span class="sp">SBY</span></button>
        </div>

        <div class="row cur">
          <span class="lbl">{{ t('rad.current') }}</span>
          <span class="val">
            COM{{ activeCom }} {{ activeIsStby ? 'STBY' : 'ACT' }} =
            {{ (curFreq(activeCom, activeIsStby) || '———') }}
          </span>
          <button class="swap" :disabled="busy" @click="doSwap(activeCom)">
            ⇄ {{ t('rad.swap') }}
          </button>
        </div>

        <div class="entry" :class="{ bad: freqBuf.length === 6 && !freqValid }">
          {{ freqDisplay }}
        </div>

        <div class="pad">
          <button v-for="d in digits" :key="d" class="k" @click="pressCom(d)">{{ d }}</button>
          <button class="k fn" @click="backspaceCom">⌫</button>
          <button class="k fn" @click="freqBuf = ''">CLR</button>
          <button class="k fn" @click="pressCom('0')">0</button>
          <button class="k fn ok" :disabled="!freqValid || busy"
                  @click="commitFreq">SET</button>
        </div>
        <p class="hint">{{ t('rad.comHint') }}</p>
      </el-tab-pane>

      <!-- ==================== 应答机 ==================== -->
      <el-tab-pane :label="t('rad.tabXpdr')" name="xpdr">
        <div class="row cur">
          <span class="lbl">{{ t('rad.current') }}</span>
          <span class="mode">{{ store.st.xpdrPwr || '—' }}</span>
          <span class="code">{{ store.st.xpdr }}</span>
        </div>

        <div class="entry">{{ codeDisplay }}</div>

        <div class="pad">
          <button v-for="d in ['1','2','3','4','5','6','7','0']" :key="d"
                  class="k" @click="pressCode(d)">{{ d }}</button>
          <button class="k fn" @click="backspaceCode">⌫</button>
          <button class="k fn" @click="codeBuf = ''">CLR</button>
          <button class="k fn ok" :disabled="codeBuf.length !== 4 || busy"
                  @click="commitCode">SET</button>
        </div>

        <div class="modes">
          <span class="lbl">{{ t('rad.mode') }}</span>
          <div class="mode-btns">
            <button v-for="m in XPDR_MODE_LIST" :key="m.v"
                    class="m" :class="{ on: curMode === m.v }"
                    :disabled="busy" @click="pickMode(m.v)">{{ m.l }}</button>
          </div>
          <button class="ident" :disabled="busy" @click="doIdent">
            IDENT
          </button>
        </div>
        <p class="hint">{{ t('rad.xpdrHint') }}</p>
      </el-tab-pane>
    </el-tabs>
  </el-dialog>
</template>

<style scoped>
  .sel { display: grid; grid-template-columns: repeat(3, 1fr); gap: 5px; margin-bottom: 9px; }
  .sel-b {
    padding: 6px 0; font-size: 12px; font-family: monospace;
    background: #262b33; color: #cfd6df; border: 1px solid #3a4250;
    border-radius: 5px; cursor: pointer;
  }
  .sel-b:hover { background: #323945; }
  .sel-b.on { background: #1f4d36; border-color: #2e7a53; color: #8ef0bc; }
  .sel-b.stby .sp { font-size: 9px; opacity: 0.65; margin-left: 2px; }

  .row.cur {
    display: flex; align-items: center; gap: 8px; margin-bottom: 8px;
    font-size: 12px; color: #9aa4b2;
  }
  .row.cur .val { font-family: monospace; color: #cfd6df; }
  .row.cur .mode { color: var(--led-amber, #ffc95e); font-weight: 600; }
  .row.cur .code {
    margin-left: auto; font-family: monospace; font-size: 17px;
    color: var(--led-green, #35ff9a);
  }
  .swap {
    margin-left: auto; padding: 3px 8px; font-size: 11px;
    background: #2a3340; color: #cfd6df; border: 1px solid #3a4250;
    border-radius: 4px; cursor: pointer;
  }
  .swap:disabled { opacity: 0.45; cursor: not-allowed; }

  .entry {
    font-family: monospace; font-size: 29px; letter-spacing: 3px;
    text-align: center; padding: 8px 0; margin-bottom: 9px;
    background: rgba(53, 255, 154, 0.06);
    border: 1px solid rgba(53, 255, 154, 0.25); border-radius: 6px;
    color: var(--led-green, #35ff9a);
  }
  .entry.bad { border-color: #a33; color: #ff8b8b; background: rgba(255, 80, 80, 0.07); }

  .pad { display: grid; grid-template-columns: repeat(4, 1fr); gap: 6px; }
  .k {
    padding: 10px 0; font-size: 16px; font-family: monospace;
    background: #262b33; color: #e6e6e6; border: 1px solid #3a4250;
    border-radius: 5px; cursor: pointer;
  }
  .k:hover:not(:disabled) { background: #323945; }
  .k:disabled { opacity: 0.4; cursor: not-allowed; }
  .k.fn { font-size: 13px; }
  .k.ok { background: #1f4d36; border-color: #2e7a53; color: #8ef0bc; }

  .modes { display: flex; flex-direction: column; gap: 6px; margin-top: 10px; }
  .lbl { font-size: 12px; color: #9aa4b2; }
  .mode-btns { display: grid; grid-template-columns: repeat(4, 1fr); gap: 5px; }
  .m {
    padding: 6px 0; font-size: 11px; letter-spacing: 0.3px;
    background: #262b33; color: #cfd6df; border: 1px solid #3a4250;
    border-radius: 5px; cursor: pointer;
  }
  .m:hover:not(:disabled) { background: #323945; }
  .m.on { background: #4a3a12; border-color: #a3781f; color: #ffc95e; }
  .m:disabled { opacity: 0.5; cursor: not-allowed; }

  .ident {
    margin-top: 4px; padding: 8px 0; font-size: 13px; letter-spacing: 2px;
    background: #3a2a2a; color: #ffb3b3; border: 1px solid #6a3a3a;
    border-radius: 5px; cursor: pointer;
  }
  .ident:hover:not(:disabled) { background: #4a3232; }
  .ident:disabled { opacity: 0.45; cursor: not-allowed; }

  .hint { margin: 9px 0 0; font-size: 11px; color: #7b8494; line-height: 1.5; }
</style>