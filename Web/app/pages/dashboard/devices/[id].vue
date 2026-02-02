<script setup lang="ts">
import { Terminal } from "@xterm/xterm";
import { FitAddon } from "@xterm/addon-fit";
import type { TableColumn, TabsItem } from "@nuxt/ui";
import { formatRelativeTime } from "~/utils/formatters";
import "@xterm/xterm/css/xterm.css";
import { card } from "#build/ui";

const route = useRoute();
const deviceId = route.params.id as string;

interface Device {
    id: string;
    name: string;
    status: "online" | "offline";
    lastSeen: Date;
    group?: string;
    tags?: string[];
    os?: string;
    version?: string;
}

interface DeviceLog {
    id: number;
    type: "connect" | "disconnect" | "reconnect";
    timestamp: string | Date;
    message?: string;
}

const device = ref<Device | null>(null);
const ws = ref<WebSocket | null>(null);
const status = ref("disconnected");
const logs = ref<DeviceLog[]>([]);

// Commands & History
const commandHistory = ref<string[]>([]);
const osOptions = ["Windows", "Linux"];
const selectedOs = ref("Windows");

const quickCommands: Record<string, { label: string; command: string; icon: string }[]> = {
    Windows: [
        { label: "Current User", command: "whoami", icon: "i-heroicons-user" },
        { label: "Working Dir", command: "cd", icon: "i-heroicons-folder" },
        { label: "List Files", command: "dir", icon: "i-heroicons-document-text" },
        { label: "System Info", command: "systeminfo", icon: "i-heroicons-information-circle" },
        { label: "IP Config", command: "ipconfig", icon: "i-heroicons-globe-alt" },
        { label: "Disk Usage", command: "wmic logicaldisk get size,freespace,caption", icon: "i-heroicons-circle-stack" },
        { label: "Memory Info", command: "systeminfo | findstr Memory", icon: "i-heroicons-cpu-chip" },
        { label: "Process List", command: "tasklist", icon: "i-heroicons-queue-list" },
        { label: "Network Stats", command: "netstat -an", icon: "i-heroicons-arrows-right-left" },
        { label: "Flush DNS", command: "ipconfig /flushdns", icon: "i-heroicons-bolt" },
    ],
    Linux: [
        { label: "Current User", command: "whoami", icon: "i-heroicons-user" },
        { label: "Working Dir", command: "pwd", icon: "i-heroicons-folder" },
        { label: "List Files", command: "ls -la", icon: "i-heroicons-document-text" },
        { label: "System Info", command: "uname -a", icon: "i-heroicons-information-circle" },
        { label: "IP Address", command: "ip addr", icon: "i-heroicons-globe-alt" },
        { label: "Disk Usage", command: "df -h", icon: "i-heroicons-circle-stack" },
        { label: "Memory Info", command: "free -h", icon: "i-heroicons-cpu-chip" },
        { label: "Process List", command: "ps aux", icon: "i-heroicons-queue-list" },
        { label: "Network Stats", command: "netstat -an", icon: "i-heroicons-arrows-right-left" },
        { label: "Uptime", command: "uptime", icon: "i-heroicons-clock" },
    ],
};

const activeQuickCommands = computed(() => quickCommands[selectedOs.value] || []);

const sidebarTabs: TabsItem[] = [
    { label: "Quick", icon: "i-heroicons-bolt", slot: "quick" as const },
    { label: "History", icon: "i-heroicons-clock", slot: "history" as const },
    { label: "Logs", icon: "i-heroicons-list-bullet", slot: "logs" as const },
];

function sendCommand(cmd: string) {
    if (!ws.value || status.value !== "connected") return;

    // Add to history if not duplicate of last
    if (cmd && commandHistory.value[0] !== cmd) {
        commandHistory.value.unshift(cmd);
        if (commandHistory.value.length > 50) commandHistory.value.pop();
    }

    ws.value.send(
        JSON.stringify({
            type: "input",
            data: cmd + "\r",
        }),
    );
}

