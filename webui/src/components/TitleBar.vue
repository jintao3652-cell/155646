<script setup>
import { store, winMin, winMax, winClose } from '../store.js'
</script>

<template>
  <div class="titlebar pywebview-drag-region">
    <div class="brand">XF</div>
    <span class="title">XShare - XFlySim Client</span>
    <div class="win-btns" @mousedown.stop>
      <button class="wb" title="最小化" @click="winMin">
        <i class="fa-solid fa-minus"></i>
      </button>
      <button class="wb" title="最大化/还原" @click="winMax">
        <i class="fa-regular fa-square"></i>
      </button>
      <button class="wb close" title="关闭" @click="winClose">
        <i class="fa-solid fa-xmark"></i>
      </button>
    </div>
  </div>
</template>