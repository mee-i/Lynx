<script setup lang="ts">
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";

interface Device {
    id: string;
    name: string;
    status: "online" | "offline";
}

const props = defineProps<{
    device: Device | null;
}>();

const isOpen = defineModel<boolean>({ default: false });

const terminalRef = ref<HTMLElement | null>(null);
const terminal = ref<Terminal | null>(null);
const fitAddon = ref<FitAddon | null>(null);
const ws = ref<WebSocket | null>(null);
const status = ref<"connecting" | "connected" | "disconnected" | "error">("disconnected");

function connect() {
    if (!props.device) return;

    status.value = "connecting";
    const clientId = `web-${Math.random().toString(36).substr(2, 9)}`;
    ws.value = new WebSocket(`${useRuntimeConfig().public.websocket_url}/ws/?type=client&id=${clientId}`);

    ws.value.onopen = () => {
        status.value = "connected";
        terminal.value?.writeln("\x1b[32m>>> Connected to Relay Server\x1b[0m");
        ws.value?.send(
            JSON.stringify({
                type: "subscribe",
                deviceId: props.device?.id,
            })
        );
    };

    ws.value.onmessage = (event) => {
        try {
            const msg = JSON.parse(event.data);
            if (msg.type === "output") {
                terminal.value?.write(msg.output);
            } else if (msg.type === "status") {
                terminal.value?.writeln(`\x1b[33m>>> Device is now ${msg.status}\x1b[0m`);
            } else if (msg.type === "error") {
                terminal.value?.writeln(`\x1b[31m>>> Error: ${msg.message}\x1b[0m`);
            }
        } catch (e) {
            console.error(e);
        }
    };

    ws.value.onclose = () => {
        status.value = "disconnected";
        terminal.value?.writeln("\x1b[31m>>> Disconnected from Relay Server\x1b[0m");
    };

    ws.value.onerror = () => {
        status.value = "error";
    };
}

function disconnect() {
    ws.value?.close();
    ws.value = null;
}

function initTerminal() {
    if (!terminalRef.value || terminal.value) return;

    terminal.value = new Terminal({
        cursorBlink: true,
        fontFamily: '"Fira Code", "Cascadia Code", Menlo, Monaco, "Courier New", monospace',
        fontSize: 14,
        theme: {
            background: "#0a0a0a",
            foreground: "#22c55e",
            cursor: "#22c55e",
            selectionBackground: "#22c55e33",
        },
    });

    fitAddon.value = new FitAddon();
    terminal.value.loadAddon(fitAddon.value);
    terminal.value.open(terminalRef.value);
    fitAddon.value.fit();

    // Handle user input
    terminal.value.onData((data) => {
        if (ws.value && status.value === "connected") {
            ws.value.send(
                JSON.stringify({
                    type: "input",
                    data: data,
                })
            );
        }
    });

    // Send initial size
    if (ws.value && status.value === "connected") {
         ws.value.send(JSON.stringify({
            type: "resize",
            cols: terminal.value.cols,
            rows: terminal.value.rows
        }));
    }

    connect();
}

function destroyTerminal() {
    disconnect();
    try {
        terminal.value?.dispose();
    } catch (e) {
        // Ignore error if addon is not loaded
        console.warn("Failed to dispose terminal:", e);
    }
    terminal.value = null;
    fitAddon.value = null;
}

// Handle resize
const resizeObserver = ref<ResizeObserver | null>(null);

watch(isOpen, (open) => {
    if (open) {
        nextTick(() => {
            initTerminal();
            if (terminalRef.value) {
                resizeObserver.value = new ResizeObserver(() => {
                    fitAddon.value?.fit();
                    // Send resize event
                    if (terminal.value && ws.value && status.value === "connected") {
                        ws.value.send(JSON.stringify({
                            type: "resize",
                            cols: terminal.value.cols,
                            rows: terminal.value.rows
                        }));
                    }
                });
                resizeObserver.value.observe(terminalRef.value);
            }
        });
    } else {
        resizeObserver.value?.disconnect();
        resizeObserver.value = null;
        destroyTerminal();
    }
});

onUnmounted(() => {
    resizeObserver.value?.disconnect();
    destroyTerminal();
});
    function clearTerminal() {
        terminal.value?.clear();
    }
</script>

<template>
    <UModal v-model:open="isOpen" :ui="{ content: 'sm:max-w-4xl' }">
        <template #content  >
            <div class="flex flex-col h-[70vh]">
                <!-- Header -->
                <div class="flex items-center justify-between p-4 border-b border-gray-800">
                    <div class="flex items-center gap-3">
                        <UIcon name="i-heroicons-command-line" class="text-xl" />
                        <div>
                            <h2 class="font-mono font-bold">{{ device?.name || "Device" }}</h2>
                            <code class="text-xs text-gray-500">{{ device?.id }}</code>
                        </div>
                    </div>
                    <div class="flex items-center gap-2">
                         <UButton
                            icon="i-heroicons-trash"
                            variant="ghost"
                            color="neutral"
                            size="xs"
                            @click="clearTerminal"
                            label="Clear"
                        />
                        <UBadge
                            :color="status === 'connected' ? 'success' : status === 'connecting' ? 'warning' : 'error'"
                            variant="subtle"
                        >
                            {{ status }}
                        </UBadge>
                        <UButton icon="i-heroicons-x-mark" variant="ghost" color="neutral" @click="isOpen = false" />
                    </div>
                </div>

                <!-- Terminal -->
                <div ref="terminalRef" class="flex-1 bg-[#0a0a0a] p-2 overflow-hidden" />
            </div>
        </template>
    </UModal>
</template>
