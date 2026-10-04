<script setup>
    import { computed, onBeforeUnmount, onMounted, ref } from 'vue'
    import router from '@/router'
    import { store, saveAccountSettings } from '../../../store.js'

    import ScreenButton from '@/components/RMP/ScreenButton.vue'

    const buf = ref('')

    const pwdMask = computed(() => '•'.repeat(Math.max(4, store.set.password.length)))

    function commitCid() {
        if (!buf.value) return
        store.set.cid = buf.value
        buf.value = ''
        saveAccountSettings()
    }

    function commitPwd() {
        if (!buf.value) return
        store.set.password = buf.value
        buf.value = ''
        saveAccountSettings()
    }

    function commitRealName() {
        if (!buf.value) return
        store.set.real_name = buf.value
        buf.value = ''
        saveAccountSettings()
    }

    function onKey(e) {
        const el = e.target
        if (el && (el.tagName === 'INPUT' || el.tagName === 'TEXTAREA' || el.isContentEditable)) return
        if (e.key === 'Backspace') {
            buf.value = buf.value.slice(0, -1)
        } else if (e.key === 'Enter') {
            commitCid()
        } else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) {
            buf.value += e.key
        }
    }

    onMounted(() => window.addEventListener('keydown', onKey))
    onBeforeUnmount(() => window.removeEventListener('keydown', onKey))
</script>

<template>
    <el-row :gutter="10">
        <el-col :span="2" class="screen-left-button">
            <ScreenButton direction="right" :onClick="commitCid"></ScreenButton>
            <ScreenButton direction="right" :onClick="commitRealName"></ScreenButton>
            <ScreenButton direction="right"></ScreenButton>
            <ScreenButton direction="right" :onClick="() => router.back()"></ScreenButton>
        </el-col>
        <el-col :span="20">
            <div class="lcd">
                <div class="lcd-body">
                    <div class="lcd-row lcd-form">
                        <div class="field">
                            <label class="lbl">CID</label>
                            <div v-if="store.set.cid" class="val">{{ store.set.cid }}</div>
                            <div v-else class="cells">
                                <span v-for="n in 4" :key="n" class="cell"></span>
                            </div>
                        </div>
                        <div class="field right">
                            <label class="lbl">PASSWORD</label>
                            <div v-if="store.set.password" class="val">{{ pwdMask }}</div>
                            <div v-else class="cells">
                                <span v-for="n in 8" :key="n" class="cell"></span>
                            </div>
                        </div>
                    </div>
                    <div class="lcd-row lcd-form">
                        <div class="field">
                            <label class="lbl">REAL NAME</label>
                            <div v-if="store.set.real_name" class="val">{{ store.set.real_name }}</div>
                            <div v-else class="cells">
                                <span v-for="n in 6" :key="n" class="cell"></span>
                            </div>
                        </div>
                    </div>
                    <div class="lcd-row" style="border-bottom: 1px solid #fff;"></div>
                    <div class="lcd-row lcd-item echo" v-if="buf">
                        <span class="lbl">{{ buf }}</span>
                    </div>
                    <div class="lcd-row lcd-item" v-else>
                        <span class="lbl"><span class="arrow">&lt;</span>RETURN</span>
                    </div>
                </div>
            </div>
        </el-col>
        <el-col :span="2" class="screen-right-button">
            <ScreenButton direction="left" :onClick="commitPwd"></ScreenButton>
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

    .lcd-form {
        display: flex;
        gap: 12px;
        padding: 6px 12px 8px;
    }

    .field {
        flex: 1 1 0;
        min-width: 0;
        display: flex;
        flex-direction: column;
        gap: 5px;
    }

    .field.right {
        align-items: flex-end;
    }

    .field.right .lbl {
        text-align: right;
    }

    .field .lbl {
        width: 72px;
        font-family: var(--mono);
        font-size: 10px;
        font-weight: bold;
        letter-spacing: 2px;
        color: #fff;
        text-shadow: 0 0 6px rgba(255, 255, 255, 0.6);
    }

    .val {
        font-family: var(--mono);
        font-size: 16px;
        font-weight: bold;
        letter-spacing: 2px;
        color: var(--led-cyan);
        text-shadow: 0 0 6px rgba(79, 216, 255, 0.8);
        white-space: nowrap;
    }

    .cells {
        display: flex;
        align-items: center;
        gap: 5px;
        height: 20px;
    }

    .cell {
        display: flex;
        align-items: center;
        justify-content: center;
        width: 11px;
        height: 14px;
        background: transparent;
        border: 1px solid #ff8c1a;
        box-shadow: 0 0 6px rgba(255, 140, 26, 0.7);
        border-radius: 1px;
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
        user-select: none;
    }

    .lcd-item .arrow {
        margin: 0 6px;
    }

    .echo {
        justify-content: flex-start;
        font-size: 13px;
        color: #fff;
    }

</style>