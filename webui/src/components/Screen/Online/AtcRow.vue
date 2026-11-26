<script setup>
import { distText } from '../../../store.js'

const props = defineProps({
  item: { type: Object, required: true },
})
</script>

<template>
  <div class="row">
    <span class="c cs" :title="props.item.callsign">{{ props.item.callsign }}</span>
    <span class="c" :title="props.item.type">{{ props.item.type }}</span>
    <span class="c" :title="props.item.frequency">{{ props.item.frequency }}</span>
    <span class="c" :title="distText(props.item.latitude, props.item.longitude)">
      {{ distText(props.item.latitude, props.item.longitude) }}
    </span>
  </div>
</template>

<style scoped>
.row {
  display: grid;
  grid-template-columns: var(--cols);
  align-items: center;
  font-size: 10px;
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
