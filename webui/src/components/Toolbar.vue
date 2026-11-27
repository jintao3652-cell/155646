<script setup>
import { useI18n } from 'vue-i18n'
import { store, doConnect, doDisconnect, onSim, onCallsignInput,
         fsdText, fsdTag, xpText, xpTag, mcText, mcTag, cslText, cslTag, cslTitle,
         regText, regSrcText } from '../store.js'
const { t } = useI18n()
</script>

<template>
  <div class="toolbar">
    <div v-show="!store.connected" class="fields">
      <el-input v-model="store.callsign" :placeholder="t('toolbar.callsign')" maxlength="12"
                size="small" class="f-1" @input="onCallsignInput" />
      <el-input v-model="store.aircraft" :placeholder="t('toolbar.aircraft')" size="small"
                class="f-2" readonly :title="t('toolbar.aircraftAutoDetected')" />
      <el-input v-model="store.airline" :placeholder="t('toolbar.airline')" maxlength="3"
                size="small" class="f-2" readonly :title="t('toolbar.airlineFromCallsign')" />
      <el-select v-model="store.sim" size="small" class="f-sim" @change="onSim">
        <el-option label="XP12" value="xp12" />
        <el-option label="XP11" value="xp11" />
      </el-select>
    </div>
    <el-button :type="store.connected ? 'danger' : 'primary'" :loading="store.busy"
               class="btn-connect"
               @click="store.connected ? doDisconnect() : doConnect()">
      <i class="fa-solid" :class="store.connected ? 'fa-link-slash' : 'fa-plane'"></i>
      {{ store.connected ? (store.busy ? t('toolbar.disconnecting') : t('toolbar.disconnect'))
                        : (store.busy ? t('toolbar.connecting') : t('toolbar.connect')) }}
    </el-button>
    <div class="tb-tags">
      <el-tooltip v-if="store.connected" placement="bottom"
                  :content="`${t('toolbar.registration')} · ${regSrcText || t('regSrc.none')}`">
        <el-tag size="small" type="info" effect="dark">{{ regText }}</el-tag>
      </el-tooltip>
      <el-tag size="small" :type="fsdTag" effect="dark">{{ fsdText }}</el-tag>
      <!-- XC 与 MC 是两个同级独立的桥分类：X-Plane 走 XC，MSFS 走 MC -->
      <el-tag size="small" :type="xpTag" effect="dark">{{ xpText }}</el-tag>
      <el-tag size="small" :type="mcTag" effect="dark">{{ mcText }}</el-tag>
      <el-tooltip :content="cslTitle" placement="bottom">
        <el-tag size="small" :type="cslTag" effect="dark">{{ cslText }}</el-tag>
      </el-tooltip>
    </div>
  </div>
</template>