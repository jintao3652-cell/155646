<script setup>
import ScreenButton from '@/components/RMP/ScreenButton.vue'
import router from '@/router'
import { useI18n } from 'vue-i18n'

const props = defineProps({
  title: { type: String, default: '' },
  // 列模板（CSS grid-track 列表），表头与行共用，保证对齐
  cols: { type: String, default: '' },
})
const { t } = useI18n()
</script>

<template>
  <el-row :gutter="10" style="height: 220px">
    <el-col :span="2" class="screen-left-button">
      <ScreenButton direction="right" :onClick="() => router.back()"></ScreenButton>
      <ScreenButton direction="right"></ScreenButton>
      <ScreenButton direction="right"></ScreenButton>
      <ScreenButton direction="right"></ScreenButton>
    </el-col>
    <el-col :span="20">
      <div class="lcd ol-lcd">
        <div class="ol-title">{{ props.title }}</div>
        <div class="ol-grid ol-head" :style="{ gridTemplateColumns: props.cols }">
          <slot name="header"></slot>
        </div>
        <div class="ol-rule"></div>
        <el-scrollbar class="ol-scroll">
          <div class="ol-list" :style="{ '--cols': props.cols }">
            <slot></slot>
          </div>
        </el-scrollbar>
        <div class="ol-foot"><span class="arrow">&lt;</span>{{ t('online.return') }}</div>
      </div>
    </el-col>
    <el-col :span="2" class="screen-right-button">
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
    </el-col>
  </el-row>
</template>

<style scoped>
.ol-lcd {
  display: flex;
  flex-direction: column;
  height: 220px;
  box-sizing: border-box;
}

.ol-title {
  font-size: 12px;
  font-weight: bold;
  color: #fff;
  letter-spacing: 1px;
  text-shadow: 0 0 6px rgba(255, 255, 255, 0.7);
  margin-bottom: 3px;
}

.ol-grid {
  display: grid;
}

.ol-head {
  color: var(--led-green-dim);
  font-size: 9px;
  letter-spacing: 0.5px;
}

.ol-head > :deep(span) {
  overflow: hidden;
  white-space: nowrap;
  text-overflow: ellipsis;
}

.ol-rule {
  border-bottom: 1px solid #fff;
  margin: 3px 0;
}

.ol-scroll {
  flex: 1 1 auto;
  min-height: 0;
}

.ol-foot {
  font-size: 11px;
  color: #fff;
  text-shadow: 0 0 6px rgba(255, 255, 255, 0.7);
  margin-top: 3px;
}

.ol-foot .arrow {
  margin-right: 4px;
}
</style>
