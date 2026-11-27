<script setup>
    import { useI18n } from 'vue-i18n'
    import { computed, ref } from 'vue'
    import { store, updateAvailable, updatePercent } from "../../store.js";
    import router from '@/router'
    import ScreenButton from '@/components/RMP/ScreenButton.vue'
    import XpdrPanel from './XpdrPanel.vue'
    const { t } = useI18n()

    /* 应答机 / 无线电控制台开关（点屏幕对应区域弹出）。
     * 这里只是"发指令"入口，值以回读的 flight_data 为准。 */
    const xpdrOpen = ref(false)
    const radTab = ref('com')
    const radInit = ref({ com: 1, stby: false })

    /* 点频率格：打开面板并预选该 COM / 主备频 */
    function openRad(com, stby) {
      radInit.value = { com, stby }
      radTab.value = 'com'
      xpdrOpen.value = true
    }
    /* 点 XPDR 区域：直接进应答机标签 */
    function openXpdr() {
      radTab.value = 'xpdr'
      xpdrOpen.value = true
    }
    /* 点 MSG 键：打开消息抽屉（不是日志屏），并清掉 NEW MSG 提示。
     * MsgDrawer 挂在 App.vue，由 store.overlays.msg 控制显隐。 */
    function openMsg() {
      store.overlays.msg = true
      store.newMsg = false
    }

    /* 频率可用的前提（对齐 XVoice）：
     *   ① 航电通电（avionics_on）—— 断电时 dataref 仍读到旧值，但实际不可用
     *   ② 音频面板选中收听该 COM —— 未选中则等于听不到
     * COM3 优先用它自己的 audio_sel_com3；部分机型没有该 dataref（恒 false），
     * 此时退回只看 avionics，避免永远显示"不可用"。 */
    /* 频率可用的前提（对齐 XVoice）：
     *   ① 该 COM 通电（com_power，缺失时插件用 avionics_on 兜底）
     *   ② 音频面板选中收听该 COM —— 未选中则等于听不到
     * 实测 A330：com1_power=1, audio_selection_com1=1, audio_selection_com2=1
     * （同时守听两个 COM），audio_com_selection=6 → 发话在 COM1。
     * COM3 优先用它自己的 audio_sel_com3；部分机型没有该 dataref（恒 false），
     * 用 hasCom3Sel 标记是否存在真实读数，没有就只看电源，避免永远"不可用" */
    const com1Ok = computed(() => store.st.com1Pwr && store.st.audSel1)
    const com2Ok = computed(() => store.st.com2Pwr && store.st.audSel2)
    const com3Ok = computed(() => store.st.com3Pwr &&
                                 (!store.st.hasCom3Sel || store.st.audSel3))

    /* 发话通道高亮（txCom: 1=COM1 2=COM2 3=COM3） */
    const tx1 = computed(() => store.st.txCom === 1)
    const tx2 = computed(() => store.st.txCom === 2)
    const tx3 = computed(() => store.st.txCom === 3)

    /* 音量百分比，给 title 提示用（含副驾，便于核对前后座是否一致） */
    const vol1 = computed(() => Math.round(store.st.audVol1 * 100))
    const vol2 = computed(() => Math.round(store.st.audVol2 * 100))
    const vol1Cp = computed(() => Math.round(store.st.audVol1Cp * 100))
    const vol2Cp = computed(() => Math.round(store.st.audVol2Cp * 100))
    const tip1 = computed(() => {
      const p = [`CAPT ${vol1.value}%`]
      if (store.st.audSel1Cp) p.push(`FO ${vol1Cp.value}%`)
      if (tx1.value) p.push('TX')
      return p.join(' · ')
    })
    const tip2 = computed(() => {
      const p = [`CAPT ${vol2.value}%`]
      if (store.st.audSel2Cp) p.push(`FO ${vol2Cp.value}%`)
      if (tx2.value) p.push('TX')
      return p.join(' · ')
    })
</script>

