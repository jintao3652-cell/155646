<script setup>
import { useI18n } from 'vue-i18n'
import { store, saveSettings, exportDiagnostics } from '../store.js'
const { t } = useI18n()

const LANGS = [
  { value: 'en', label: t('settings.languages.en') },
  { value: 'zh-CN', label: t('settings.languages.zh-CN') },
  { value: 'zh-TW', label: t('settings.languages.zh-TW') },
]

/* 机型自动补全：调用后端机型库搜索，候选只含库内合法代码。
 * el-autocomplete 要求每项有 value 字段（选中后写入输入框）。
 * detail 用于下拉里展示「厂商 · 型号 · WTC」。 */
async function queryAircraft(q, cb) {
  const list = []
  if (q && q.trim() && store.api && store.api.search_aircraft) {
    try {
      const rows = await store.api.search_aircraft(q.trim(), 10) || []
      for (const r of rows) {
        list.push({
          value: r.designator,
          detail: `${r.manufacturer || ''} · ${r.model || ''} · WTC ${r.wtc || '-'}`,
        })
      }
    } catch (e) { /* 搜索失败不阻塞输入 */ }
  }
  cb(list)
}
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
        <label>{{ t('settings.aircraft') }}
          <el-autocomplete v-model="store.set.aircraft_manual" size="small"
                           class="ac-input" :fetch-suggestions="queryAircraft"
                           :placeholder="t('settings.aircraftPlaceholder')"
                           :trigger-on-focus="false" clearable>
            <template #default="{ item }">
              <div class="ac-item"><b>{{ item.value }}</b><span>{{ item.detail }}</span></div>
            </template>
          </el-autocomplete>
        </label>
        <p class="tip">{{ t('settings.aircraftTip') }}</p>
        <label>{{ t('settings.airline') }} <el-input v-model="store.set.airline" maxlength="3" size="small" /></label>
      </fieldset>
      <fieldset>
        <legend>{{ t('settings.server') }}</legend>
        <label>{{ t('settings.fsdServer') }} <el-input v-model="store.set.server_host" size="small" /></label>
        <label>{{ t('settings.fsdPort') }} <el-input v-model="store.set.server_port" size="small" /></label>
        <label>{{ t('settings.pluginAddr') }} <el-input v-model="store.set.xconnect_host" size="small" /></label>
        <label>{{ t('settings.pluginPort') }} <el-input v-model="store.set.xconnect_port" size="small" /></label>
        <el-checkbox v-model="store.set.debug">{{ t('settings.debugMode') }}</el-checkbox>
        <el-checkbox v-model="store.set.msfs_labels">{{ t('settings.msfsLabels') }}</el-checkbox>
        <label>{{ t('settings.msfsTrafficLib') }}
          <el-select v-model="store.set.traffic_lib" size="small">
            <el-option value="auto" :label="t('settings.trafficLibAuto')" />
            <el-option value="fsltl" :label="t('settings.trafficLibFsltl')" />
            <el-option value="aig" :label="t('settings.trafficLibAig')" />
          </el-select>
        </label>
        <p class="tip">{{ t('settings.pluginTip') }}</p>
        <p class="tip">{{ t('settings.msfsTrafficLibTip') }}</p>
        <p class="tip">{{ t('settings.msfsLabelsTip') }}</p>
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