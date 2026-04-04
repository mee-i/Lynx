<script setup lang="ts">
import { ref, onMounted, computed, watch } from 'vue';
import CctvCell from '~/components/device/CctvCell.vue';

definePageMeta({
    layout: "dashboard",
    title: "CCTV Monitoring",
});

interface Device {
    id: string;
    name: string;
    status: "online" | "offline";
    lastSeen: Date;
}

const allDevices = ref<Device[]>([]);
const activeDeviceIds = ref<string[]>([]);
const isAddDeviceModalOpen = ref(false);
const searchQuery = ref("");

// Grid Configuration
const gridCols = ref(2);
const gridRows = ref(2);

const cctvContainerRef = ref<HTMLElement | null>(null);
const isGlobalFullscreen = ref(false);

async function fetchDevices() {
    try {
        const res = await $fetch<Device[]>("/lynx/api/devices");
        allDevices.value = res;
    } catch (err) {
        console.error("Failed to fetch devices:", err);
    }
}

const onlineDevices = computed(() => {
    return allDevices.value.filter(d => d.status === 'online' && !activeDeviceIds.value.includes(d.id));
});

const filteredOnlineDevices = computed(() => {
    if (!searchQuery.value) return onlineDevices.value;
    return onlineDevices.value.filter(d => 
        d.name.toLowerCase().includes(searchQuery.value.toLowerCase()) || 
        d.id.toLowerCase().includes(searchQuery.value.toLowerCase())
    );
});

const activeDevices = computed(() => {
    return activeDeviceIds.value.map(id => {
        const device = allDevices.value.find(d => d.id === id);
        return { 
            id, 
            name: device?.name || 'Unknown Device' 
        };
    });
});

function addDevice(id: string) {
    if (!activeDeviceIds.value.includes(id)) {
        activeDeviceIds.value.push(id);
        saveState();
    }
}

function removeDevice(id: string) {
    activeDeviceIds.value = activeDeviceIds.value.filter(d => d !== id);
    saveState();
}

function saveState() {
    if (typeof window === 'undefined') return;
    localStorage.setItem('cctv_active_devices', JSON.stringify(activeDeviceIds.value));
}

function loadState() {
    if (typeof window === 'undefined') return;
    const saved = localStorage.getItem('cctv_active_devices');
    if (saved) {
        try {
            activeDeviceIds.value = JSON.parse(saved);
        } catch (e) {
            console.error("Failed to load CCTV state:", e);
        }
    }
    const savedCols = localStorage.getItem('cctv_grid_cols');
    if (savedCols) {
        gridCols.value = Number(savedCols);
    }
    const savedRows = localStorage.getItem('cctv_grid_rows');
    if (savedRows) {
        gridRows.value = Number(savedRows);
    }
}

function clearAll() {
    activeDeviceIds.value = [];
    saveState();
}

onMounted(() => {
    loadState();
    fetchDevices();
    document.addEventListener('fullscreenchange', () => {
        isGlobalFullscreen.value = !!document.fullscreenElement && document.fullscreenElement === cctvContainerRef.value;
    });
});

function toggleGlobalFullscreen() {
    if (!cctvContainerRef.value) return;
    if (!document.fullscreenElement) {
        cctvContainerRef.value.requestFullscreen();
    } else {
        document.exitFullscreen();
    }
}

watch(gridCols, (val) => {
    if (typeof window !== 'undefined') {
        localStorage.setItem('cctv_grid_cols', val.toString());
    }
});

watch(gridRows, (val) => {
    if (typeof window !== 'undefined') {
        localStorage.setItem('cctv_grid_rows', val.toString());
    }
});
</script>

