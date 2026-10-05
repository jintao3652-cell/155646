<script setup>
    import { defineProps } from 'vue'
    const props = defineProps({
        text: {
            type: String,
            required: true
        },
        active: {
            type: Boolean,
            default: false
        },
        danger: {
            type: Boolean,
            default: false
        },
        onClick: Function
    })
</script>

<template>

    <button class="fn" @click="props.onClick">
        <div class="light">
            <div class="light-item" :class="{ 'active': props.active, 'danger': props.danger }"></div>
            <div class="light-item" :class="{ 'active': props.active, 'danger': props.danger }"></div>
            <div class="light-item" :class="{ 'active': props.active, 'danger': props.danger }"></div>
        </div>
        <span>{{ props.text }}</span>
    </button>
</template>


<style scoped>
    .light-item {
        border-bottom: #33363a 1.5px solid;
    }

    .light-item.active {
        border-bottom: #18d757 1.5px solid;
        text-shadow: 0 0 6px rgba(53, 255, 154, 0.8);
    }

    .light-item.danger {
        border-bottom: #ff5b62 1.5px solid;
        text-shadow: 0 0 6px rgba(255, 91, 98, 0.8);
    }

    .light {
        height: 50%;
        display: flex;
        flex-direction: column;
        justify-content: space-around;
        padding: 2px 11px;
    }

    .fn {
        height: 25px;
        width: 40px;
        border-radius: 5px;
        border: none;
        background: linear-gradient(180deg, #3e4246, #33363a);
        color: var(--led-amber);
        font-family: var(--mono);
        font-size: 12px;
        font-weight: 700;
        letter-spacing: 0.5px;
        box-shadow:
            0 2px 0 #1c1e20,
            inset 0 1px 1px rgba(255, 255, 255, 0.12);
        cursor: pointer;
        text-shadow: 0 0 4px rgba(255, 201, 94, 0.35);
    }

    .fn:hover {
        filter: brightness(1.2);
    }

    .fn:active {
        transform: translateY(1px);
        box-shadow: 0 1px 0 #1c1e20;
    }
</style>