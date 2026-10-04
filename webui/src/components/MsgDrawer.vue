<script setup>
import { store, sendMsg } from '../store.js'
</script>

<template>
  <el-drawer v-model="store.overlays.msg" title="Messages" size="360px"
             :append-to-body="true">
    <el-scrollbar class="msg-scroll">
      <div class="msg-list">
        <div v-for="(m, i) in store.msgs" :key="i" :class="['m', m.sent ? 'sent' : '']">
          <b>{{ m.from }}</b>  {{ m.text }}
        </div>
      </div>
    </el-scrollbar>
    <template #footer>
      <div class="msg-input-row">
        <el-input v-model="store.msgTo" placeholder="呼号 或 @频率" size="small"
                  class="msg-to" />
        <el-input v-model="store.msgText" placeholder="Message..." size="small"
                  @keydown.enter="sendMsg" />
        <el-button type="primary" size="small" @click="sendMsg">Send</el-button>
      </div>
    </template>
  </el-drawer>
</template>