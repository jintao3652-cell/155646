<script setup>
import { useI18n } from 'vue-i18n'
import { store, saveSettings, exportDiagnostics } from '../store.js'
const { t } = useI18n()

const LANGS = [
  { value: 'en', label: t('settings.languages.en') },
  { value: 'zh-CN', label: t('settings.languages.zh-CN') },
  { value: 'zh-TW', label: t('settings.languages.zh-TW') },
]
</script>

<template>
  <el-dialog v-model="store.overlays.set" :title="t('settings.title')"
             width="min(760px, 94vw)" :append-to-body="true">
    <div class="fp-grid">
      <fieldset>
        <legend>{{ t('settings.account') }}</legend>
        <label>{{ t('settings.realName') }} <el-input v-model="store.set.real_name" size="small" /></label>
        <label>{{ t('settings.cid') }} <el-input v-model="store.set.cid" size="small" /></label>
        <label>{{ t('settings.password') }} <el-input v-model="store.set.password" type="password"
                             show-password size="small" /></label>
        <el-checkbox v-model="store.set.remember">{{ t('settings.rememberPassword') }}</el-checkbox>
      </fieldset>
      <fieldset>
        <legend>{{ t('settings.flightInfo') }}</legend>
        <label>{{ t('settings.callsign') }} <el-input v-model="store.set.callsign" size="small" /></label>
        <label>{{ t('settings.aircraft') }}</label>
        <label>{{ t('settings.airline') }} <el-input v-model="store.set.airline" maxlength="3" size="small" /></label>
      </fieldset>
      <fieldset>
        <legend>{{ t('settings.server') }}</legend>
        <label>{{ t('settings.fsdServer') }} <el-input v-model="store.set.host" size="small" /></label>
        <label>{{ t('settings.fsdPort') }} <el-input v-model="store.set.port" size="small" /></label>
        <label>{{ t('settings.pluginAddr') }} <el-input v-model="store.set.xphost" size="small" /></label>
        <label>{{ t('settings.pluginPort') }} <el-input v-model="store.set.xpport" size="small" /></label>
        <el-checkbox v-model="store.set.debug">{{ t('settings.debugMode') }}</el-checkbox>
        <p class="tip">{{ t('settings.pluginTip') }}</p>
      </fieldset>
      <fieldset>
        <legend>{{ t('settings.language') }}</legend>
        <label>{{ t('settings.language') }}
          <el-select v-model="store.set.language" size="small">
            <el-option v-for="l in LANGS" :key="l.value" :label="l.label" :value="l.value" />
          </el-select>
        </label>
        <p class="tip">{{ t('settings.languageTip') }}</p>
      </fieldset>
    </div>
    <template #footer>
      <el-button @click="exportDiagnostics">
        <i class="fa-solid fa-file-arrow-down"></i>
        {{ t('settings.exportDiagnostics') }}
      </el-button>
      <el-button @click="store.overlays.set = false">{{ t('settings.cancel') }}</el-button>
      <el-button type="primary" @click="saveSettings">
        <i class="fa-solid fa-check"></i> {{ t('settings.save') }}
      </el-button>
    </template>
  </el-dialog>
</template>