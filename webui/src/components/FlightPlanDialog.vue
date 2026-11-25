<script setup>
import { ref, nextTick } from 'vue'
import { useI18n } from 'vue-i18n'
import { store, submitFlightPlan, importSimbrief } from '../store.js'
const { t } = useI18n()

const scrollRef = ref(null)

/* 打开时把表单滚回顶部：抽屉内容比窗口高，聚焦/焦点陷阱可能留下上次的滚动位置，
 * 表现为"一打开就在表单中段"。 */
async function onOpen() {
  await nextTick()
  scrollRef.value?.setScrollTop?.(0)
  setTimeout(() => scrollRef.value?.setScrollTop?.(0), 120)   // 首开时子节点延后挂载
}
</script>

<template>
  <el-drawer v-model="store.overlays.fp" :title="t('flightPlan.title')" class="fp-drawer"
             modal-class="fp-overlay" :lock-scroll="false" @open="onOpen"
             direction="btt" size="100%" :append-to-body="true">
    <el-scrollbar ref="scrollRef" class="fp-scroll">
      <div class="fp-content">
        <div class="fp-sb">
          <span class="fp-sb-title">
            <i class="fa-solid fa-cloud-arrow-down"></i> {{ t('flightPlan.sbLabel') }}
          </span>
          <el-input v-model="store.sbUser" size="small" clearable
                    :placeholder="t('flightPlan.sbPlaceholder')"
                    @input="store.sbError = ''"
                    @keyup.enter="importSimbrief" />
          <el-button size="small" type="primary" plain :loading="store.sbBusy"
                     @click="importSimbrief">{{ t('flightPlan.sbImport') }}</el-button>
        </div>
        <div class="fp-sb-tip" :class="{ 'is-err': store.sbError, 'is-busy': store.sbBusy }">
          {{ store.sbBusy ? t('flightPlan.sbImporting')
                          : (store.sbError || store.sbInfo || t('flightPlan.sbHint')) }}
        </div>
        <div class="fp-grid">
          <fieldset>
            <legend>{{ t('flightPlan.basic') }}</legend>
            <label>{{ t('flightPlan.rule') }}
              <el-select v-model="store.fp.type" size="small">
            <el-option :label="t('flightPlan.rules.I')" value="I" />
            <el-option :label="t('flightPlan.rules.V')" value="V" />
            <el-option :label="t('flightPlan.rules.S')" value="S" />
            <el-option :label="t('flightPlan.rules.D')" value="D" />
              </el-select>
            </label>
            <label>{{ t('flightPlan.aircraft') }} <el-input v-model="store.fp.aircraft" size="small" /></label>
            <label :title="t('flightPlan.regHint')">{{ t('flightPlan.registration') }}
              <el-input v-model="store.fp.registration" size="small"
                        :placeholder="store.reg ? t('flightPlan.regAuto', { reg: store.reg })
                                                : t('flightPlan.regPlaceholder')" />
            </label>
            <label>{{ t('flightPlan.cruiseSpeed') }} <el-input v-model="store.fp.speed" placeholder="450" size="small" /></label>
            <label>{{ t('flightPlan.cruiseAlt') }} <el-input v-model="store.fp.alt" placeholder="FL350" size="small" /></label>
          </fieldset>
          <fieldset>
            <legend>{{ t('flightPlan.route') }}</legend>
            <label>{{ t('flightPlan.dep') }} <el-input v-model="store.fp.dep" placeholder="ZGGG" size="small" /></label>
            <label>{{ t('flightPlan.dest') }} <el-input v-model="store.fp.dest" placeholder="ZBAD" size="small" /></label>
            <label>{{ t('flightPlan.altApt') }} <el-input v-model="store.fp.altapt" placeholder="ZSNJ" size="small" /></label>
            <label>{{ t('flightPlan.planTime') }} <el-input v-model="store.fp.plan_time" placeholder="1200" size="small" /></label>
            <label>{{ t('flightPlan.actualTime') }} <el-input v-model="store.fp.actual" placeholder="1200" size="small" /></label>
          </fieldset>
          <fieldset>
            <legend>{{ t('flightPlan.enrouteFuel') }}</legend>
            <label>{{ t('flightPlan.enrouteTime') }}
              <span class="hhmm">
                <el-input-number v-model="store.fp.enrh" :min="0" :max="23"
                                 size="small" controls-position="right" />
                <el-input-number v-model="store.fp.enrm" :min="0" :max="59"
                                 size="small" controls-position="right" />
              </span>
            </label>
            <label>{{ t('flightPlan.fuelTime') }}
              <span class="hhmm">
                <el-input-number v-model="store.fp.fuelh" :min="0" :max="23"
                                 size="small" controls-position="right" />
                <el-input-number v-model="store.fp.fuelm" :min="0" :max="59"
                                 size="small" controls-position="right" />
              </span>
            </label>
            <label>{{ t('flightPlan.remarks') }} <el-input v-model="store.fp.remarks" placeholder="RMK" size="small" /></label>
          </fieldset>
        </div>
        <div class="fp-route">
          <fieldset>
            <legend>{{ t('flightPlan.routeField') }}</legend>
            <label>{{ t('flightPlan.routeField') }} <el-input v-model="store.fp.route"
                      placeholder="DCT SHL W22 TYN B208" size="small" /></label>
          </fieldset>
        </div>
      </div>
    </el-scrollbar>
    <template #footer>
      <el-button @click="store.overlays.fp = false">{{ t('settings.cancel') }}</el-button>
      <el-button type="primary" @click="submitFlightPlan">
        <i class="fa-solid fa-paper-plane"></i> {{ t('flightPlan.submit') }}
      </el-button>
    </template>
  </el-drawer>
</template>