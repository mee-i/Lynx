<template>
  <ChartContainer :config="config" :class="props.class">
    <Bar :data="chartData" :options="mergedOptions" />
  </ChartContainer>
</template>

<script setup lang="ts">
import { Bar } from 'vue-chartjs';
import { computed } from 'vue';
import { getBaseOptions, getDarkOptions } from '@/utils/chart';
import ChartContainer from './ChartContainer.vue';

interface ChartConfigItem {
  label: string;
  color?: string;
}

type ChartConfig = Record<string, ChartConfigItem>;

const props = defineProps<{
  data: any[];
  index: string;
  categories: string[];
  config: ChartConfig;
  class?: string;
  options?: any;
}>();

const colorMode = useColorMode();

const mergedOptions = computed(() => {
  const base = colorMode.value === 'dark' ? getDarkOptions() : getBaseOptions();
  
  return {
    ...base,
    ...props.options,
    plugins: {
      ...base.plugins,
      ...props.options?.plugins,
    },
    scales: {
      ...base.scales,
      ...props.options?.scales,
    }
  };
});

const chartData = computed(() => ({
  labels: props.data.map(d => d[props.index]),
  datasets: props.categories.map((category) => ({
    label: props.config[category]?.label || category,
    data: props.data.map(d => d[category]),
    backgroundColor: (context: any) => {
      return getComputedStyle(context.chart.canvas).getPropertyValue(`--color-${category}`).trim() || '#888';
    },
    borderRadius: 4,
    borderSkipped: false,
    maxBarThickness: 20,
  })),
}));
</script>
