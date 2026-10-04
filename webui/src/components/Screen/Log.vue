<script setup>
  import { nextTick, watch } from 'vue'
  import { store } from "../../store.js";
  import router from '@/router'

  import ScreenButton from '@/components/RMP/ScreenButton.vue'
  // 日志追加时滚动到底部
  watch(() => store.log.length, async () => {
    await nextTick()
    const bar = document.querySelector('.log-pane .el-scrollbar__wrap')
    if (bar) bar.scrollTop = bar.scrollHeight
  })
</script>

<template>
  <el-row :gutter="10">
    <el-col :span="2" class="screen-left-button">
      <ScreenButton direction="right"></ScreenButton>
      <ScreenButton direction="right"></ScreenButton>
      <ScreenButton direction="right"></ScreenButton>
      <ScreenButton direction="right"></ScreenButton>
    </el-col>
    <el-col :span="20">
      <div class="lcd">
        <el-scrollbar class="lcd-body">
          <div class="log-inner">
            <div v-for="item in store.log" :key="item.id" class="log-line" :class="item.cls">
              <span class="t">{{ item.t }}</span>{{ item.m }}
            </div>
          </div>
        </el-scrollbar>
      </div>
    </el-col>
    <el-col :span="2" class="screen-right-button">
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left" :onClick="() => router.push('/log')"></ScreenButton>
    </el-col>
  </el-row>
</template>


<style scoped>

  /* ---------- 日志 ---------- */


  .log-inner {
    padding: 0px 0px;
  }

  .log-line {
    font-family: var(--mono);
    font-size: 9px;
    line-height: 1.8;
    color: var(--dim);
    white-space: pre-wrap;
    word-break: break-all;
  }

  .log-line .t {
    color: #3c444c;
    margin-right: 8px;
  }

  .log-line.in {
    color: #7fd6a6;
  }

  .log-line.out {
    color: #b9a88e;
  }

  .log-line.err {
    color: var(--bad);
  }

  .log-line.txt {
    color: #6fd38a;
  }

  /* Element Plus 滚动条 */
  .el-scrollbar__bar.is-vertical {
    width: 8px;
    right: 2px;
  }

  .el-scrollbar__bar.is-horizontal {
    height: 8px;
    bottom: 2px;
  }

  .el-scrollbar__thumb {
    background: #3d444d;
    opacity: .8;
    border-radius: 4px;
  }

  .el-scrollbar__thumb:hover {
    background: #4c545f;
  }

  /* ---------- 消息 ---------- */
  .msg-scroll {
    height: 100%;
  }

  .msg-list {
    padding: 0 4px;
    user-select: text;
    font-size: 12.5px;
    line-height: 1.8;
  }

  .msg-list .m b {
    color: var(--accent);
  }

  .msg-list .m.sent b {
    color: var(--warn);
  }

  .msg-input-row {
    display: flex;
    gap: 8px;
  }

  .msg-input-row .msg-to {
    width: 150px;
    flex: 0 0 auto;
  }

  .msg-input-row .el-input {
    flex: 1 1 auto;
  }

  .el-drawer {
    background: var(--panel-bg);
  }

</style>