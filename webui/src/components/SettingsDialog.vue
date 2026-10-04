<script setup>
import { store, saveSettings } from '../store.js'
</script>

<template>
  <el-dialog v-model="store.overlays.set" title="设置"
             width="min(760px, 94vw)" :append-to-body="true">
    <div class="fp-grid">
      <fieldset>
        <legend>账号</legend>
        <label>真实姓名 <el-input v-model="store.set.real_name" size="small" /></label>
        <label>CID <el-input v-model="store.set.cid" size="small" /></label>
        <label>密码 <el-input v-model="store.set.password" type="password"
                            show-password size="small" /></label>
        <el-checkbox v-model="store.set.remember">记住密码</el-checkbox>
      </fieldset>
      <fieldset>
        <legend>飞行信息</legend>
        <label>呼号 <el-input v-model="store.set.callsign" size="small" /></label>
        <label>机型（自动识别）</label>
        <label>航司 ICAO <el-input v-model="store.set.airline" maxlength="3" size="small" /></label>
      </fieldset>
      <fieldset>
        <legend>服务器</legend>
        <label>FSD 服务器 <el-input v-model="store.set.host" size="small" /></label>
        <label>FSD 端口 <el-input v-model="store.set.port" size="small" /></label>
        <label>插件地址 <el-input v-model="store.set.xphost" size="small" /></label>
        <label>插件端口 <el-input v-model="store.set.xpport" size="small" /></label>
        <el-checkbox v-model="store.set.debug">调试模式（显示插件原始报文）</el-checkbox>
        <p class="tip">客户端连接 X-Plane 内的 XConnect 插件（默认 127.0.0.1:51001），插件离线时禁止连线</p>
      </fieldset>
    </div>
    <template #footer>
      <el-button @click="store.overlays.set = false">取消</el-button>
      <el-button type="primary" @click="saveSettings">
        <i class="fa-solid fa-check"></i> 保存
      </el-button>
    </template>
  </el-dialog>
</template>