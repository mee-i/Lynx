<script setup lang="ts">
import type { TableColumn } from "@nuxt/ui";

definePageMeta({
    layout: "dashboard",
});
interface Device {
    id: string;
    name: string;
    status: "online" | "offline";
    lastSeen: Date;
}

const devices = ref<Device[]>([]);
const loading = ref(true);

// Terminal modal state
const terminalOpen = ref(false);
const selectedDevice = ref<Device | null>(null);

function openTerminal(device: Device) {
    selectedDevice.value = device;
    terminalOpen.value = true;
}

async function fetchDevices() {
    loading.value = true;
    try {
        // Fetch from Server directly (not Nuxt API)
        const res = await $fetch<Device[]>("/lynx/api/devices");
        devices.value = res;
    } catch (err) {
        console.error("Failed to fetch devices:", err);
    } finally {
        loading.value = false;
    }
}

const columns: TableColumn<Device>[] = [
    {
        accessorKey: "name",
        header: "Name",
    },
    {
        accessorKey: "status",
        header: "Status",
    },
    {
        accessorKey: "lastSeen",
        header: "Last Seen",
    },
    {
        id: "actions",
        header: "",
    },
];

function formatDate(dateString: string | Date | null) {
    if (!dateString) return "Never";
    return new Date(dateString).toLocaleString();
}

onMounted(() => {
    fetchDevices();
});
</script>

<template>
    <div>
        <div class="flex justify-between items-center mb-6">
            <h1 class="text-2xl font-bold">Devices</h1>
            <UButton
                icon="i-heroicons-arrow-path"
                variant="ghost"
                :loading="loading"
                @click="fetchDevices"
            >
                Refresh
            </UButton>
        </div>

        <UCard>
            <template v-if="devices.length === 0 && !loading">
                <div class="text-center py-8 text-gray-500">
                    <p class="text-lg">No devices connected</p>
                    <p class="text-sm mt-2">
                        Run the Lynx App on a device to register it
                        automatically
                    </p>
                </div>
            </template>

            <UTable
                v-else
                :data="devices"
                :columns="columns"
                :loading="loading"
            >
                <template #lastSeen-cell="{ row }">
                    {{ formatDate(row.original.lastSeen) }}
                </template>
                <template #status-cell="{ row }">
                    <UBadge
                        :color="
                            row.original.status === 'online'
                                ? 'success'
                                : 'neutral'
                        "
                        variant="subtle"
                    >
                        {{ row.original.status }}
                    </UBadge>
                </template>
                <template #actions-cell="{ row }">
                    <div class="flex gap-2 items-center">
                        <UButton
                            size="xs"
                            color="primary"
                            variant="solid"
                            icon="i-lucide-terminal"
                            :disabled="row.original.status !== 'online'"
                            @click="openTerminal(row.original)"
                        >
                            Terminal
                        </UButton>
                        <UModal>
                            <UButton
                                size="xs"
                                color="neutral"
                                variant="solid"
                                icon="i-lucide-settings"
                            >
                                Settings
                            </UButton>

                            <template #header="{close}">
                                <div class="flex items-center justify-between gap-2 w-full">
                                    <h2 class="text-lg font-bold">Settings</h2>
                                    <UButton icon="i-lucide-x" variant="ghost" @click="close" />
                                </div>
                            </template>
                            <template #footer="{close}">
                                <UButton color="success" variant="solid" @click="close">
                                    Save
                                </UButton>
                            </template>
                        </UModal>
                        <!-- <code class="text-xs text-gray-500 select-all">{{
                            row.original.id
                        }}</code> -->
                    </div>
                </template>
            </UTable>
        </UCard>

        <!-- Terminal Modal -->
        <TerminalModal v-model="terminalOpen" :device="selectedDevice" />
    </div>
</template>
