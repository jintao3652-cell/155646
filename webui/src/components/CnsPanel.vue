<script setup>
  import { computed } from 'vue'
  import { useRoute } from 'vue-router'
  import { store, openFlightPlan, openSettings, noop, doConnect, doDisconnect, cslTag } from '../store.js'
  import { winMin, winClose } from '../store.js'
  import router from '@/router'
  import ScreenButton from './RMP/ScreenButton.vue'
  import ScreenStatusLight from './RMP/ScreenStatusLight.vue'
  import ScreenKnobButton from './RMP/ScreenKnobButton.vue'
  import ScreenStatusButton from './RMP/ScreenStatusButton.vue'
  const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', 'CLR']
  const knobs = ['VOL', 'BRIGHT', 'FREQ', 'HDG/CRS']
  const toggleFsdConnection = () => store.connected ? doDisconnect() : doConnect()
  const route = useRoute()

  /* 下方功能键指示灯 */
  const fsdActive = computed(() => store.connected)
  const xplActive = computed(() => store.xpState === 'connected')
  const cslActive = computed(() => cslTag.value === 'success')
  const cslDanger = computed(() => cslTag.value === 'danger')
  const fplActive = computed(() => !!(store.fp.dep && store.fp.dest))
  const msgActive = computed(() => route.path === '/log')
  const setActive = computed(() => route.path === '/settings')
  const mainActive = computed(() => route.path === '/main')
</script>

<template>
  <section class="cns pywebview-drag-region">
    <el-row>
      <el-col :span="24">
        <div class="rmp-status-light-group">
          <ScreenStatusLight text="VHF" status="success"></ScreenStatusLight>
          <ScreenStatusLight text="HF" status="success" ></ScreenStatusLight>
          <ScreenStatusLight text="TEL" status="success" ></ScreenStatusLight>
          <ScreenStatusLight text="ATC" status="success"></ScreenStatusLight>
          <ScreenStatusLight text="" status="info"></ScreenStatusLight>
          <ScreenStatusLight text="MIN" status="info" :onClick="winMin"></ScreenStatusLight>
          <ScreenStatusLight text="EXIT" status="danger" :onClick="winClose"></ScreenStatusLight>
        </div>
      </el-col>
      <el-col :span="24">
        <router-view v-slot="{ Component }" name="screen">
          <component :is="Component" />
        </router-view>
      </el-col>
      <el-col :span="2">
        <ScreenKnobButton></ScreenKnobButton>
      </el-col>
      <el-col :span="20" style="margin-top: 5px">
        <el-row>
          <el-col :span="4"></el-col>
          <el-col :span="8" style="display: flex; justify-content: space-around;">
            <ScreenButton direction="up"></ScreenButton>
            <ScreenButton direction="up"></ScreenButton>
          </el-col>
          <el-col :span="12">
            <div class="keypad">
              <button v-for="n in keys" :key="n" class="key" @click="noop">{{ n }}</button>
            </div>
          </el-col>
        </el-row>
      </el-col>
      <el-col :span="2"></el-col>
      <el-col :span="24">
        <div class="fn-row">
          <div class="fn-item">
            <ScreenStatusButton text="FSD" :active="fsdActive" :onClick="toggleFsdConnection"></ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton text="XPL" :active="xplActive"></ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton text="CSL" :active="cslActive" :danger="cslDanger" :onClick="openSettings">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton text="FPL" :active="fplActive" :onClick="openFlightPlan"></ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>

          <div class="fn-item">
            <ScreenStatusButton text="MAIN" :active="mainActive" :onClick="() => router.push('/main')">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton text="MSG" :active="msgActive" :onClick="() => router.push('/log')">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton text="SET" :active="setActive" :onClick="() => router.push('/settings')">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
        </div>
      </el-col>
    </el-row>
    <div class="cns-brand"><b>XFLY</b>SIM<span class="ver">v1.0.0</span></div>
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
