<script setup lang="ts">
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import "@xterm/xterm/css/xterm.css";
import { card } from "#build/ui";

const route = useRoute();
const deviceId = route.params.id as string;

interface Device {
    id: string;
    name: string;
    status: "online" | "offline";
    lastSeen: Date;
}

const device = ref<Device | null>(null);
const ws = ref<WebSocket | null>(null);
const status = ref("disconnected");

// Terminal Refs
const terminalRef = ref<HTMLElement | null>(null);
const terminal = ref<Terminal | null>(null);
const fitAddon = ref<FitAddon | null>(null);

// Fetch device info from Server
async function fetchDevice() {
    try {
        device.value = await $fetch<Device>(`/lynx/api/devices/${deviceId}`);
    } catch (err) {
        console.error("Failed to fetch device:", err);
    }
}

onMounted(() => {
    fetchDevice();
    // Delay initialization slightly to ensure DOM is ready and layout is stable
    nextTick(() => {
        initTerminal();
    });
});

onUnmounted(() => {
    disconnect();
    destroyTerminal();
});

// Terminal Logic
function initTerminal() {
    if (!terminalRef.value || terminal.value) return;

    terminal.value = new Terminal({
        cursorBlink: true,
        fontFamily:
            '"Fira Code", "Cascadia Code", Menlo, Monaco, "Courier New", monospace',
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
                }),
            );
        }
    });

    connect();

    // Resize observer
    const resizeObserver = new ResizeObserver(() => {
        fitAddon.value?.fit();
        if (terminal.value && ws.value && status.value === "connected") {
            ws.value.send(
                JSON.stringify({
                    type: "resize",
                    cols: terminal.value.cols,
                    rows: terminal.value.rows,
                }),
            );
        }
    });

    if (terminalRef.value) {
        resizeObserver.observe(terminalRef.value);
    }
}

function destroyTerminal() {
    try {
        terminal.value?.dispose();
    } catch (e) {
        console.warn("Failed to dispose terminal:", e);
    }
    terminal.value = null;
    fitAddon.value = null;
}

function clearTerminal() {
    terminal.value?.clear();
}

function connect() {
    status.value = "connecting";
    const clientId = `web-${Math.random().toString(36).substr(2, 9)}`;
    ws.value = new WebSocket(
        `${useRuntimeConfig().public.websocket_url}/ws?type=client&id=${clientId}`,
    );

    ws.value.onopen = () => {
        status.value = "connected";
        terminal.value?.writeln("\x1b[32m>>> Connected to Relay Server\x1b[0m");
        ws.value?.send(
            JSON.stringify({
                type: "subscribe",
                deviceId: deviceId,
            }),
        );

        // Send initial size
        if (terminal.value) {
            ws.value?.send(
                JSON.stringify({
                    type: "resize",
                    cols: terminal.value.cols,
                    rows: terminal.value.rows,
                }),
            );
        }
    };

    ws.value.onmessage = (event) => {
        try {
            const msg = JSON.parse(event.data);
            if (msg.type === "output") {
                terminal.value?.write(msg.output);
            } else if (msg.type === "status") {
                terminal.value?.writeln(
                    `\x1b[33m>>> Device is now ${msg.status}\x1b[0m`,
                );
                if (device.value) {
                    device.value.status = msg.status;
                }
            } else if (msg.type === "error") {
                terminal.value?.writeln(
                    `\x1b[31m>>> Error: ${msg.message}\x1b[0m`,
                );
            } else if (msg.type === "screenshot_saved") {
                terminal.value?.writeln(
                    `\n\n\x1b[32m>>> Screenshot saved: ${msg.filename}\x1b[0m`,
                );
                isTakingScreenshot.value = false;

                fetchScreenshots();

                // Notify user
                const toast = useToast();
                toast.add({
                    title: "Screenshot saved",
                    description:
                        "The screenshot has been captured and saved successfully.",
                    icon: "i-heroicons-photo",
                    color: "success",
                });
            }
        } catch (e) {
            console.error(e);
            terminal.value?.writeln(`\x1b[31m>>> Internal Error: ${e}\x1b[0m`);
        }
    };

    ws.value.onclose = () => {
        status.value = "disconnected";
        terminal.value?.writeln(
            "\x1b[31m>>> Disconnected from Relay Server\x1b[0m",
        );
    };

    ws.value.onerror = () => {
        status.value = "error";
        terminal.value?.writeln("\x1b[31m>>> WebSocket Error\x1b[0m");
    };
}

