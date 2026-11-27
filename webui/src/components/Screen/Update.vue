<script setup>
    /* 更新小屏：LCD 上的第二块过场页（从主屏右下角的"更新"角标进入）。
     * 只操作客户端整包更新：检查 → 下载（断点续传）→ 校验 → 拉起助手重启。
     * 插件的更新不在这里（XConnect.xpl / MConnect.exe 由 XUpdate 清单通道负责）。 */
    import { computed, onMounted } from 'vue'
    import { useI18n } from 'vue-i18n'
    import {
      store, checkUpdate, startUpdate, dismissUpdate, updatePercent,
    } from '../../store.js'
    import router from '@/router'
    import ScreenButton from '@/components/RMP/ScreenButton.vue'

    const { t } = useI18n()

    /* 从版本号点进来时若还没查过（idle），顺手查一次；disabled 不查，
     * 屏幕自己会说明"未配置更新地址"。 */
    onMounted(() => {
      if (store.update.state === 'idle') checkUpdate(true)
    })

    const u = computed(() => store.update)
    const currentVersion = computed(() => store.version || u.value.current || '—')
    const latestVersion = computed(() => u.value.latest || '—')

    /* 有新版本可装（available）或上次装失败（error，含下载/校验失败）时显示"更新" */
    const canUpdate = computed(() => ['available', 'error'].includes(u.value.state))
    const downloading = computed(() => u.value.state === 'downloading')

    const statusText = computed(() => {
      switch (u.value.state) {
        case 'disabled': return t('update.notConfigured')
        case 'checking': return t('update.checking')
        case 'uptodate': return t('update.upToDate')
        case 'available': return t('update.available')
        case 'downloading': return t('update.downloading', { percent: updatePercent.value })
        case 'restarting': return t('update.restarting')
        case 'updated': return t('update.updated', { version: u.value.latest || '' })
        case 'error': return t('update.failed')
        default: return t('update.idle')
      }
    })

    /* 说明区：正常显示更新日志；失败时直接显示失败原因（比"失败"有用） */
    const notes = computed(() => {
      if (u.value.state === 'error' && u.value.error) return u.value.error
      return u.value.notes || t('update.noNotes')
    })

    const created = computed(() => u.value.created || '')

    function later() {
      dismissUpdate()
      router.push('/main')
    }
  </script>

  <template>
    <el-row :gutter="10">
      <el-col :span="2" class="screen-left-button">
        <ScreenButton direction="right"></ScreenButton>
        <ScreenButton direction="right"></ScreenButton>
        <ScreenButton direction="right"></ScreenButton>
        <ScreenButton direction="right" :onClick="() => router.push('/main')"></ScreenButton>
      </el-col>
      <el-col :span="20">
        <div class="lcd">
          <div class="lcd-head">
            <span>{{ t('update.title') }}</span>
            <span class="lcd-sub">{{ statusText }}</span>
          </div>
          <div class="lcd-body">
            <div class="lcd-row ver-row">
              <span class="lbl">{{ t('update.current') }}</span>
              <span class="v">{{ currentVersion }}</span>
              <span class="blink">→</span>
              <span class="lbl">{{ t('update.latest') }}</span>
              <span class="v" :class="{ hit: canUpdate }">{{ latestVersion }}</span>
            </div>
            <div class="lcd-row notes-row">
              <el-scrollbar class="notes-scroll">
                <div class="notes-inner">{{ notes }}</div>
              </el-scrollbar>
            </div>
            <div v-if="downloading" class="lcd-row prog-row">
              <div class="bar">
                <div class="bar-fill" :style="{ width: updatePercent + '%' }"></div>
              </div>
              <span class="pct">{{ updatePercent }}%</span>
            </div>
            <div class="lcd-row act-row">
              <span v-if="canUpdate" class="act key" @click="startUpdate">{{ t('update.doUpdate') }}</span>
              <span v-else-if="downloading" class="act dim">{{ t('update.downloading', { percent: updatePercent }) }}</span>
              <span v-else-if="u.state === 'restarting'" class="act dim">{{ t('update.restartingTip') }}</span>
              <span v-else class="act dim">{{ t('update.upToDate') }}</span>
              <span class="act" @click="checkUpdate(true)">{{ t('update.recheck') }}</span>
              <span class="act dim" @click="later">{{ t('update.later') }}</span>
            </div>
            <div class="lcd-row meta-row">
              <span class="meta">{{ created }}</span>
            </div>
          </div>
        </div>
      </el-col>
      <el-col :span="2" class="screen-right-button">
        <ScreenButton direction="left"></ScreenButton>
        <ScreenButton direction="left"></ScreenButton>
        <ScreenButton direction="left"></ScreenButton>
        <ScreenButton direction="left" :onClick="() => router.push('/main')"></ScreenButton>
      </el-col>
    </el-row>
  </template>

  <style scoped>
    /* LCD 是固定 220px 高：head 占掉一行，body 必须按"剩余高度"算，
     * 否则最后一行会压到键盘区（实测踩过）。 */
    .lcd {
      display: flex;
      flex-direction: column;
    }

    .lcd-body {
      display: flex;
      flex-direction: column;
      flex: 1 1 auto;
      min-height: 0;
      overflow: hidden;
    }

    .lcd-row {
      display: flex;
      align-items: center;
      gap: 8px;
      padding: 0 8px;
      width: 100%;
      min-height: 0;
      border-bottom: 1px solid #fff;
    }

    .ver-row {
      flex: 0 0 auto;
      height: 40px;
      font-family: var(--mono);
      font-size: 15px;
      font-weight: bold;
      color: var(--led-green);
      text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    .ver-row .lbl {
      font-size: 10px;
      letter-spacing: 1px;
      color: var(--led-green-dim);
      text-shadow: none;
    }

    .ver-row .blink {
      color: var(--led-dim, #6b7a72);
      text-shadow: none;
    }

    /* 有新版本：目标版本号用琥珀色，和主屏"更新"角标一致 */
    .ver-row .v.hit {
      color: var(--led-amber);
      text-shadow: 0 0 8px rgba(255, 201, 94, 0.9);
    }

    .notes-row {
      flex: 1 1 0;
      align-items: stretch;
      padding: 0;
      overflow: hidden;
    }

    .notes-scroll {
      width: 100%;
      padding: 4px 8px;
    }

    .notes-inner {
      font-family: var(--mono);
      font-size: 11.5px;
      line-height: 1.7;
      color: #9fe6bf;
      white-space: pre-wrap;
      word-break: break-word;
      user-select: text;
    }

    .prog-row {
      flex: 0 0 auto;
      height: 18px;
      gap: 6px;
    }

    .prog-row .bar {
      flex: 1 1 auto;
      height: 6px;
      background: #0b1a12;
      border: 1px solid #1f4a34;
      border-radius: 3px;
      overflow: hidden;
    }

    .prog-row .bar-fill {
      height: 100%;
      background: var(--led-green);
      box-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
      transition: width 0.2s linear;
    }

    .prog-row .pct {
      font-family: var(--mono);
      font-size: 11px;
      color: var(--led-green);
      width: 34px;
      text-align: right;
    }

    .act-row {
      flex: 0 0 auto;
      height: 36px;
      justify-content: space-between;
      font-family: var(--mono);
      font-size: 14px;
      font-weight: bold;
      color: #fff;
      text-shadow: 0 0 6px rgba(255, 255, 255, 0.8);
    }

    .act {
      cursor: pointer;
      padding: 2px 6px;
      border-radius: 3px;
      white-space: nowrap;      /* 三个按钮固定一行，不允许"更/新"折行 */
    }

    .act:hover {
      background: rgba(53, 255, 154, 0.1);
    }

    /* 主操作（更新）用琥珀色，和"稍后/重查"区分开 */
    .act.key {
      color: var(--led-amber);
      text-shadow: 0 0 8px rgba(255, 201, 94, 0.9);
    }

    .act.dim {
      color: var(--led-green-dim);
      text-shadow: none;
      cursor: default;
      font-size: 12px;
    }

    .meta-row {
      flex: 0 0 auto;
      height: 14px;
      border-bottom: none;
      justify-content: flex-end;
    }

    .meta {
      font-family: var(--mono);
      font-size: 9.5px;
      color: #3c5c4a;
    }
  </style>
