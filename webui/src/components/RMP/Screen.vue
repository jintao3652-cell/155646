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

const callsign = computed(() => (store.callsign || '---').toUpperCase())
const aircraft = computed(() => (store.aircraft || '---').toUpperCase())
const realName = computed(() => (store.set.real_name || '---').toUpperCase())
</script>

<template>
  <div class="lcd">
    <div class="lcd-body lcd-screen-msg">
      <div v-if="msg" :class="['scr-msg', msg.isErr ? 'err' : '']">{{ msg.text }}</div>
      <div v-else-if="store.connected" class="scr-idle">
        <div class="idle-line">
          <span class="idle-label">{{ t('settings.lblCallsign') }}</span><b>{{ callsign }}</b>
        </div>
        <div class="idle-line">
          <span class="idle-label">{{ t('lcd.lblAircraft') }}</span><b>{{ aircraft }}</b>
        </div>
        <div class="idle-line">
          <span class="idle-label">{{ t('settings.lblRealName') }}</span><b>{{ realName }}</b>
        </div>
        <div class="idle-line">
          <span class="idle-label">{{ t('lcd.lblPilots') }}</span><b>{{ store.plrCount }}</b>
          <span class="idle-sep">·</span>
          <span class="idle-label">{{ t('lcd.lblAtc') }}</span><b>{{ store.atcCount }}</b>
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
    gap: 3px;
    font-family: var(--mono);
    font-size: 10px;
    line-height: 1.4;
    letter-spacing: 0.5px;
    color: var(--led-green-dim);
    text-align: left;
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

  .idle-sep {
    color: #2a7a4c;
  }

  .idle-clock {
    text-align: center;
    font-size: 12px;
    color: var(--led-green);
    text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
  }
</style>
