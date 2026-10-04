<script setup>
    import { store } from "../../store.js";
</script>

<template>
    <el-row class="lcd-body">
        <el-col :span="24" class="lcd-row">
            <el-row>
                <el-col :span="8"><span class="freq">199.998</span></el-col>
                <el-col :span="8" align="middle" justify="center"><span class="freq stby">VHF1</span></el-col>
                <el-col :span="8" align="middle" justify="center"><span class="freq stby">199.998</span></el-col>
            </el-row>
        </el-col>
        <el-col :span="24" class="lcd-row">
            <el-row>
                <el-col :span="8"><span class="freq">199.998</span></el-col>
                <el-col :span="8" align="middle" justify="center"><span class="freq stby">VHF2</span></el-col>
                <el-col :span="8" align="middle" justify="center"><span class="freq stby">199.998</span></el-col>
            </el-row>
        </el-col>
        <el-col :span="24" class="lcd-row">
            <el-row>
                <el-col :span="8"><span class="freq">DATA</span></el-col>
                <el-col :span="8" align="middle" justify="center"><span class="freq stby">VHF3</span></el-col>
                <el-col :span="8" align="middle" justify="center"><span class="freq stby">121.500</span></el-col>
            </el-row>
        </el-col>
        <el-col :span="24" class="lcd-row" style="border-bottom: none">
            <el-row>
                <el-col :span="4" style="border-right: 1px solid #fff">
                    <el-row>
                        <el-col :span="24" align="middle" justify="center"><span
                                class="squawk-text">STBY</span></el-col>
                        <el-col :span="24" align="middle" justify="center"><span class="squawk-val">2000</span></el-col>
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
                <el-col :span="8">
                    <!-- <LogPane></LogPane> -->
                </el-col>
            </el-row>
        </el-col>
    </el-row>
</template>

<style scoped>

    /* --- LCD + 环形导航键 --- */
    .lcd-zone {
        display: flex;
        align-items: center;
        justify-content: center;
        gap: 10px;
    }

    .lcd {
        height: 100%;
        min-width: 220px;
        flex: 1 1 auto;
        min-width: 0;
        background: var(--lcd-bg);
        border: 2px solid var(--lcd-border);
        border-radius: 6px;
        box-shadow: inset 0 0 14px rgba(0, 0, 0, 0.9), 0 0 10px rgba(30, 160, 90, 0.15);
        padding: 8px 10px;
        font-family: var(--mono);
    }

    .lcd-head {
        display: flex;
        justify-content: space-between;
        font-size: 9px;
        letter-spacing: 1px;
        color: var(--led-green-dim);
        border-bottom: 1px solid #0c1811;
        padding-bottom: 4px;
        margin-bottom: 6px;
    }

    .lcd-head .lcd-sub {
        color: #2a7a4c;
    }

    .lcd-body {
        display: flex;
        flex-direction: column;
        height: 100%;
    }

    .lcd-row {
        flex: 1 1 0;
        width: 100%;
        min-height: 0;
        border-bottom: 1px solid #fff;
    }

    .lcd-row :deep(.el-row) {
        height: 100%;
    }

    .lcd-row .k {
        font-size: 9px;
        color: var(--led-green-dim);
        letter-spacing: 1px;
        width: 40px;
    }

    .lcd-row .dimk {
        color: #1f6b40;
    }

    .lcd-row .v {
        font-size: 16px;
        color: var(--led-green);
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    .lcd-row .v.big {
        font-size: 22px;
    }

    .lcd-row .standby {
        font-size: 9px;
        color: var(--led-cyan);
        border: 1px solid #12404f;
        border-radius: 2px;
        padding: 0 4px;
        margin-left: 2px;
    }

    .lcd-row.alt-row .v {
        font-size: 14px;
    }

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
</style>
