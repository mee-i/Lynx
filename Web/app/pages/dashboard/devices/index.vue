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
    group?: string;
    tags?: string[];
    os?: string;
    version?: string;
}

const devices = ref<Device[]>([]);
const loading = ref(true);
const search = ref("");
const sortBy = ref<"status" | "name" | "lastSeen">("status");

// Terminal modal state
const terminalOpen = ref(false);
const selectedDevice = ref<Device | null>(null);

// Edit/Delete modal state
const editModalOpen = ref(false);
const deleteModalOpen = ref(false);
const deviceToEdit = ref<Device | null>(null);
const deviceToDelete = ref<Device | null>(null);
const editForm = reactive({ name: "", group: "" });

const filteredDevices = computed(() => {
    let result = [...devices.value];

    if (search.value) {
        const lowerSearch = search.value.toLowerCase();
        result = result.filter(
            (d) =>
                d.name.toLowerCase().includes(lowerSearch) ||
                d.id.toLowerCase().includes(lowerSearch) ||
                d.group?.toLowerCase().includes(lowerSearch) ||
                d.os?.toLowerCase().includes(lowerSearch)
        );
    }

    result.sort((a, b) => {
        if (sortBy.value === "status") {
            if (a.status === b.status) return 0;
            return a.status === "online" ? -1 : 1;
        } else if (sortBy.value === "name") {
            return a.name.localeCompare(b.name);
        } else if (sortBy.value === "lastSeen") {
            return new Date(b.lastSeen).getTime() - new Date(a.lastSeen).getTime();
        }
        return 0;
    });

    return result;
});

function openTerminal(device: Device) {
    selectedDevice.value = device;
    terminalOpen.value = true;
}

function openEditModal(device: Device) {
    deviceToEdit.value = device;
    editForm.name = device.name;
    editForm.group = device.group || "";
    editModalOpen.value = true;
}

function openDeleteModal(device: Device) {
    deviceToDelete.value = device;
    deleteModalOpen.value = true;
}

async function saveDevice() {
    if (!deviceToEdit.value) return;
    try {
        const updated = await $fetch<Device>(`/lynx/api/devices/${deviceToEdit.value.id}`, {
            method: "POST",
            body: { name: editForm.name, group: editForm.group || undefined },
        });
        // Update local list
        const index = devices.value.findIndex((d) => d.id === updated.id);
        if (index !== -1) {
            devices.value[index] = { ...updated, lastSeen: new Date(updated.lastSeen) };
        }
        editModalOpen.value = false;
    } catch (e) {
        console.error("Failed to update device:", e);
    }
}

async function deleteDevice() {
    if (!deviceToDelete.value) return;
    try {
        await $fetch(`/lynx/api/devices/${deviceToDelete.value.id}`, {
            method: "DELETE",
        });
        devices.value = devices.value.filter((d) => d.id !== deviceToDelete.value?.id);
        deleteModalOpen.value = false;
    } catch (e) {
        console.error("Failed to delete device:", e);
    }
}

async function fetchDevices() {
    loading.value = true;
    try {
        const res = await $fetch<Device[]>("/lynx/api/devices");
        devices.value = res.map(d => ({...d, lastSeen: new Date(d.lastSeen)}));
    } catch (err) {
        console.error("Failed to fetch devices:", err);
    } finally {
        loading.value = false;
    }
}

const columns: TableColumn<Device>[] = [
    { accessorKey: "status", header: "Status" },
    { accessorKey: "name", header: "Name" },
    { accessorKey: "group", header: "Group" },
    { accessorKey: "os", header: "OS/Version" },
    { accessorKey: "lastSeen", header: "Last Seen" },
    { id: "actions", header: "" },
];

function formatDate(date: Date) {
    return date.toLocaleString();
}

onMounted(() => {
    fetchDevices();
});
</script>

