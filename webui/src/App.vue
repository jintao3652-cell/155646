<script setup>
import { onMounted } from 'vue'
import { store, start } from './store.js'
import TitleBar from './components/TitleBar.vue'
import Toolbar from './components/Toolbar.vue'
import CnsPanel from './components/CnsPanel.vue'
import LogPane from './components/LogPane.vue'
import MsgDrawer from './components/MsgDrawer.vue'
import FlightPlanDialog from './components/FlightPlanDialog.vue'
import SettingsDialog from './components/SettingsDialog.vue'

onMounted(start)
</script>

<template>
  <div id="app" class="frameless-shell">
    <TitleBar />
    <Toolbar />

    <div class="workspace">
      <CnsPanel />
      <LogPane />
    </div>

    <MsgDrawer />
    <FlightPlanDialog />
    <SettingsDialog />
  </div>
</template>