<template>
    <el-row :gutter="10" style="height: 220px">
        <el-col :span="2" class="screen-left-button">
            <ScreenButton direction="right"></ScreenButton>
            <ScreenButton direction="right"></ScreenButton>
            <ScreenButton direction="right"></ScreenButton>
            <ScreenButton direction="right"></ScreenButton>
        </el-col>
        <el-col :span="20">
            <div class="lcd">
                <el-row class="lcd-body">
                    <el-col :span="24" class="lcd-row">
                        <el-row>
                            <el-col :span="8" class="freq-cell" @click="openRad(1, false)">
                                <span class="freq" :class="{ off: !com1Ok, tx: tx1 }"
                                      :title="tip1">{{ store.st.com1 }}</span>
                            </el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">{{ t('lcd.lblVhf1') }}</span></el-col>
                            <el-col :span="8" align="middle" justify="center" class="freq-cell"
                                    @click="openRad(1, true)"><span
                                    class="freq stby" :class="{ off: !com1Ok }">{{ store.st.com1s }}</span></el-col>
                        </el-row>
                    </el-col>
                    <el-col :span="24" class="lcd-row">
                        <el-row>
                            <el-col :span="8" class="freq-cell" @click="openRad(2, false)">
                                <span class="freq" :class="{ off: !com2Ok, tx: tx2 }"
                                      :title="tip2">{{ store.st.com2 }}</span>
                            </el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">{{ t('lcd.lblVhf2') }}</span></el-col>
                            <el-col :span="8" align="middle" justify="center" class="freq-cell"
                                    @click="openRad(2, true)"><span
                                    class="freq stby" :class="{ off: !com2Ok }">{{ store.st.com2s }}</span></el-col>
                        </el-row>
                    </el-col>
                    <el-col :span="24" class="lcd-row">
                        <el-row>
                            <el-col :span="8" class="freq-cell" @click="openRad(3, false)">
                                <span class="freq" :class="{ off: !com3Ok, tx: tx3 }">{{ store.st.com3 }}</span></el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">{{ t('lcd.lblVhf3') }}</span></el-col>
                            <el-col :span="8" align="middle" justify="center" class="freq-cell"
                                    @click="openRad(3, true)"><span
                                    class="freq stby" :class="{ off: !com3Ok }">{{ store.st.com3s }}</span></el-col>
                        </el-row>
                    </el-col>
                    <el-col :span="24" class="lcd-row" style="border-bottom: none">
                        <el-row>
                            <el-col :span="4" style="border-right: 1px solid #fff; cursor: pointer"
                                     :title="t('rad.clickHint')" @click="openXpdr">
                                <el-row>
                                    <el-col :span="24" align="middle" justify="center"><span
                                            class="squawk-text">{{ store.st.xpdrPwr || t('lcd.lblXpdr') }}</span></el-col>
                                    <el-col :span="24" align="middle" justify="center"><span
                                            class="squawk-val">{{ store.st.xpdr }}</span></el-col>
                                </el-row>
                            </el-col>
                            <el-col :span="8" style="border-right: 1px solid #fff">
                                <el-row>
                                    <el-col :span="24" align="middle" justify="center"><span
                                            class="squawk-text">{{ t('lcd.lblTcas') }}</span></el-col>
                                    <el-col :span="24" align="middle" justify="center"><span
                                            class="tcas-main">{{ store.st.tcasMode }}</span></el-col>
                                    <el-col :span="24" style="display: flex; justify-content: space-around;">
                                        <span class="tcas-val">{{ store.st.tcasAlt }}</span>
                                        <span class="tcas-val">{{ store.st.xpdrSys }}</span>
                                    </el-col>
                                </el-row>
                            </el-col>
                            <el-col :span="4" style="border-right: 1px solid #fff"></el-col>
                            <el-col :span="8" class="msg">
                                <!-- 启动检查到新版本：与"新消息"共用同一格（先到先显示），
                                     点击进更新小屏；下载中直接显示百分比 -->
                                <span v-if="updateAvailable" class="new-msg upd"
                                      :title="t('update.available')"
                                      @click="router.push('/update')">
                                    {{ t('lcd.lblUpdate') }} {{ store.update.latest }}<template
                                        v-if="store.update.state === 'downloading'"> {{ updatePercent }}%</template>
                                </span>
                                <span v-else-if="store.newMsg" class="new-msg">{{ t('lcd.lblNewMsg') }}</span>
                            </el-col>
                        </el-row>
                    </el-col>
                </el-row>
            </div>
        </el-col>
        <el-col :span="2" class="screen-right-button">
            <ScreenButton direction="left"></ScreenButton>
            <ScreenButton direction="left"></ScreenButton>
            <ScreenButton direction="left"></ScreenButton>
            <ScreenButton direction="left" :onClick="openMsg"></ScreenButton>
        </el-col>
    </el-row>

    <!-- 无线电 / 应答机控制台（点频率格或 XPDR 区域打开） -->
    <XpdrPanel v-model="xpdrOpen" :tab="radTab" :init-com="radInit.com"
               :init-stby="radInit.stby" />
</template>

<style scoped>
    .freq {
        font-size: 27px;
        font-weight: bold;
        color: var(--led-white);
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    /* 频率格可点击（打开无线电控制台），给出手型光标与悬停提示 */
    .freq-cell {
        cursor: pointer;
        border-radius: 3px;
    }

    .freq-cell:hover {
        background: rgba(53, 255, 154, 0.09);
    }

    .freq.stby {
        font-size: 20px;
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    /* 航电断电 / 未选中收听：变暗，表示"有值但不可用" */
    .freq.off {
        color: var(--led-green-dim);
        opacity: 0.45;
        text-shadow: none;
    }

    /* 当前发话通道：琥珀色高亮，一眼看出在哪个 COM 上说话 */
    .freq.tx {
        color: var(--led-amber);
        text-shadow: 0 0 8px rgba(255, 201, 94, 0.9);
    }

    .squawk-text {
        font-size: 12px;
        color: var(--led-green-dim);
        letter-spacing: 1px;
    }

    .squawk-val {
        font-size: 20px;
        color: var(--led-green);
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    .tcas-main {
        font-size: 18px;
        color: var(--led-white);
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    .msg {
        display: flex;
        align-items: center;
        justify-content: center;
    }

    .new-msg {
        font-size: 14px;
        font-weight: bold;
        letter-spacing: 1px;
        animation: msg-blink 1s linear infinite;
    }

    /* 有更新：同款闪烁但可点（进更新小屏），字号小一点避免挤爆 8 列格 */
    .new-msg.upd {
        font-size: 11.5px;
        letter-spacing: 0;
        white-space: nowrap;
        cursor: pointer;
    }

    @keyframes msg-blink {
        0%, 49% {
            color: var(--led-amber);
            text-shadow: 0 0 6px rgba(255, 201, 94, 0.8);
        }
        50%, 100% {
            color: #fff;
            text-shadow: 0 0 6px rgba(255, 255, 255, 0.8);
        }
    }
</style>