function disconnect() {
    ws.value?.close();
    ws.value = null;
}

// Screenshot Logic
const isGalleryOpen = ref(false);
const screenshots = ref<string[]>([]);
const isTakingScreenshot = ref(false);

async function fetchScreenshots() {
    try {
        screenshots.value = await $fetch<string[]>(
            `/lynx/api/devices/${deviceId}/screenshots`,
        );
    } catch (err) {
        console.error("Failed to fetch screenshots:", err);
    }
}

function takeScreenshot() {
    if (!ws.value) return;
    isTakingScreenshot.value = true;
    ws.value.send(
        JSON.stringify({
            type: "action",
            action: "screenshot",
        }),
    );
}

const activeScreenshot = ref<string | null>(null);

const isScreenshotModalOpen = computed({
    get: () => !!activeScreenshot.value,
    set: (v) => {
        if (!v) activeScreenshot.value = null;
    },
});

const zoomLevel = ref(1);
const panX = ref(0);
const panY = ref(0);
const isDragging = ref(false);
const startX = ref(0);
const startY = ref(0);
const imageRef = ref<HTMLImageElement | null>(null);

function resetZoom() {
    zoomLevel.value = 1;
    panX.value = 0;
    panY.value = 0;
}

function handleWheel(e: WheelEvent) {
    e.preventDefault();
    const delta = e.deltaY > 0 ? -0.1 : 0.1;
    const newZoom = Math.max(0.5, Math.min(5, zoomLevel.value + delta));
    zoomLevel.value = newZoom;
}

function startDrag(e: MouseEvent) {
    isDragging.value = true;
    startX.value = e.clientX - panX.value;
    startY.value = e.clientY - panY.value;
}

function onDrag(e: MouseEvent) {
    if (isDragging.value) {
        e.preventDefault();
        panX.value = e.clientX - startX.value;
        panY.value = e.clientY - startY.value;
    }
}

function stopDrag() {
    isDragging.value = false;
}

// Reset zoom on open
watch(activeScreenshot, (v) => {
    if (v) resetZoom();
});

onMounted(async () => {
    await nextTick();
    fetchScreenshots();
});
</script>

