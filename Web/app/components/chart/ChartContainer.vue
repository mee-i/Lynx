<template>
  <div :class="['w-full h-[350px]', props.class]" :style="containerStyle">
    <slot />
  </div>
</template>

<script setup lang="ts">
import { computed } from 'vue';

interface ChartConfigItem {
  label: string;
  color?: string;
}

type ChartConfig = Record<string, ChartConfigItem>;

const props = defineProps<{
  config: ChartConfig;
  class?: string;
}>();

const containerStyle = computed(() => {
  const styles: Record<string, string> = {};
  
  Object.entries(props.config).forEach(([key, value], index) => {
    if (value.color) {
      styles[`--color-${key}`] = value.color;
    } else {
      // Fallback to CSS variables defined in main.css
      styles[`--color-${key}`] = `hsl(var(--chart-${(index % 5) + 1}))`;
    }
  });
  
  return styles;
});
</script>
