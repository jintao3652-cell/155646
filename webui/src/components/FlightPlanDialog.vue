<script setup>
import { useI18n } from 'vue-i18n'
import { store, submitFlightPlan } from '../store.js'
const { t } = useI18n()
</script>

<template>
  <el-drawer v-model="store.overlays.fp" :title="t('flightPlan.title')" class="fp-drawer"
             direction="btt" size="100%" :append-to-body="true">
    <el-scrollbar class="fp-scroll">
      <div class="fp-content">
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