<script setup>
import { onBeforeUnmount, onMounted, ref } from 'vue'
import { useI18n } from 'vue-i18n'
import { store } from '../../../store.js'
import ListShell from './ListShell.vue'
import AtcRow from './AtcRow.vue'

const { t } = useI18n()
const list = ref([])
let timer = null

const COLS = 'minmax(0,1.5fr) minmax(0,0.9fr) minmax(0,1fr) minmax(0,0.9fr)'

async function refresh() {
  try {
    list.value = (await store.api.get_atc_list()) || []
  } catch (e) { /* 桥接瞬时错误忽略 */ }
}

onMounted(() => {
  refresh()
  timer = setInterval(refresh, 1500)
})
onBeforeUnmount(() => { if (timer) clearInterval(timer) })
</script>

<template>
  <ListShell :title="t('online.atcTitle')" :cols="COLS">
    <template #header>
      <span>{{ t('online.callsign') }}</span>
      <span>{{ t('online.facility') }}</span>
      <span>{{ t('online.frequency') }}</span>
      <span>{{ t('online.distance') }}</span>
    </template>
    <div v-if="!list.length" class="ol-empty">{{ t('online.empty') }}</div>
    <AtcRow v-for="a in list" :key="a.callsign" :item="a" />
  </ListShell>
</template>

<style scoped>
.ol-empty {
  font-size: 10px;
  color: var(--led-green-dim);
  padding: 6px 2px;
}
</style>
