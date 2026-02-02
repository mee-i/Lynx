<template>
  <ChartContainer :config="config" :class="props.class">
    <Line :data="chartData" :options="mergedOptions" />
  </ChartContainer>
</template>

<script setup lang="ts">
import { Line } from 'vue-chartjs';
import { computed } from 'vue';
import { getBaseOptions, getDarkOptions } from '~/app/utils/chart-defaults';
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
  
  // Merge with custom options if provided
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
    borderColor: (context: any) => {
      const color = getComputedStyle(context.chart.canvas).getPropertyValue(`--color-${category}`).trim();
      return color || `hsl(var(--chart-1))`;
    },
    backgroundColor: (context: any) => {
      const chart = context.chart;
      const { ctx, chartArea } = chart;
      if (!chartArea) return null;
      
      const color = getComputedStyle(chart.canvas).getPropertyValue(`--color-${category}`).trim();
      if (!color) return 'transparent';

      // Transform hsl(...) to hsla(... / opacity) for the gradient
      const gradient = ctx.createLinearGradient(0, chartArea.top, 0, chartArea.bottom);
      
      // Handle Hex colors
      if (color.startsWith('#')) {
        const hex = color.replace('#', '');
        const r = parseInt(hex.substring(0, 2), 16);
        const g = parseInt(hex.substring(2, 4), 16);
        const b = parseInt(hex.substring(4, 6), 16);
        gradient.addColorStop(0, `rgba(${r}, ${g}, ${b}, 0.3)`);
        gradient.addColorStop(1, `rgba(${r}, ${g}, ${b}, 0)`);
      } else {
        // Handle hsl(H S L) or rgb(...)
        const baseColor = color.replace(')', '');
        gradient.addColorStop(0, `${baseColor} / 0.3)`);
        gradient.addColorStop(1, `${baseColor} / 0)`);
      }
      return gradient;
    },
    tension: 0.4,
    pointRadius: 0,
    pointHoverRadius: 4,
    pointHoverBackgroundColor: (context: any) => {
      return getComputedStyle(context.chart.canvas).getPropertyValue(`--color-${category}`).trim() || '#888';
    },
    pointHoverBorderColor: '#fff',
    pointHoverBorderWidth: 2,
    borderWidth: 2,
    fill: true,
  })),
}));
</script>
