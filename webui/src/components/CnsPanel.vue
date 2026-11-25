<script setup>
  import { computed } from 'vue'
  import { useRoute } from 'vue-router'
  import { useI18n } from 'vue-i18n'
  import { store, openFlightPlan, openSettings, noop, doConnect, doDisconnect, cslTag } from '../store.js'
  import { winMin, winClose } from '../store.js'
  import router from '@/router'
  import ScreenButton from './RMP/ScreenButton.vue'
  import ScreenStatusLight from './RMP/ScreenStatusLight.vue'
  import ScreenKnobButton from './RMP/ScreenKnobButton.vue'
  import ScreenStatusButton from './RMP/ScreenStatusButton.vue'
  import ScreenPageButton from './RMP/ScreenPageButton.vue'
  import Screen from './RMP/Screen.vue'
  const { t } = useI18n()
  const keys = ['1', '2', '3', '4', '5', '6', '7', '8', '9', '.', '0', t('cns.clr')]
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
  /* VHF 灯：至少一个 COM 通电且在收听才亮，否则降级为 info（灰） */
  const vhfOn = computed(() =>
    (store.st.com1Pwr && store.st.audSel1) ||
    (store.st.com2Pwr && store.st.audSel2))
  const vhfTag = computed(() => vhfOn.value ? 'success' : 'info')
</script>

<template>
  <section class="cns pywebview-drag-region">
    <el-row>
      <el-col :span="24">
        <div class="rmp-status-light-group">
          <ScreenStatusLight :text="t('cns.vhf')" :status="vhfTag"></ScreenStatusLight>
          <ScreenStatusLight :text="t('cns.hf')" status="success"></ScreenStatusLight>
          <ScreenStatusLight :text="t('cns.tel')" status="success"></ScreenStatusLight>
          <ScreenStatusLight :text="t('cns.atc')" status="success"></ScreenStatusLight>
          <ScreenStatusLight text="" status="info"></ScreenStatusLight>
          <ScreenStatusLight :text="t('cns.min')" status="info" :onClick="winMin"></ScreenStatusLight>
          <ScreenStatusLight :text="t('cns.exit')" status="danger" :onClick="winClose"></ScreenStatusLight>
        </div>
      </el-col>
      <el-col :span="24">
        <router-view v-slot="{ Component }" name="screen">
          <component :is="Component" />
        </router-view>
      </el-col>
      <el-col :span="22" style="padding: 5px 0px">
        <el-row>
          <el-col :span="14">
            <div style="display: flex; flex-direction: column; height: 100%;">
              <el-row>
                <el-col :span="4">
                  <ScreenKnobButton></ScreenKnobButton>
                </el-col>
                <el-col :span="6"></el-col>
                <el-col :span="14" style="display: flex; justify-content: space-around;">
                  <ScreenButton direction="up"></ScreenButton>
                  <ScreenButton direction="up"></ScreenButton>
                </el-col>
              </el-row>
              <Screen style="margin-top: 5px;"></Screen>
            </div>
          </el-col>

          <el-col :span="10">
            <div class="keypad">
              <button v-for="n in keys" :key="n" class="key" @click="noop">{{ n }}</button>
            </div>
          </el-col>
        </el-row>
      </el-col>
      <el-col :span="2">
        <div class="page-button-group">
          <ScreenPageButton direction="up"></ScreenPageButton>
          <ScreenPageButton direction="down"></ScreenPageButton>
        </div>
      </el-col>
      <el-col :span="24">
        <div class="fn-row">
          <div class="fn-item">
            <ScreenStatusButton :text="t('cns.fsd')" :active="fsdActive" :onClick="toggleFsdConnection"></ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton :text="t('cns.xpl')" :active="xplActive"></ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton :text="t('cns.csl')" :active="cslActive" :danger="cslDanger" :onClick="openSettings">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton :text="t('cns.fpl')" :active="fplActive" :onClick="openFlightPlan"></ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>

          <div class="fn-item">
            <ScreenStatusButton :text="t('cns.main')" :active="mainActive" :onClick="() => router.push('/main')">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton :text="t('cns.msg')" :active="msgActive" :onClick="() => router.push('/log')">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
          <div class="fn-item">
            <ScreenStatusButton :text="t('cns.set')" :active="setActive" :onClick="() => router.push('/settings')">
            </ScreenStatusButton>
            <ScreenKnobButton></ScreenKnobButton>
          </div>
        </div>
      </el-col>
    </el-row>
    <div class="cns-brand"><b>{{ t('cns.brand') }}</b>{{ t('cns.brandSim') }}<span class="ver">{{ t('cns.version') }}</span></div>
  </section>

</template>

<style scoped>
  .rmp-status-light-group {
    display: flex;
    justify-content: space-between;
    padding: 0 35px;
    margin-bottom: 5px;
  }

  .page-button-group {
    display: flex;
    flex-direction: column;
    height: 100%;
    padding: 5px 0;
    gap: 10px;
  }
</style>