<template>
    <div class="h-full flex flex-col gap-4">
        <!-- Header -->
        <div class="flex items-center justify-between">
            <div class="flex items-center gap-4">
                <h1 class="text-2xl font-bold text-primary">CCTV Monitoring</h1>
                <UBadge color="success" variant="subtle">{{ activeDeviceIds.length }} Active Streams</UBadge>
            </div>
            
            <div class="flex items-center gap-3">
                <!-- Grid Adjuster -->
                <div class="flex items-center gap-2 bg-neutral-900/50 px-3 py-1.5 rounded-lg border border-neutral-800">
                    <span class="text-[10px] uppercase tracking-widest text-neutral-500 font-bold">Cols</span>
                    <USlider v-model="gridCols" :min="1" :max="4" :step="1" size="xs" class="w-16" color="primary" />
                    <div class="w-px h-3 bg-neutral-800 mx-1"></div>
                    <span class="text-[10px] uppercase tracking-widest text-neutral-500 font-bold">Rows</span>
                    <USlider v-model="gridRows" :min="1" :max="4" :step="1" size="xs" class="w-16" color="primary" />
                </div>

                <UButton 
                    :icon="isGlobalFullscreen ? 'i-heroicons-arrows-pointing-in' : 'i-heroicons-arrows-pointing-out'" 
                    color="neutral" 
                    variant="soft" 
                    @click="toggleGlobalFullscreen"
                >
                    {{ isGlobalFullscreen ? 'Exit Full Screen' : 'Full Screen' }}
                </UButton>

                <UButton icon="i-heroicons-plus" color="primary" @click="isAddDeviceModalOpen = true">Add Device</UButton>
                <UButton icon="i-heroicons-trash" variant="soft" color="error" @click="clearAll" v-if="activeDeviceIds.length > 0">Clear All</UButton>
            </div>
        </div>

        <!-- The Grid -->
        <div 
            ref="cctvContainerRef"
            class="flex-1 grid gap-4 auto-rows-fr p-2 overflow-auto"
            :style="{ 
                gridTemplateColumns: `repeat(${gridCols}, minmax(0, 1fr))`,
                gridTemplateRows: `repeat(${gridRows}, minmax(0, 1fr))` 
            }"
        >
            <CctvCell 
                v-for="device in activeDevices" 
                :key="device.id" 
                :device-id="device.id" 
                :device-name="device.name"
                @remove="removeDevice(device.id)"
            />

            <!-- Empty State -->
            <div 
                v-if="activeDeviceIds.length === 0" 
                class="col-span-full flex flex-col items-center justify-center p-20 border-2 border-dashed border-neutral-800 rounded-2xl bg-neutral-900/10"
            >
                <UIcon name="i-heroicons-video-camera" class="w-16 h-16 mb-4 text-neutral-800" />
                <h2 class="text-xl font-bold text-neutral-500 mb-2">No Devices Selected</h2>
                <p class="text-neutral-600 mb-6">Start monitoring by adding online devices to your grid.</p>
                <UButton icon="i-heroicons-plus" size="lg" @click="isAddDeviceModalOpen = true">Select Devices</UButton>
            </div>
        </div>

        <!-- Add Device Modal -->
        <UModal v-model:open="isAddDeviceModalOpen" :close="{ }">
                <template #header>
                    <div class="flex items-center justify-between">
                        <h3 class="text-lg font-bold">Add Online Devices</h3>
                    </div>
                </template>

                <template #body>
                    <div class="flex flex-col gap-4 max-h-[400px] overflow-y-auto p-1">
                        <UInput
                            v-model="searchQuery"
                            icon="i-heroicons-magnifying-glass"
                            placeholder="Search devices..."
                            block
                        />
    
                        <div v-if="filteredOnlineDevices.length === 0" class="text-center py-10 text-neutral-500 italic">
                            No online devices found.
                        </div>
    
                        <div 
                            v-for="device in filteredOnlineDevices" 
                            :key="device.id"
                            class="flex items-center justify-between p-3 rounded-lg border border-neutral-800 bg-neutral-950/50 hover:border-primary/50 transition-colors"
                        >
                            <div class="flex flex-col">
                                <span class="font-bold text-sm">{{ device.name }}</span>
                                <code class="text-[10px] text-neutral-600">{{ device.id }}</code>
                            </div>
                            <UButton icon="i-heroicons-plus" size="xs" variant="soft" @click="addDevice(device.id)">Add</UButton>
                        </div>
                    </div>
                </template>

                <template #footer>
                    <div class="flex justify-end">
                        <UButton color="neutral" @click="isAddDeviceModalOpen = false">Done</UButton>
                    </div>
                </template>
        </UModal>
    </div>
</template>

<style scoped>
/* Ensure the grid takes up available space and distributes evenly */
.grid {
    grid-auto-flow: dense;
}

/* Fullscreen styling to prevent black background and ensure proper layout */
div:fullscreen {
    width: 100vw;
    height: 100vh;
    background-color: #09090b; /* Match neutral-950 */
    padding: 1rem;
    overflow-y: auto;
}
</style>
