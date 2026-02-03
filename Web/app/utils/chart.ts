import {
  Chart as ChartJS,
  Title,
  Tooltip,
  Legend,
  LineElement,
  PointElement,
  CategoryScale,
  LinearScale,
  Filler,
  BarElement,
  type ChartOptions,
} from 'chart.js';

// Register necessary components
ChartJS.register(
  Title,
  Tooltip,
  Legend,
  LineElement,
  PointElement,
  CategoryScale,
  LinearScale,
  Filler,
  BarElement
);

export const getBaseOptions = (): ChartOptions => ({
  responsive: true,
  maintainAspectRatio: false,
  interaction: {
    intersect: false,
    mode: 'index',
  },
  plugins: {
    legend: {
      display: false,
    },
    tooltip: {
      enabled: true,
      backgroundColor: 'rgba(15, 23, 42, 0.9)',
      titleColor: '#94a3b8',
      bodyColor: '#f1f5f9',
      borderColor: 'rgba(255, 255, 255, 0.1)',
      borderWidth: 1,
      padding: 8,
      boxPadding: 4,
      usePointStyle: true,
      displayColors: true,
      mode: 'index',
      intersect: false,
      callbacks: {
        title: function(context) {
          const val = context[0]?.label;
          if (val && !isNaN(Number(val))) {
            const date = new Date(Number(val));
            return `${date.getHours().toString().padStart(2, '0')}:${date.getMinutes().toString().padStart(2, '0')}:${date.getSeconds().toString().padStart(2, '0')}`;
          }
          return val;
        },
        labelColor: function(context) {
          const chart = context.chart;
          const dataset = context.dataset;
          const canvas = chart.canvas;
          
          // Get the actual color from the dataset properties
          // If it's a function (as in our case), call it or use the resolved value
          let color = '#888';
          
          if (typeof dataset.backgroundColor === 'function') {
            // @ts-ignore
            color = (dataset.backgroundColor as any)({ chart, dataset, dataIndex: context.dataIndex }) as string;
            // If it's a gradient, try to get the border color instead
            if (typeof color !== 'string') {
              if (typeof dataset.borderColor === 'function') {
                // @ts-ignore
                color = (dataset.borderColor as any)({ chart, dataset, dataIndex: context.dataIndex }) as string;
              } else {
                color = (dataset.borderColor as string) || '#888';
              }
            }
          } else {
            color = (dataset.backgroundColor as string) || '#888';
          }

          return {
            borderColor: color,
            backgroundColor: color,
            borderWidth: 0,
            borderRadius: 4,
          };
        },
        labelPointStyle: function() {
          return {
            pointStyle: 'rectRounded',
            rotation: 0
          };
        }
      },
    },
  },
  scales: {
    x: {
      grid: {
        display: false,
      },
      ticks: {
        color: '#888888',
        font: {
          family: "'Mona Sans', sans-serif",
          size: 12,
        },
      },
    },
    y: {
      border: {
        display: false,
        dash: [4, 4],
      },
      grid: {
        color: 'rgba(148, 163, 184, 0.1)',
        display: true,
      },
      ticks: {
        color: '#888888',
        font: {
          family: "'Mona Sans', sans-serif",
          size: 12,
        },
        maxTicksLimit: 5,
      },
    },
  },
});

export const getDarkOptions = (): ChartOptions => ({
  ...getBaseOptions(),
  plugins: {
    ...getBaseOptions().plugins,
    tooltip: {
      ...getBaseOptions().plugins?.tooltip,
      backgroundColor: 'rgba(9, 16, 31, 0.9)',
      titleColor: '#ffffff',
      bodyColor: '#e2e8f0',
      borderColor: 'rgba(255, 255, 255, 0.1)',
    },
  },
  scales: {
    ...getBaseOptions().scales,
    y: {
      ...getBaseOptions().scales?.y,
      grid: {
        color: 'rgba(255, 255, 255, 0.05)',
      },
    },
  },
});
