<script setup>
import { distText, fmtDuration } from '../../../store.js'

const props = defineProps({
  item: { type: Object, required: true },
})
</script>

<template>
  <div class="row">
    <span class="c cs" :title="props.item.callsign">{{ props.item.callsign }}</span>
    <span class="c" :title="props.item.real_name">{{ props.item.real_name || '—' }}</span>
    <span class="c" :title="props.item.aircraft">{{ props.item.aircraft || '—' }}</span>
    <span class="c" :title="props.item.dep">{{ props.item.dep || '—' }}</span>
    <span class="c" :title="props.item.dest">{{ props.item.dest || '—' }}</span>
    <span class="c" :title="distText(props.item.latitude, props.item.longitude)">
      {{ distText(props.item.latitude, props.item.longitude) }}
    </span>
    <span class="c">{{ fmtDuration(props.item.online_seconds) }}</span>
  </div>
</template>

<style scoped>
.row {
  display: grid;
  grid-template-columns: var(--cols);
  align-items: center;
  font-size: 9px;
  color: var(--led-cyan);
  padding: 2px 0;
}

.c {
  min-width: 0;
  overflow: hidden;
  white-space: nowrap;
  text-overflow: ellipsis;
}

.cs {
  color: #fff;
  font-weight: bold;
}
</style>
