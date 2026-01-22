<script setup lang="ts">
const route = useRoute()
const deviceId = route.params.id as string

interface Device {
  id: string;
  name: string;
  status: 'online' | 'offline';
  lastSeen: Date;
}

const device = ref<Device | null>(null);
const logs = ref<string[]>([]);
const commandInput = ref('');
const ws = ref<WebSocket | null>(null);
const status = ref('disconnected');
const logsContainer = ref<HTMLElement | null>(null);

// Fetch device info from Server
async function fetchDevice() {
  try {
    device.value = await $fetch<Device>(`http://localhost:3001/api/devices/${deviceId}`);
  } catch (err) {
    console.error('Failed to fetch device:', err);
  }
}

// Auto-scroll
watch(logs, async () => {
  await nextTick();
  if (logsContainer.value) {
    logsContainer.value.scrollTop = logsContainer.value.scrollHeight;
  }
}, { deep: true });

onMounted(() => {
  fetchDevice();
  connect();
});

onUnmounted(() => {
  ws.value?.close();
});

function connect() {
  status.value = 'connecting';
  const clientId = `web-${Math.random().toString(36).substr(2, 9)}`;
  ws.value = new WebSocket(`ws://localhost:3001?type=client&id=${clientId}`);

  ws.value.onopen = () => {
    status.value = 'connected';
    logs.value.push('>>> Connected to Relay Server');
    ws.value?.send(JSON.stringify({
      type: 'subscribe',
      deviceId: deviceId
    }));
  };

  ws.value.onmessage = (event) => {
    try {
      const msg = JSON.parse(event.data);
      if (msg.type === 'output') {
        logs.value.push(msg.output);
      } else if (msg.type === 'status') {
        logs.value.push(`>>> Device is now ${msg.status}`);
        if (device.value) {
          device.value.status = msg.status;
        }
      } else if (msg.type === 'error') {
        logs.value.push(`>>> Error: ${msg.message}`);
      }
    } catch (e) {
      console.error(e);
    }
  };

  ws.value.onclose = () => {
    status.value = 'disconnected';
    logs.value.push('>>> Disconnected from Relay Server');
  };

  ws.value.onerror = () => {
    status.value = 'error';
  };
}

function sendCommand() {
  if (!commandInput.value.trim() || !ws.value) return;

  const cmd = commandInput.value;
  logs.value.push(`$ ${cmd}`);

  ws.value.send(JSON.stringify({
    type: 'command',
    command: cmd
  }));

  commandInput.value = '';
}

function clearLogs() {
  logs.value = [];
}
</script>

<template>
  <div class="h-[calc(100vh-100px)] flex flex-col gap-4">
    <div class="flex justify-between items-center">
      <div class="flex items-center gap-3">
        <UButton icon="i-heroicons-arrow-left" variant="ghost" to="/dashboard/devices" />
        <div>
          <h1 class="text-xl font-mono font-bold">{{ device?.name || 'Device' }}</h1>
          <code class="text-xs text-gray-500">{{ deviceId }}</code>
        </div>
      </div>
      <div class="flex items-center gap-2">
        <UButton icon="i-heroicons-trash" variant="ghost" color="neutral" size="xs" @click="clearLogs">Clear</UButton>
        <UBadge :color="device?.status === 'online' ? 'success' : 'error'">
          {{ device?.status || status }}
        </UBadge>
      </div>
    </div>

    <UCard class="flex-1 flex flex-col min-h-0" :ui="{ body: 'flex-1 flex flex-col min-h-0 p-0' }">
      <div
        ref="logsContainer"
        class="flex-1 overflow-y-auto bg-gray-950 text-green-400 p-4 font-mono text-sm whitespace-pre-wrap select-text"
      >
        <div v-for="(log, i) in logs" :key="i">{{ log }}</div>
        <div v-if="logs.length === 0" class="text-gray-600">Waiting for output...</div>
      </div>

      <div class="p-2 border-t border-gray-800 bg-gray-900">
        <form @submit.prevent="sendCommand" class="flex gap-2">
          <UInput
            v-model="commandInput"
            icon="i-heroicons-command-line"
            placeholder="Enter command..."
            class="flex-1 font-mono"
            :disabled="device?.status !== 'online'"
            autofocus
            autocomplete="off"
          />
          <UButton type="submit" color="primary" :disabled="device?.status !== 'online'">Send</UButton>
        </form>
      </div>
    </UCard>
  </div>
</template>
