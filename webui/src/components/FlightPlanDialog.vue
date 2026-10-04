<script setup>
import { store, submitFlightPlan } from '../store.js'
</script>

<template>
  <el-dialog v-model="store.overlays.fp" title="飞行计划"
             width="min(720px, 94vw)" :append-to-body="true">
    <div class="fp-grid">
      <fieldset>
        <legend>基本信息</legend>
        <label>飞行规则
          <el-select v-model="store.fp.type" size="small">
            <el-option label="IFR" value="I" />
            <el-option label="VFR" value="V" />
            <el-option label="SVFR" value="S" />
            <el-option label="DVFR" value="D" />
          </el-select>
        </label>
        <label>机型 <el-input v-model="store.fp.aircraft" size="small" /></label>
        <label>巡航速度 <el-input v-model="store.fp.speed" placeholder="450" size="small" /></label>
        <label>巡航高度 <el-input v-model="store.fp.alt" placeholder="FL350" size="small" /></label>
      </fieldset>
      <fieldset>
        <legend>航段</legend>
        <label>起飞机场 <el-input v-model="store.fp.dep" placeholder="ZGGG" size="small" /></label>
        <label>目的地 <el-input v-model="store.fp.dest" placeholder="ZBAD" size="small" /></label>
        <label>备降场 <el-input v-model="store.fp.altapt" placeholder="ZSNJ" size="small" /></label>
        <label>计划起飞时间 <el-input v-model="store.fp.plan_time" placeholder="1200" size="small" /></label>
        <label>实际起飞时间 <el-input v-model="store.fp.actual" placeholder="1200" size="small" /></label>
      </fieldset>
      <fieldset>
        <legend>航程与燃油</legend>
        <label>航程(时/分)
          <span class="hhmm">
            <el-input-number v-model="store.fp.enrh" :min="0" :max="23"
                             size="small" controls-position="right" />
            <el-input-number v-model="store.fp.enrm" :min="0" :max="59"
                             size="small" controls-position="right" />
          </span>
        </label>
        <label>燃油(时/分)
          <span class="hhmm">
            <el-input-number v-model="store.fp.fuelh" :min="0" :max="23"
                             size="small" controls-position="right" />
            <el-input-number v-model="store.fp.fuelm" :min="0" :max="59"
                             size="small" controls-position="right" />
          </span>
        </label>
        <label>备注 <el-input v-model="store.fp.remarks" placeholder="RMK" size="small" /></label>
      </fieldset>
    </div>
    <div class="fp-route">
      <fieldset>
        <legend>航路</legend>
        <label>航路 <el-input v-model="store.fp.route"
                  placeholder="DCT SHL W22 TYN B208" size="small" /></label>
      </fieldset>
    </div>
    <template #footer>
      <el-button @click="store.overlays.fp = false">取消</el-button>
      <el-button type="primary" @click="submitFlightPlan">
        <i class="fa-solid fa-paper-plane"></i> 提交计划
      </el-button>
    </template>
  </el-dialog>
</template>