<script setup>
import { useI18n } from 'vue-i18n'
import { store, winMin, winMax, winClose } from '../store.js'
const { t } = useI18n()
</script>

<template>
  <div class="titlebar pywebview-drag-region">
    <div class="brand">{{ t('app.brand') }}</div>
    <span class="title">{{ t('app.title') }}</span>
    <div class="win-btns" @mousedown.stop>
      <button class="wb" :title="t('titlebar.minimize')" @click="winMin">
        <i class="fa-solid fa-minus"></i>
      </button>
      <button class="wb" :title="t('titlebar.maximize')" @click="winMax">
        <i class="fa-regular fa-square"></i>
      </button>
      <button class="wb close" :title="t('titlebar.close')" @click="winClose">
        <i class="fa-solid fa-xmark"></i>
      </button>
    </div>
  </div>
</template>