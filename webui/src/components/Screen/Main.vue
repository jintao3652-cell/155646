<script setup>
    import { store } from "../../store.js";
    import router from '@/router'
    import ScreenButton from '@/components/RMP/ScreenButton.vue'
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
                            <el-col :span="8"><span class="freq">199.998</span></el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">VHF1</span></el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">199.998</span></el-col>
                        </el-row>
                    </el-col>
                    <el-col :span="24" class="lcd-row">
                        <el-row>
                            <el-col :span="8"><span class="freq">199.998</span></el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">VHF2</span></el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">199.998</span></el-col>
                        </el-row>
                    </el-col>
                    <el-col :span="24" class="lcd-row">
                        <el-row>
                            <el-col :span="8"><span class="freq">DATA</span></el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">VHF3</span></el-col>
                            <el-col :span="8" align="middle" justify="center"><span
                                    class="freq stby">121.500</span></el-col>
                        </el-row>
                    </el-col>
                    <el-col :span="24" class="lcd-row" style="border-bottom: none">
                        <el-row>
                            <el-col :span="4" style="border-right: 1px solid #fff">
                                <el-row>
                                    <el-col :span="24" align="middle" justify="center"><span
                                            class="squawk-text">STBY</span></el-col>
                                    <el-col :span="24" align="middle" justify="center"><span
                                            class="squawk-val">2000</span></el-col>
                                </el-row>
                            </el-col>
                            <el-col :span="8" style="border-right: 1px solid #fff">
                                <el-row>
                                    <el-col :span="24" align="middle" justify="center"><span
                                            class="squawk-text">TCAS</span></el-col>
                                    <el-col :span="24" align="middle" justify="center">--------</el-col>
                                    <el-col :span="24" style="display: flex; justify-content: space-around;">
                                        <span class="tcas-val">ABV</span>
                                        <span class="tcas-val">STBY</span>
                                    </el-col>
                                </el-row>
                            </el-col>
                            <el-col :span="4" style="border-right: 1px solid #fff"></el-col>
                            <el-col :span="8" class="msg">
                                <span v-if="store.newMsg" class="new-msg">NEW MSG</span>
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
            <ScreenButton direction="left" :onClick="() => router.push('/log')"></ScreenButton>
        </el-col>
    </el-row>
</template>

<style scoped>
    .freq {
        font-size: 27px;
        font-weight: bold;
        color: var(--led-white);
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    .freq.stby {
        font-size: 20px;
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
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