const logColumns: TableColumn<DeviceLog>[] = [
    {
        accessorKey: "type",
        header: "Event",
    },
    {
        accessorKey: "timestamp",
        header: "Time",
    },
];

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

async function fetchLogs() {
    try {
        logs.value = await $fetch<DeviceLog[]>(`/lynx/api/devices/${deviceId}/logs`);
    } catch (err) {
        console.error("Failed to fetch logs:", err);
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

// Edit/Delete Logic
const editModalOpen = ref(false);
const deleteModalOpen = ref(false);
const editForm = reactive({ name: "", group: "" });

function openEditModal() {
    if (!device.value) return;
    editForm.name = device.value.name;
    editForm.group = device.value.group || "";
    editModalOpen.value = true;
}

async function saveDevice() {
    if (!device.value) return;
    try {
        const updated = await $fetch<Device>(`/lynx/api/devices/${device.value.id}`, {
            method: "POST",
            body: { name: editForm.name, group: editForm.group || undefined },
        });
        device.value = updated;
        editModalOpen.value = false;
    } catch (e) {
        console.error("Failed to update device:", e);
    }
}

async function deleteDevice() {
    if (!device.value) return;
    try {
        await $fetch(`/lynx/api/devices/${device.value.id}`, {
            method: "DELETE",
        });
        navigateTo("/dashboard/devices");
    } catch (e) {
        console.error("Failed to delete device:", e);
    }
}

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
    let currentInputBuffer = "";
    terminal.value.onData((data) => {
        if (ws.value && status.value === "connected") {
            // Capture history from keyboard
            if (data === "\r" || data === "\n" || data === "\r\n") {
                const cmd = currentInputBuffer.trim();
                if (cmd) {
                    if (commandHistory.value[0] !== cmd) {
                        commandHistory.value.unshift(cmd);
                        if (commandHistory.value.length > 50) commandHistory.value.pop();
                    }
                }
                currentInputBuffer = "";
            } else if (data === "\x7f" || data === "\b") {
                currentInputBuffer = currentInputBuffer.slice(0, -1);
            } else if (data.length === 1 && data.charCodeAt(0) >= 32) {
                currentInputBuffer += data;
            }

            ws.value.send(
                JSON.stringify({
                    type: "input",
                    data: data,
                }),
            );
        }
    });

    connect();

    // Resize observer with debounce and guard to prevent loops
    let resizeTimer: ReturnType<typeof setTimeout> | null = null;
    let lastCols = 0;
    let lastRows = 0;
    let isResizing = false;

    const resizeObserver = new ResizeObserver(() => {
        if (isResizing) return;
        if (resizeTimer) clearTimeout(resizeTimer);
        
        resizeTimer = setTimeout(() => {
            if (!terminal.value || !fitAddon.value) return;
            
            isResizing = true;
            try {
                fitAddon.value.fit();
                
                const newCols = terminal.value.cols;
                const newRows = terminal.value.rows;
                
                // Only send resize if dimensions actually changed
                if (newCols !== lastCols || newRows !== lastRows) {
                    lastCols = newCols;
                    lastRows = newRows;
                    
                    if (ws.value && status.value === "connected") {
                        ws.value.send(
                            JSON.stringify({
                                type: "resize",
                                cols: newCols,
                                rows: newRows,
                            }),
                        );
                    }
                }
            } finally {
                // Release guard after a short delay to absorb any triggered resize events
                setTimeout(() => { isResizing = false; }, 100);
            }
        }, 300);
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
                    `\n\x1b[33m>>> Device is now ${msg.status}\x1b[0m`,
                );
                if (device.value) {
                    device.value.status = msg.status;
                }
                if (msg.status == "online") {
                    ws.value?.send(
                        JSON.stringify({
                            type: "hello",
                        })
                    );
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
    fetchLogs();
});

const powerItems = [
    [{
        label: 'Restart',
        icon: 'i-heroicons-arrow-path',
        click: () => sendPowerCommand('restart')
    }, {
        label: 'Shutdown',
        icon: 'i-heroicons-power',
        click: () => sendPowerCommand('shutdown')
    }]
]

function sendPowerCommand(action: 'restart' | 'shutdown') {
    if (!ws.value || !device.value) return;

    let command = '';
    const os = device.value.os || 'Windows'; // Default to Windows if unknown

    if (os.includes('Windows')) {
        if (action === 'restart') command = 'shutdown /r /t 0';
        if (action === 'shutdown') command = 'shutdown /s /t 0';
    } else {
        // Linux/Unix
        if (action === 'restart') command = 'shutdown -r now';
        if (action === 'shutdown') command = 'shutdown -h now';
    }

    if (command) {
        ws.value.send(JSON.stringify({
            type: 'command',
            command: command
        }));
        
        useToast().add({
            title: `Sending ${action} command`,
            description: 'The device should respond shortly.',
            icon: 'i-heroicons-paper-airplane',
            color: 'primary'
        });
    }
}
</script>

<template>
    <div class="min-h-[calc(100vh-100px)] flex flex-col gap-4 p-4 md:p-6">
        <!-- Header -->
        <div class="flex flex-col sm:flex-row justify-between items-start sm:items-center gap-3">
            <div class="flex items-center gap-3">
                <UButton
                    icon="i-heroicons-arrow-left"
                    variant="ghost"
                    to="/dashboard/devices"
                />
                <div>
                    <div class="flex items-center gap-2 flex-wrap">
                         <h1 class="text-lg md:text-xl font-mono font-bold">
                            {{ device?.name || "Device" }}
                        </h1>
                        <UBadge v-if="device?.group" color="primary" variant="soft" size="xs">
                            {{ device.group }}
                        </UBadge>
                        <UBadge
                            :color="device?.status === 'online' ? 'success' : 'neutral'"
                            size="xs"
                        >
                            {{ device?.status || status }}
                        </UBadge>
                    </div>
                   
                    <div class="flex items-center gap-2 flex-wrap">
                        <code class="text-xs text-gray-500">{{ deviceId }}</code>
                        <span v-if="device?.os" class="text-xs text-gray-500">• {{ device.os }} <span v-if="device.version">(v{{ device.version }})</span></span>
                    </div>
                </div>
            </div>
            <div class="flex items-center gap-2 flex-wrap">
                 <UButton
                    icon="i-heroicons-pencil-square"
                    variant="ghost"
                    color="neutral"
                    size="xs"
                    @click="openEditModal"
                >Edit</UButton>
                <UButton
                    icon="i-heroicons-trash"
                    variant="ghost"
                    color="error"
                    size="xs"
                    @click="deleteModalOpen = true"
                >Delete</UButton>
                <UDropdown :items="powerItems" :popper="{ placement: 'bottom-end' }">
                    <UButton
                        icon="i-heroicons-power"
                        color="warning"
                        variant="soft"
                        size="xs"
                        :disabled="device?.status !== 'online'"
                    >Power</UButton>
                </UDropdown>
            </div>
        </div>

        <!-- Bento Grid Layout -->
        <div class="flex-1 grid grid-cols-1 md:grid-cols-2 lg:grid-cols-4 gap-4 auto-rows-fr">
            
            <!-- Terminal Card - Large (spans 3 cols, 2 rows) -->
            <UCard
                class="md:col-span-2 lg:col-span-3 lg:row-span-2 flex flex-col min-h-[350px]"
                :ui="{ body: 'flex-1 flex flex-col p-0 overflow-hidden' }"
            >
                <template #header>
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <UIcon name="i-heroicons-command-line" class="text-primary-400" />
                            <span class="text-sm font-semibold">Terminal</span>
                        </div>
                        <UButton
                            icon="i-heroicons-trash"
                            variant="ghost"
                            color="neutral"
                            size="xs"
                            @click="clearTerminal"
                        >Clear</UButton>
                    </div>
                </template>
                <div
                    ref="terminalRef"
                    class="flex-1 bg-[#0a0a0a] p-2 overflow-hidden"
                />
            </UCard>

            <!-- Quick Commands Card (spans 1 col, 1 row) -->
            <UCard class="flex flex-col min-h-[200px]" :ui="{ body: 'flex-1 flex flex-col p-0 overflow-hidden' }">
                <template #header>
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <UIcon name="i-heroicons-bolt" class="text-yellow-400" />
                            <span class="text-sm font-semibold">Quick Commands</span>
                        </div>
                        <USelectMenu v-model="selectedOs" :items="osOptions" size="xs" class="w-20" />
                    </div>
                </template>
                <div class="flex-1 p-3 overflow-y-auto">
                    <div class="grid grid-cols-1 gap-1.5">
                        <UButton
                            v-for="cmd in activeQuickCommands"
                            :key="cmd.command"
                            :icon="cmd.icon"
                            variant="ghost"
                            color="neutral"
                            size="xs"
                            block
                            class="justify-start font-mono text-[10px]"
                            @click="sendCommand(cmd.command)"
                        >
                            {{ cmd.label }}
                        </UButton>
                    </div>
                </div>
            </UCard>

            <!-- History Card (spans 1 col, 1 row) -->
            <UCard class="flex flex-col min-h-[200px]" :ui="{ body: 'flex-1 flex flex-col p-0 overflow-hidden' }">
                <template #header>
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <UIcon name="i-heroicons-clock" class="text-blue-400" />
                            <span class="text-sm font-semibold">History</span>
                            <UBadge v-if="commandHistory.length > 0" color="neutral" variant="subtle" size="xs">
                                {{ commandHistory.length }}
                            </UBadge>
                        </div>
                        <UButton
                            v-if="commandHistory.length > 0"
                            icon="i-heroicons-trash"
                            variant="ghost"
                            color="error"
                            size="xs"
                            @click="commandHistory = []"
                        />
                    </div>
                </template>
                <div class="flex-1 overflow-y-auto">
                    <div v-if="commandHistory.length === 0" class="flex-1 flex flex-col items-center justify-center text-gray-500 text-xs p-4 text-center h-full">
                        <UIcon name="i-heroicons-clock" class="w-8 h-8 mb-2 opacity-20" />
                        No history yet
                    </div>
                    <div v-else class="p-2 flex flex-col gap-1">
                        <div
                            v-for="(cmd, i) in commandHistory.slice(0, 10)"
                            :key="i"
                            class="group flex items-center gap-2 p-2 rounded hover:bg-white/5 cursor-pointer text-xs"
                            @click="sendCommand(cmd)"
                        >
                            <UIcon name="i-heroicons-chevron-right" class="text-gray-600 w-3 h-3 shrink-0" />
                            <code class="text-gray-300 truncate flex-1">{{ cmd }}</code>
                        </div>
                    </div>
                </div>
            </UCard>

            <!-- Screenshots Card (spans 2 cols on lg) -->
            <UCard class="md:col-span-2 lg:col-span-2 flex flex-col min-h-[200px]" :ui="{ body: 'flex-1 p-0 overflow-hidden' }">
                <template #header>
                    <div class="flex items-center justify-between">
                        <div class="flex items-center gap-2">
                            <UIcon name="i-heroicons-photo" class="text-purple-400" />
                            <span class="text-sm font-semibold">Screenshots</span>
                            <UBadge v-if="screenshots.length > 0" color="neutral" variant="subtle" size="xs">
                                {{ screenshots.length }}
                            </UBadge>
                        </div>
                        <UButton
                            icon="i-heroicons-camera"
                            color="primary"
                            variant="soft"
                            size="xs"
                            :loading="isTakingScreenshot"
                            :disabled="device?.status !== 'online'"
                            @click="takeScreenshot"
                        >
                            Capture
                        </UButton>
                    </div>
                </template>
                <div class="p-3 h-full">
                    <div
                        v-if="screenshots.length === 0"
                        class="flex flex-col items-center justify-center text-gray-500 h-full"
                    >
                        <UIcon name="i-heroicons-photo" class="w-8 h-8 mb-2 opacity-20" />
                        <span class="text-xs">No screenshots yet</span>
                    </div>
                    <div
                        v-else
                        class="grid grid-cols-3 sm:grid-cols-4 gap-2"
                    >
                        <div
                            v-for="shot in screenshots.slice(0, 8)"
                            :key="shot"
                            class="group relative aspect-video bg-gray-900 rounded overflow-hidden cursor-pointer border border-gray-800 hover:border-primary-500 transition-all"
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
                                <UIcon name="i-heroicons-magnifying-glass-plus" class="text-white w-4 h-4" />
                            </div>
                        </div>
                        <div
                            v-if="screenshots.length > 8"
                            class="aspect-video bg-gray-800/50 rounded flex items-center justify-center text-gray-400 text-xs font-medium cursor-pointer hover:bg-gray-800 transition-colors"
                        >
                            +{{ screenshots.length - 8 }}
                        </div>
                    </div>
                </div>
            </UCard>

            <!-- Connection Logs Card (spans 2 cols on lg) -->
            <UCard class="md:col-span-2 lg:col-span-2 flex flex-col min-h-[200px]" :ui="{ body: 'flex-1 p-0 overflow-hidden' }">
                <template #header>
                    <div class="flex items-center gap-2">
                        <UIcon name="i-heroicons-list-bullet" class="text-green-400" />
                        <span class="text-sm font-semibold">Connection Logs</span>
                    </div>
                </template>
                <div class="flex-1 overflow-y-auto">
                    <UTable :data="logs" :columns="logColumns" :ui="{ td: 'px-3 py-2', th: 'px-3 py-2' }">
                        <template #type-cell="{ row }">
                            <UBadge 
                                :color="row.original.type === 'connect' ? 'success' : row.original.type === 'reconnect' ? 'warning' : 'error'"
                                variant="subtle"
                                size="xs"
                                class="text-[10px]"
                            >
                                {{ row.original.type.toUpperCase() }}
                            </UBadge>
                        </template>
                        <template #timestamp-cell="{ row }">
                            <span class="text-[10px] text-gray-500 font-mono">{{ formatRelativeTime(row.original.timestamp) }}</span>
                        </template>
                    </UTable>
                </div>
            </UCard>
        </div>

        <UModal
            v-model:open="isScreenshotModalOpen"
            :ui="{ 
                body: 'relative text-left rtl:text-right overflow-hidden w-full h-full flex flex-col bg-gray-900 w-full max-w-[95vw] h-[90vh] p-0 rounded-xl',
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


        <!-- Edit Modal -->
        <UModal v-model:open="editModalOpen">
            <template #title>Edit Device</template>
            <template #body>
                <div class="flex flex-col gap-4">
                    <UFormField label="Device Name">
                        <UInput v-model="editForm.name" />
                    </UFormField>
                    <UFormField label="Group (e.g. Office, Lab)">
                        <UInput v-model="editForm.group" placeholder="Enter group name" />
                    </UFormField>
                </div>
            </template>
            <template #footer>
                <div class="flex justify-end gap-2">
                     <UButton color="neutral" variant="ghost" @click="editModalOpen = false">Cancel</UButton>
                     <UButton color="primary" @click="saveDevice">Save</UButton>
                </div>
            </template>
        </UModal>

        <!-- Delete Modal -->
         <UModal v-model:open="deleteModalOpen">
            <template #title>Delete Device</template>
             <template #body>
                <p>Are you sure you want to delete <span class="font-bold text-white">{{ device?.name }}</span>?</p>
                <p class="text-sm text-gray-400 mt-2">This action cannot be undone.</p>
            </template>
            <template #footer>
                <div class="flex justify-end gap-2">
                     <UButton color="neutral" variant="ghost" @click="deleteModalOpen = false">Cancel</UButton>
                     <UButton color="error" @click="deleteDevice">Delete</UButton>
                </div>
            </template>
        </UModal>
    </div>
</template>
