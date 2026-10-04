<script setup>
import { useI18n } from 'vue-i18n'
import { store, sendMsg } from '../store.js'
const { t } = useI18n()
</script>

<template>
  <el-drawer v-model="store.overlays.msg" :title="t('msg.title')" size="360px"
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
        <el-input v-model="store.msgTo" :placeholder="t('msg.toPlaceholder')" size="small"
                  class="msg-to" />
        <el-input v-model="store.msgText" :placeholder="t('msg.textPlaceholder')" size="small"
                  @keydown.enter="sendMsg" />
        <el-button type="primary" size="small" @click="sendMsg">{{ t('msg.send') }}</el-button>
      </div>
    </template>
  </el-drawer>
</template>