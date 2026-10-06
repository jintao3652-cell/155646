<script setup>
    import { computed, onBeforeUnmount, onMounted } from 'vue'
    import { useI18n } from 'vue-i18n'
    import router from '@/router'
    import { store, setLanguageAndSave } from '../../../store.js'

    import ScreenButton from '@/components/RMP/ScreenButton.vue'
    const { t } = useI18n()

    const LANGS = [
        { code: 'en', lbl: t('settings.languages.en'), native: t('settings.languages.en') },
        { code: 'zh-CN', lbl: t('settings.languages.zh-CN'), native: t('settings.languages.zh-CN') },
        { code: 'zh-TW', lbl: t('settings.languages.zh-TW'), native: t('settings.languages.zh-TW') },
    ]

    const current = computed(() => store.lang || 'en')

    function select(code) {
        if (code !== current.value) setLanguageAndSave(code)
    }

    function onKey(e) {
        const el = e.target
        if (el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable)) return
        if (e.key === '1') select('en')
        else if (e.key === '2') select('zh-CN')
        else if (e.key === '3') select('zh-TW')
    }

    onMounted(() => window.addEventListener('keydown', onKey))
    onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
    <el-row :gutter="10">
        <el-col :span="2" class="screen-left-button">
            <ScreenButton direction="right" :onClick="() => select('en')"></ScreenButton>
            <ScreenButton direction="right" :onClick="() => select('zh-CN')"></ScreenButton>
            <ScreenButton direction="right" :onClick="() => select('zh-TW')"></ScreenButton>
            <ScreenButton direction="right" :onClick="() => router.back()"></ScreenButton>
        </el-col>
        <el-col :span="20">
            <div class="lcd">
                <div class="lcd-body">
                    <div class="lcd-row lcd-item" v-for="l in LANGS" :key="l.code">
                        <span class="lbl">
                            <span class="arrow">&lt;</span>{{ l.lbl }}
                        </span>
                        <span class="sel" :class="{ on: current === l.code }">
                            {{ current === l.code ? '●' : '○' }}
                        </span>
                    </div>
                    <div class="lcd-row" style="border-bottom: 1px solid #fff;"></div>
                    <div class="lcd-row lcd-item">
                        <span class="lbl"><span class="arrow">&lt;</span>{{ t('settings.lblReturn') }}</span>
                    </div>
                </div>
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

    .lcd-body {
        display: flex;
        flex-direction: column;
        height: 100%;
        max-height: 220px;
    }

    .lcd-row {
        flex: 1 1 0;
        min-height: 0;
        border-bottom: none;
        width: 100%;
    }

    .lcd-item {
        display: flex;
        align-items: center;
        justify-content: space-between;
        padding: 0 12px;
        font-family: var(--mono);
        font-size: 15px;
        font-weight: bold;
        color: #fff;
        text-shadow: 0 0 6px rgba(255, 255, 255, 0.8);
        user-select: none;
    }

    .lcd-item .arrow {
        margin: 0 6px;
    }

    .sel {
        color: #2a7a4c;
        font-size: 14px;
    }

    .sel.on {
        color: var(--led-green);
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

</style>