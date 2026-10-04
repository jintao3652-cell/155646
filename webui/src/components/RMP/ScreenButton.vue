<script setup>
    import { computed } from 'vue'

    const props = defineProps({
        direction: {
            type: String,
            required: true,
            default: 'up',
            validator: (value) => ['up', 'down', 'left', 'right'].includes(value)
        },
        onClick: Function
    })

    const rotationMap = {
        up: 0,
        right: 90,
        down: 180,
        left: -90
    }

    const buttonStyle = computed(() => ({
        '--screen-rotation': `${rotationMap[props.direction] || 0}deg`
    }))
</script>

<template>
    <div class="screen-button-wrap" :style="buttonStyle">
        <button class="screen-button" type="button" @click="props.onClick">
            <i class="fa-solid fa-caret-up"></i>
        </button>
    </div>
</template>

<style scoped>
    .screen-button-wrap {
        display: inline-flex;
        transform: rotate(var(--screen-rotation, 0deg));
        transition: transform .15s ease;
    }

    .screen-button {
        width: 40px;
        height: 25px;
        border-radius: 5px;
        border: none;
        background: radial-gradient(circle at 35% 30%, var(--btn-hi, #4a4e52), var(--btn-face) 70%, var(--btn-edge));
        color: var(--led-amber);
        font-size: 30px;
        display: flex;
        align-items: center;
        justify-content: center;
        box-shadow: 0 3px 0 var(--btn-edge), inset 0 1px 1px rgba(255, 255, 255, .18);
        cursor: pointer;
        transition: filter .15s ease, box-shadow .15s ease;
    }

    .screen-button i {
        transform: none;
        display: block;
    }

    .screen-button:hover {
        filter: brightness(1.25);
    }

    .screen-button:active {
        transform: translateY(2px);
        box-shadow: 0 1px 0 var(--btn-edge);
    }
</style>