<template>
    <div>
        <div class="flex flex-col gap-4 mb-6">
            <div class="flex justify-between items-center">
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
            
            <div class="flex gap-4 items-center">
                <UInput
                    v-model="search"
                    icon="i-heroicons-magnifying-glass"
                    placeholder="Search devices..."
                    class="flex-1 max-w-sm"
                />
                <USelect
                    v-model="sortBy"
                    :items="[
                        { label: 'Status (Online First)', value: 'status' },
                        { label: 'Name (A-Z)', value: 'name' },
                        { label: 'Last Seen (Newest)', value: 'lastSeen' }
                    ]"
                    placeholder="Sort by"
                    class="w-48"
                />
            </div>
        </div>

        <UCard :ui="{ body: 'p-0' }">
            <template v-if="filteredDevices.length === 0 && !loading">
                <div class="text-center py-12 text-gray-500">
                    <p class="text-lg">No devices found</p>
                    <p v-if="search" class="text-sm mt-2">Try adjusting your search query</p>
                    <p v-else class="text-sm mt-2">
                        Run the Lynx App on a device to register it automatically
                    </p>
                </div>
            </template>

            <UTable
                v-else
                :data="filteredDevices"
                :columns="columns"
                :loading="loading"
                class="w-full"
            >
                <template #status-cell="{ row }">
                    <UBadge
                        :color="row.original.status === 'online' ? 'success' : 'neutral'"
                        variant="subtle"
                        size="sm"
                        class="capitalize"
                    >
                        {{ row.original.status }}
                    </UBadge>
                </template>
                
                <template #name-cell="{ row }">
                    <div class="flex flex-col">
                        <span class="font-medium text-white">{{ row.original.name }}</span>
                        <code class="text-xs text-gray-500">{{ row.original.id }}</code>
                    </div>
                </template>

                <template #group-cell="{ row }">
                     <UBadge v-if="row.original.group" color="primary" variant="soft" size="xs">
                        {{ row.original.group }}
                     </UBadge>
                     <span v-else class="text-gray-500 text-xs italic">None</span>
                </template>

                <template #os-cell="{ row }">
                    <div class="flex flex-col text-xs" v-if="row.original.os">
                        <span class="text-gray-300">{{ row.original.os }}</span>
                        <span class="text-gray-500" v-if="row.original.version">v{{ row.original.version }}</span>
                    </div>
                    <span v-else class="text-gray-500 text-xs italic">Unknown</span>
                </template>

                <template #lastSeen-cell="{ row }">
                    <div class="text-sm text-gray-400">
                        {{ formatDate(row.original.lastSeen) }}
                    </div>
                </template>

                <template #actions-cell="{ row }">
                    <div class="flex gap-2 items-center justify-end">
                        <UButton
                            size="xs"
                            color="primary"
                            variant="soft"
                            icon="i-heroicons-command-line"
                            :disabled="row.original.status !== 'online'"
                            @click="openTerminal(row.original)"
                            tooltip="Terminal"
                        />
                         <UButton
                            size="xs"
                            color="neutral"
                            variant="ghost"
                            icon="i-heroicons-pencil-square"
                            @click="openEditModal(row.original)"
                            tooltip="Edit"
                        />
                        <UButton
                            size="xs"
                            color="neutral"
                            variant="ghost"
                            icon="i-heroicons-information-circle"
                            :to="`/dashboard/devices/${row.original.id}`"
                            tooltip="Info"
                        />
                        <UButton
                            size="xs"
                            color="error"
                            variant="ghost"
                            icon="i-heroicons-trash"
                            @click="openDeleteModal(row.original)"
                            tooltip="Delete"
                        />
                    </div>
                </template>
            </UTable>
        </UCard>

        <!-- Terminal Modal -->
        <TerminalModal v-model="terminalOpen" :device="selectedDevice" />

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
                <p>Are you sure you want to delete <span class="font-bold text-white">{{ deviceToDelete?.name }}</span>?</p>
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
