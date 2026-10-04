<script setup>
  import { nextTick, watch } from 'vue'
    import { store } from "../../store.js";


  // 日志追加时滚动到底部
  watch(() => store.log.length, async () => {
    await nextTick()
    const bar = document.querySelector('.log-pane .el-scrollbar__wrap')
    if (bar) bar.scrollTop = bar.scrollHeight
  })
</script>

<template>
  <el-scrollbar class="log-pane">
    <div class="log-inner">
      <div v-for="item in store.log" :key="item.id" class="log-line" :class="item.cls">
        <span class="t">{{ item.t }}</span>{{ item.m }}
      </div>
    </div>
  </el-scrollbar>
</template>