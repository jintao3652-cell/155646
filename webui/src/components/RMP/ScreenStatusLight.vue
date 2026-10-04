<script setup>
    const props = defineProps({
        text: String,
        active: Boolean,
        onClick: Function
    })

</script>

<template>
    <div class="screen-status-wrap">
        <button class="screen-status" type="button" @click="props.onClick">
            <span class="screen-status-text">{{ props.text }}</span>
        </button>
    </div>
</template>

<style scoped>
    .screen-status-wrap {
        display: inline-flex;
        transform: rotate(var(--screen-rotation, 0deg));
        transition: transform .15s ease;
    }

    .screen-status {
        width: 35px;
        max-width: 100%;
        height: 20px;
        border-radius: 5px;
        border: none;
        background: radial-gradient(circle at 35% 30%, var(--btn-hi, #4a4e52), var(--btn-face) 70%, var(--btn-edge));
        color: var(--led-amber);
        font-size: 12px;
        font-weight: bold;
        display: flex;
        align-items: center;
        justify-content: center;
        box-shadow: 0 3px 0 var(--btn-edge), inset 0 1px 1px rgba(255, 255, 255, .18);
        cursor: pointer;
        transition: filter .15s ease, box-shadow .15s ease;
        overflow: hidden;
    }

    .screen-status-text {
        display: inline-block;
        max-width: 100%;
        white-space: nowrap;
        overflow: hidden;
        text-overflow: ellipsis;
        transform: scaleX(0.82);
        transform-origin: center;
    }

    .screen-status i {
        transform: none;
        display: block;
    }

    .screen-status:hover {
        filter: brightness(1.25);
    }

    .screen-status:active {
        transform: translateY(2px);
        box-shadow: 0 1px 0 var(--btn-edge);
    }
</style>