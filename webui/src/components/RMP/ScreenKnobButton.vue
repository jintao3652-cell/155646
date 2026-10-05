<template>
    <div class="knob"><span class="tick"></span></div>
</template>

<style scoped>
    .knob {
        width: 34px;
        height: 34px;
        border-radius: 50%;
        position: relative;
        background:
            repeating-conic-gradient(#3f4347 0deg 8deg, #303335 8deg 16deg);
        border: 3px solid #222426;
        box-shadow: 0 2px 4px rgba(0, 0, 0, .6), inset 0 1px 1px rgba(255, 255, 255, .15);
    }

    .knob .tick {
        position: absolute;
        top: 2px;
        left: 50%;
        width: 2px;
        height: 8px;
        margin-left: -1px;
        background: var(--led-amber);
        border-radius: 1px;
    }

</style>