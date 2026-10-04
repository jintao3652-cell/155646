<script setup>
  import { store, scrollLog, toggleDebug, openFlightPlan, openSettings, noop, doConnect, doDisconnect, fsdText, fsdTag, xpText, xpTag, cslText, cslTag, cslTitle } from '../store.js'
  import { winMin, winClose } from '../store.js'
  import router from '@/router'
  import ScreenButton from './RMP/ScreenButton.vue'
  import ScreenStatusLight from './RMP/ScreenStatusLight.vue'
  const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', 'CLR']
  const knobs = ['VOL', 'BRIGHT', 'FREQ', 'HDG/CRS']
  const toggleFsdConnection = () => store.connected ? doDisconnect() : doConnect()
</script>

<template>
  <section class="cns pywebview-drag-region">
    <el-row>
      <el-col :span="24">
        <div class="rmp-status-light-group">
          <ScreenStatusLight text="FSD" :status="fsdTag" :title="fsdText" :onClick="toggleFsdConnection">
          </ScreenStatusLight>
          <ScreenStatusLight text="XP" :status="xpTag" :title="xpText"></ScreenStatusLight>
          <ScreenStatusLight text="CSL" :status="cslTag" :title="cslTitle" :onClick="openSettings">
          </ScreenStatusLight>
          <ScreenStatusLight text="FPL" status="success" :onClick="() => router.push('/settings')"></ScreenStatusLight>
          <ScreenStatusLight text="SET" status="success" :onClick="() => router.push('/settings')"></ScreenStatusLight>
          <ScreenStatusLight text="MIN" status="info" :onClick="winMin"></ScreenStatusLight>
          <ScreenStatusLight text="EXIT" status="danger" :onClick="winClose"></ScreenStatusLight>
        </div>
      </el-col>
      <el-col :span="24">
        <router-view v-slot="{ Component }" name="screen">
          <component :is="Component" />
        </router-view>
      </el-col>
      <el-col :span="2"></el-col>
      <el-col :span="20" style="margin-top: 5px">
        <el-row>
          <el-col :span="4"></el-col>
          <el-col :span="8" style="display: flex; justify-content: space-around;">
            <ScreenButton direction="up"></ScreenButton>
            <ScreenButton direction="up"></ScreenButton>
          </el-col>
          <el-col :span="4"></el-col>
          <el-col :span="8"></el-col>
        </el-row>
      </el-col>
      <el-col :span="2"></el-col>
    </el-row>
    <div class="cns-brand"><b>XFLY</b>SIM<span class="ver">CNS</span></div>

    <!-- LCD + 环形导航键 -->
    <div class="lcd-zone">


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

<style scoped>
  .rmp-status-light-group {
    display: flex;
    justify-content: space-between;
    padding: 0 35px;
    margin-bottom: 5px;
  }
</style>
