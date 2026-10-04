<script setup>
  import router from '@/router'

  import ScreenButton from '@/components/RMP/ScreenButton.vue'
</script>

<template>
  <el-row :gutter="10">
    <el-col :span="2" class="screen-left-button">
      <ScreenButton direction="right" :onClick="() => router.push('/settings/account')"></ScreenButton>
      <ScreenButton direction="right" :onClick="() => router.push('/settings/login_info')"></ScreenButton>
      <ScreenButton direction="right"></ScreenButton>
      <ScreenButton direction="right" :onClick="() => router.back()"></ScreenButton>
    </el-col>
    <el-col :span="20">
      <div class="lcd">
        <div class="lcd-body">
          <div class="lcd-row lcd-item">
            <span class="lbl"><span class="arrow">&lt;</span>ACCOUNT</span>
            <span class="lbl">SERVER<span class="arrow">&gt;</span></span>
          </div>
          <div class="lcd-row lcd-item">
            <span class="lbl"><span class="arrow">&lt;</span>LOGIN INFO</span>
          </div>
          <div class="lcd-row lcd-item" style="border-bottom: 1px solid #fff"></div>
          <div class="lcd-row lcd-item">
            <span class="lbl"><span class="arrow">&lt;</span>RETURN</span>
          </div>
        </div>
      </div>
    </el-col>
    <el-col :span="2" class="screen-right-button">
      <ScreenButton direction="left" :onClick="() => router.push('/settings/server')"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
      <ScreenButton direction="left"></ScreenButton>
    </el-col>
  </el-row>
</template>


<style scoped>

  .lcd-body {
    display: flex;
    flex-direction: column;
    height: 100%;
    max-height: 220px;
  }

  .lcd-row {
    flex: 1 1 0;
    min-height: 0;
    width: 100%;
    border-bottom: none;
  }

  .lcd-item {
    display: flex;
    align-items: center;
    justify-content: space-between;
    padding: 0 12px;
    font-family: var(--mono);
    font-size: 16px;
    font-weight: bold;
    color: #fff;
    text-shadow: 0 0 6px rgba(255, 255, 255, 0.8);
  }

  .lcd-item .arrow {
    margin: 0 6px;
  }

</style>
