<script setup>
  import { store, scrollLog, toggleDebug, openFlightPlan, openSettings, noop } from '../store.js'
  import ScreenButton from './RMP/ScreenButton.vue'
  import Screen from './RMP/Screen.vue'
  const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', 'CLR']
  const knobs = ['VOL', 'BRIGHT', 'FREQ', 'HDG/CRS']
</script>

<template>
  <section class="cns">
    <el-row>
      <el-col :span="24"></el-col>
      <el-col :span="24">
        <el-row>
          <el-col :span="2" class="screen-left-button">
            <el-row>
              <el-col :span="24">
                <ScreenButton direction="right" class="screen-left"></ScreenButton>
              </el-col>
              <el-col :span="24">
                <ScreenButton direction="right"></ScreenButton>
              </el-col>
              <el-col :span="24">
                <ScreenButton direction="right"></ScreenButton>
              </el-col>
              <el-col :span="24">
                <ScreenButton direction="right"></ScreenButton>
              </el-col>
            </el-row>
          </el-col>
          <el-col :span="20">
            <Screen></Screen>
          </el-col>
          <el-col :span="2" class="screen-right-button">
            <el-row>
              <el-col :span="24">
                <ScreenButton direction="left"></ScreenButton>
              </el-col>
              <el-col :span="24">
                <ScreenButton direction="left"></ScreenButton>
              </el-col>
              <el-col :span="24">
                <ScreenButton direction="left"></ScreenButton>
              </el-col>
              <el-col :span="24">
                <ScreenButton direction="left"></ScreenButton>
              </el-col>
            </el-row>
          </el-col>
        </el-row>
      </el-col>
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
  .screen-left-button .el-row,
  .screen-right-button .el-row {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 35px;
    padding-top: 6px;
    padding-bottom: 6px;
  }
</style>
