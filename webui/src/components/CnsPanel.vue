<script setup>
  import { store, scrollLog, toggleDebug, openFlightPlan, openSettings, noop } from '../store.js'
  import { winMin, winClose } from '../store.js'
  import ScreenButton from './RMP/ScreenButton.vue'
  import Screen from './RMP/Screen.vue'
  import ScreenStatusLight from './RMP/ScreenStatusLight.vue'
  const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', 'CLR']
  const knobs = ['VOL', 'BRIGHT', 'FREQ', 'HDG/CRS']
</script>

<template>
  <section class="cns">
    <el-row>
      <el-col :span="24">
        <div class="rmp-status-light-group">
          <ScreenStatusLight text="VHF"></ScreenStatusLight>
          <ScreenStatusLight text="HF"></ScreenStatusLight>
          <ScreenStatusLight text="FPL" :onClick="openFlightPlan"></ScreenStatusLight>
          <ScreenStatusLight text="SET" :onClick="openSettings"></ScreenStatusLight>
          <ScreenStatusLight text=""></ScreenStatusLight>
          <ScreenStatusLight text="MIN" :onClick="winMin"></ScreenStatusLight>
          <ScreenStatusLight text="EXIT" :onClick="winClose"></ScreenStatusLight>
        </div>
      </el-col>
      <el-col :span="24">
        <el-row :gutter="10">
          <el-col :span="2" class="screen-left-button">
            <ScreenButton direction="right"></ScreenButton>
            <ScreenButton direction="right"></ScreenButton>
            <ScreenButton direction="right"></ScreenButton>
            <ScreenButton direction="right"></ScreenButton>
          </el-col>
          <el-col :span="20">
            <Screen></Screen>
          </el-col>
          <el-col :span="2" class="screen-right-button">
            <ScreenButton direction="left"></ScreenButton>
            <ScreenButton direction="left"></ScreenButton>
            <ScreenButton direction="left"></ScreenButton>
            <ScreenButton direction="left"></ScreenButton>
          </el-col>
        </el-row>
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

  .screen-left-button,
  .screen-right-button {
    display: flex;
    flex-direction: column;
    justify-content: space-between;
    align-items: center;
    min-height: 220px;
    padding: 6px 0;
  }

  .rmp-status-light-group {
    display: flex;
    justify-content: space-between;
    padding: 0 35px;
    margin-bottom: 5px;
  }
</style>
