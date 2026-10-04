<script setup>
import { store, doConnect, doDisconnect, onSim } from '../store.js'
</script>

<template>
  <div class="toolbar">
    <div v-show="!store.connected" class="fields">
      <el-input v-model="store.callsign" placeholder="CALLSIGN" maxlength="12"
                size="small" class="f-1" />
      <el-input v-model="store.aircraft" placeholder="机型自动识别" size="small"
                class="f-2" readonly title="由模拟器自动识别" />
      <el-input v-model="store.airline" placeholder="AIRLINE" maxlength="3"
                size="small" class="f-2" />
      <el-select v-model="store.sim" size="small" class="f-sim" @change="onSim">
        <el-option label="XP12" value="xp12" />
        <el-option label="XP11" value="xp11" />
      </el-select>
    </div>
    <el-button :type="store.connected ? 'danger' : 'primary'" :loading="store.busy"
               class="btn-connect"
               @click="store.connected ? doDisconnect() : doConnect()">
      <i class="fa-solid" :class="store.connected ? 'fa-link-slash' : 'fa-plane'"></i>
      {{ store.connected ? (store.busy ? '断开中' : 'Disconnect')
                        : (store.busy ? '连线中' : 'Connect') }}
    </el-button>
    <div class="tb-tags">
      <el-tag size="small" :type="store.fsd.tag" effect="dark">{{ store.fsd.text }}</el-tag>
      <el-tag size="small" :type="store.xplane.tag" effect="dark">{{ store.xplane.text }}</el-tag>
      <el-tooltip :content="store.csl.title" placement="bottom">
        <el-tag size="small" :type="store.csl.tag" effect="dark">{{ store.csl.text }}</el-tag>
      </el-tooltip>
    </div>
  </div>
</template>