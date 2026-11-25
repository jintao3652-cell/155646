<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from 'vue'
import { useI18n } from 'vue-i18n'
import { store } from '../../store.js'
const { t } = useI18n()

const msg = ref(store.screenMsg)
let timer = null
let clock = null
const utc = ref('')

watch(() => store.screenMsg, (v) => {
  msg.value = v
  if (timer) clearTimeout(timer)
  timer = v ? setTimeout(() => (msg.value = null), 2600) : null
})

function pad(n) { return String(n).padStart(2, '0') }

function tick() {
  const d = new Date()
  utc.value = `${pad(d.getUTCHours())}:${pad(d.getUTCMinutes())}:${pad(d.getUTCSeconds())}Z`
}

onMounted(() => {
  tick()
  clock = setInterval(tick, 1000)
})
onBeforeUnmount(() => {
  if (timer) clearTimeout(timer)
  if (clock) clearInterval(clock)
})

/* 在线人数：固定两位，读数宽度稳定，不随数字变化抖动 */
function fmtCount(n) {
  const v = Number(n)
  return String(Number.isFinite(v) ? Math.max(0, Math.trunc(v)) : 0).padStart(2, '0')
}

const callsign = computed(() => (store.callsign || '---').toUpperCase())
const aircraft = computed(() => (store.aircraft || '---').toUpperCase())
const realName = computed(() => (store.set.real_name || '---').toUpperCase())
</script>

<template>
  <div class="lcd">
    <div class="lcd-body lcd-screen-msg">
      <div v-if="msg" :class="['scr-msg', msg.isErr ? 'err' : '']">{{ msg.text }}</div>
      <div v-else-if="store.connected" class="scr-idle">
        <div class="idle-ident">
          <div class="idle-line">
            <span class="idle-label">{{ t('settings.lblCallsign') }}</span><b>{{ callsign }}</b>
          </div>
          <div class="idle-line">
            <span class="idle-label">{{ t('lcd.lblAircraft') }}</span><b>{{ aircraft }}</b>
          </div>
          <div class="idle-line">
            <span class="idle-label">{{ t('settings.lblRealName') }}</span><b>{{ realName }}</b>
          </div>
        </div>
        <!-- 在线人数：RMP 风格双读数格，标签暗绿、数值亮绿 -->
        <div class="idle-traffic">
          <div class="traffic-cell">
            <span class="traffic-label">{{ t('lcd.lblPilots') }}</span>
            <span class="traffic-val">{{ fmtCount(store.plrCount) }}</span>
          </div>
          <span class="traffic-div" aria-hidden="true"></span>
          <div class="traffic-cell">
            <span class="traffic-label">{{ t('lcd.lblAtc') }}</span>
            <span class="traffic-val">{{ fmtCount(store.atcCount) }}</span>
          </div>
        </div>
        <div class="idle-clock">{{ utc }}</div>
      </div>
    </div>
  </div>
</template>

<style scoped>
  .lcd-screen-msg {
    display: flex;
    align-items: center;
    justify-content: center;
    text-align: center;
  }

  .scr-msg {
    font-family: var(--mono);
    font-size: 11px;
    line-height: 1.6;
    letter-spacing: 0.5px;
    color: var(--led-green);
    text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    white-space: pre-wrap;
    word-break: break-all;
  }

  .scr-msg.err {
    color: var(--led-amber);
    text-shadow: 0 0 6px rgba(255, 201, 94, 0.8);
  }

  .scr-idle {
    display: flex;
    flex-direction: column;
    gap: 6px;
    font-family: var(--mono);
    font-size: 10px;
    line-height: 1.4;
    letter-spacing: 0.5px;
    color: var(--led-green-dim);
    text-align: left;
  }

  .idle-ident {
    display: flex;
    flex-direction: column;
    gap: 2px;
  }

  .idle-line {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 8px;
  }

  .idle-label {
    color: #2a7a4c;
  }

  .idle-line b {
    color: var(--led-green);
    font-weight: bold;
    text-shadow: 0 0 6px rgba(53, 255, 154, 0.5);
  }

  /* 在线人数读数格：与上方身份行对齐，中间一条细分隔线 */
  .idle-traffic {
    display: flex;
    align-items: center;
    gap: 10px;
    margin-top: 1px;
    padding-top: 6px;
    border-top: 1px solid #0c1811;
  }

  .traffic-cell {
    flex: 1 1 0;
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 6px;
  }

  .traffic-label {
    font-size: 9px;
    letter-spacing: 1px;
    color: #2a7a4c;
    text-transform: uppercase;
  }

  .traffic-val {
    min-width: 2ch;
    text-align: right;
    font-size: 15px;
    font-weight: 700;
    line-height: 1;
    color: var(--led-green);
    font-variant-numeric: tabular-nums;
    text-shadow: 0 0 6px rgba(53, 255, 154, 0.6);
  }

  .traffic-div {
    width: 1px;
    height: 12px;
    background: #0f3a24;
  }

  .idle-clock {
    text-align: center;
    font-size: 12px;
    color: var(--led-green);
    text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
  }
</style>