<template>
    <div class="h-[calc(100vh-100px)] flex flex-col gap-4 p-8">
        <div class="flex justify-between items-center">
            <div class="flex items-center gap-3">
                <UButton
                    icon="i-heroicons-arrow-left"
                    variant="ghost"
                    to="/dashboard/devices"
                />
                <div>
                    <h1 class="text-xl font-mono font-bold">
                        {{ device?.name || "Device" }}
                    </h1>
                    <code class="text-xs text-gray-500">{{ deviceId }}</code>
                </div>
            </div>
            <div class="flex items-center gap-2">
                <UButton
                    icon="i-heroicons-trash"
                    variant="ghost"
                    color="neutral"
                    size="xs"
                    @click="clearTerminal"
                    >Clear</UButton
                >
                <UBadge
                    :color="device?.status === 'online' ? 'success' : 'error'"
                >
                    {{ device?.status || status }}
                </UBadge>
            </div>
        </div>

        <UCard
            class="flex-1 flex flex-col min-h-0"
            :ui="{ body: 'flex-1 flex flex-col min-h-0 p-0' }"
        >
            <div
                ref="terminalRef"
                class="flex-1 bg-[#0a0a0a] p-2 overflow-hidden"
            />
        </UCard>

        <!-- Screenshot Gallery Modal -->
        <UCard :ui="{ body: 'w-full sm:max-w-4xl' }">
            <template #header>
                <div class="flex items-center gap-5">
                    <h3 class="text-base font-semibold leading-6 text-white">
                        Screenshots
                    </h3>
                    <UButton
                        icon="i-heroicons-camera"
                        color="neutral"
                        variant="solid"
                        size="xs"
                        :loading="isTakingScreenshot"
                        :disabled="device?.status !== 'online'"
                        @click="takeScreenshot"
                    >
                        Take Screenshot
                    </UButton>
                </div>
            </template>

            <div class="p-4">
                <div
                    v-if="screenshots.length === 0"
                    class="text-center text-gray-500 py-8"
                >
                    No screenshots found.
                </div>
                <div
                    v-else
                    class="grid grid-cols-2 md:grid-cols-3 lg:grid-cols-4 gap-4 max-h-[60vh] overflow-y-auto"
                >
                    <div
                        v-for="shot in screenshots"
                        :key="shot"
                        class="group relative aspect-video bg-gray-900 rounded-lg overflow-hidden cursor-pointer border border-gray-800 hover:border-primary-500 transition-colors"
                        @click="activeScreenshot = shot"
                    >
                        <img
                            :src="`/lynx/images/${deviceId}/${shot}`"
                            class="w-full h-full object-cover"
                            loading="lazy"
                        />
                        <div
                            class="absolute inset-0 bg-black/50 opacity-0 group-hover:opacity-100 transition-opacity flex items-center justify-center"
                        >
                            <UIcon
                                name="i-heroicons-eye"
                                class="text-white w-8 h-8"
                            />
                        </div>
                    </div>
                </div>
            </div>
        </UCard>

        <UModal
            v-model:open="isScreenshotModalOpen"
            :ui="{ 
                base: 'relative text-left rtl:text-right overflow-hidden w-full h-full flex flex-col bg-gray-900',
                width: 'w-full max-w-[95vw]',
                height: 'h-[90vh]',
                padding: 'p-0',
                rounded: 'rounded-xl',
                overlay: { background: 'bg-gray-900/90 backdrop-blur-sm' }
            }"
            fullscreen
        >
          <template #body>
            <div class="relative w-full h-full flex flex-col bg-black/90">
                <!-- Toolbar -->
                <div class="absolute top-4 left-1/2 -translate-x-1/2 z-50 flex items-center gap-2 bg-gray-800/80 backdrop-blur rounded-full px-4 py-2 border border-white/10 shadow-xl">
                    <UButton
                        icon="i-heroicons-minus"
                        variant="ghost"
                        color="neutral"
                        size="sm"
                        @click="zoomLevel = Math.max(0.5, zoomLevel - 0.25)"
                    />
                    <span class="text-white font-mono text-sm min-w-[3ch] text-center">{{ Math.round(zoomLevel * 100) }}%</span>
                     <UButton
                        icon="i-heroicons-plus"
                        variant="ghost"
                        color="neutral"
                        size="sm"
                        @click="zoomLevel = Math.min(5, zoomLevel + 0.25)"
                    />
                     <div class="w-px h-4 bg-white/20 mx-1"></div>
                    <UButton
                        icon="i-heroicons-arrows-pointing-out"
                        variant="ghost"
                        color="neutral"
                        size="sm"
                        @click="resetZoom"
                    />
                     <div class="w-px h-4 bg-white/20 mx-1"></div>
                     <UButton
                        icon="i-heroicons-x-mark"
                        variant="ghost"
                        color="neutral"
                        size="sm"
                        @click="activeScreenshot = null"
                    />
                </div>

                <!-- Image Container -->
                <div 
                    class="flex-1 overflow-hidden relative cursor-grab active:cursor-grabbing flex items-center justify-center"
                    @wheel="handleWheel"
                    @mousedown="startDrag"
                    @mousemove="onDrag"
                    @mouseup="stopDrag"
                    @mouseleave="stopDrag"
                >
                    <img
                        v-if="activeScreenshot"
                        ref="imageRef"
                        :src="`/lynx/images/${deviceId}/${activeScreenshot}`"
                        :class="['max-w-full max-h-full object-contain select-none', { 'transition-transform duration-200 ease-out': !isDragging }]"
                        :style="{
                            transform: `translate(${panX}px, ${panY}px) scale(${zoomLevel})`,
                            cursor: isDragging ? 'grabbing' : 'grab'
                        }"
                        draggable="false"
                    />
                </div>
            </div>
          </template>
        </UModal>
    </div>
</template>
