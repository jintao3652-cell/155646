<script setup>
import { store, scrollLog, toggleDebug, openFlightPlan, openSettings, noop } from '../store.js'

const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', 'CLR']
const knobs = ['VOL', 'BRIGHT', 'FREQ', 'HDG/CRS']
</script>

<template>
  <section class="cns">
    <div class="cns-brand"><b>XFLY</b>SIM<span class="ver">CNS</span></div>

    <!-- LCD + 环形导航键 -->
    <div class="lcd-zone">
      <div class="ring ring-left">
        <button class="rbtn" title="上" @click="scrollLog(-1)"><i class="fa-solid fa-chevron-up"></i></button>
        <button class="rbtn" title="左" @click="noop"><i class="fa-solid fa-chevron-left"></i></button>
        <button class="rbtn" title="右" @click="noop"><i class="fa-solid fa-chevron-right"></i></button>
        <button class="rbtn" title="下" @click="scrollLog(1)"><i class="fa-solid fa-chevron-down"></i></button>
      </div>

      <div class="lcd">
        <div class="lcd-head">
          <span>FREQ CONTROL</span>
          <span class="lcd-sub">XFLYSIM · {{ store.callsign || 'NO CALLSIGN' }}</span>
        </div>
        <div class="lcd-body">
          <div class="lcd-row">
            <span class="k">COM1</span>
            <span class="v big">{{ store.st.com1 }}</span>
            <span class="standby">STBY</span>
            <span class="v">{{ store.st.com2 }}</span>
            <span class="k dimk">VHF1</span>
          </div>
          <div class="lcd-row">
            <span class="k">COM2</span>
            <span class="v big">{{ store.st.com2 }}</span>
            <span class="k dimk">XPDR</span>
            <span class="v">{{ store.st.xpdr }}</span>
            <span class="k dimk">{{ store.st.gs }}</span>
          </div>
          <div class="lcd-row alt-row">
            <span class="k">ALT</span>
            <span class="v">{{ store.st.alt }}</span>
            <span class="k dimk">FLT</span>
            <span class="v lcd-sm">{{ store.fsd.text }} · {{ store.xplane.text }}</span>
            <span class="k dimk">CSL</span>
            <span class="v lcd-sm">{{ store.csl.text }}</span>
          </div>
        </div>
      </div>

      <div class="ring ring-right">
        <button class="rbtn" title="上" @click="scrollLog(-1)"><i class="fa-solid fa-chevron-up"></i></button>
        <button class="rbtn" title="左" @click="noop"><i class="fa-solid fa-chevron-left"></i></button>
        <button class="rbtn" title="右" @click="noop"><i class="fa-solid fa-chevron-right"></i></button>
        <button class="rbtn" title="下" @click="scrollLog(1)"><i class="fa-solid fa-chevron-down"></i></button>
      </div>
    </div>

    <!-- 功能键行 -->
    <div class="fn-row">
      <button class="fn" @click="store.overlays.msg = true">MSG</button>
      <button class="fn" @click="openFlightPlan">FLT PLAN</button>
      <button class="fn" @click="openSettings">SET</button>
      <button class="fn" @click="toggleDebug">DEBUG</button>
      <button class="fn" @click="noop">NAV</button>
      <button class="fn" @click="noop">MENU</button>
    </div>

    <!-- 数字键盘 -->
    <div class="keypad">
      <button v-for="n in keys" :key="n" class="key" @click="noop">{{ n }}</button>
    </div>

    <!-- 旋钮行 -->
    <div class="knob-row">
      <div v-for="k in knobs" :key="k" class="knob-item">
        <div class="knob"><span class="tick"></span></div>
        <div class="knob-label">{{ k }}</div>
      </div>
    </div>
  </section>
</template>