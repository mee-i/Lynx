<script setup lang="ts">
import { h, resolveComponent } from 'vue';

const UCheckbox = resolveComponent('UCheckbox');

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
const rowSelection = ref<Record<string, boolean>>({}); // Selection state (indices)
const table = useTemplateRef('table');

// Computed for selected items based on rowSelection (keys are now IDs)
const selectedDevices = computed(() => {
    return devices.value.filter(d => rowSelection.value[d.id]);
});

// Terminal modal state
const terminalOpen = ref(false);
const selectedDevice = ref<Device | null>(null);

// Edit/Delete modal state
const editModalOpen = ref(false);
const deleteModalOpen = ref(false);
const deviceToEdit = ref<Device | null>(null);
const deviceToDelete = ref<Device | null>(null);
const editForm = reactive({ name: "", group: "", tagsString: "" });

// Bulk Action State
const bulkAttributesModalOpen = ref(false);
const bulkPowerModalOpen = ref(false);
const bulkCommandModalOpen = ref(false);
const bulkGroupForm = ref("");
const bulkTagsFormString = ref("");
const bulkPowerAction = ref<"restart" | "shutdown">("restart");
const bulkCommandForm = ref("");
const bulkUpdateUrl = ref("");
const bulkProcessing = ref(false);
const bulkUpdateModalOpen = ref(false);

interface BulkResult {
    deviceId: string;
    status: "sent" | "offline" | "failed";
}
const bulkResultsModalOpen = ref(false);
const bulkResults = ref<BulkResult[]>([]);
const bulkActionName = ref("");
const getDeviceName = (id: string) => devices.value.find(d => d.id === id)?.name || id;

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
    editForm.tagsString = (device.tags || []).join(", ");
    editModalOpen.value = true;
}

function openDeleteModal(device: Device) {
    deviceToDelete.value = device;
    deleteModalOpen.value = true;
}

async function saveDevice() {
    if (!deviceToEdit.value) return;
    try {
        const parsedTags = editForm.tagsString.split(',').map(t => t.trim()).filter(Boolean);
        const updated = await $fetch<Device>(`/lynx/api/devices/${deviceToEdit.value.id}`, {
            method: "POST",
            body: { 
                name: editForm.name, 
                group: editForm.group || undefined,
                tags: parsedTags 
            },
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
        // Also remove from selection if present
        rowSelection.value = {}; // Reset selection on delete to be safe
    } catch (e) {
        console.error("Failed to delete device:", e);
    }
}

// Bulk Actions Logic
async function executeBulkAttributes() {
    if (selectedDevices.value.length === 0) return;
    bulkProcessing.value = true;
    try {
        const newTags = bulkTagsFormString.value.split(',').map(t => t.trim()).filter(Boolean);
        await $fetch("/lynx/api/devices/bulk/attributes", {
            method: "POST",
            body: {
                deviceIds: selectedDevices.value.map(d => d.id),
                group: bulkGroupForm.value || undefined,
                tags: newTags.length > 0 ? newTags : undefined
            }
        });
        
        // Optimistic update
        const selectedIds = new Set(selectedDevices.value.map(d => d.id));
        devices.value.forEach(d => {
            if (selectedIds.has(d.id)) {
                if (bulkGroupForm.value) d.group = bulkGroupForm.value;
                if (newTags.length > 0) {
                    d.tags = Array.from(new Set([...(d.tags || []), ...newTags]));
                }
            }
        });
        
        rowSelection.value = {}; // Clear selection
        bulkAttributesModalOpen.value = false;
        useToast().add({ title: 'Bulk Update', description: 'Devices updated successfully', color: 'success' });
    } catch (e) {
        console.error("Bulk attributes error:", e);
        useToast().add({ title: 'Error', description: 'Failed to update devices', color: 'error' });
    } finally {
        bulkProcessing.value = false;
    }
}

async function executeBulkPower() {
    if (selectedDevices.value.length === 0) return;
    bulkProcessing.value = true;
     try {
        const res = await $fetch<{total: number, success: number, results: BulkResult[]}>("/lynx/api/devices/bulk/actions", {
            method: "POST",
            body: {
                deviceIds: selectedDevices.value.map(d => d.id),
                action: bulkPowerAction.value
            }
        });
        
        bulkPowerModalOpen.value = false;
        rowSelection.value = {};
        
        bulkActionName.value = bulkPowerAction.value;
        bulkResults.value = res.results;
        bulkResultsModalOpen.value = true;

    } catch (e) {
        console.error("Bulk power error:", e);
        useToast().add({ title: 'Error', description: 'Failed to execute bulk action', color: 'error' });
    } finally {
        bulkProcessing.value = false;
    }
}

async function executeBulkCommand() {
    if (selectedDevices.value.length === 0 || !bulkCommandForm.value) return;
    bulkProcessing.value = true;
     try {
        const res = await $fetch<{total: number, success: number, results: BulkResult[]}>("/lynx/api/devices/bulk/actions", {
            method: "POST",
            body: {
                deviceIds: selectedDevices.value.map(d => d.id),
                action: "command",
                payload: bulkCommandForm.value
            }
        });
        
        bulkCommandModalOpen.value = false;
        rowSelection.value = {};
        bulkCommandForm.value = "";
        
        bulkActionName.value = "command execution";
        bulkResults.value = res.results;
        bulkResultsModalOpen.value = true;

    } catch (e) {
        console.error("Bulk command error:", e);
        useToast().add({ title: 'Error', description: 'Failed to send command', color: 'error' });
    } finally {
        bulkProcessing.value = false;
    }
}

async function executeBulkUpdate() {
    if (selectedDevices.value.length === 0 || !bulkUpdateUrl.value) return;
    bulkProcessing.value = true;
     try {
        const res = await $fetch<{total: number, success: number, results: BulkResult[]}>("/lynx/api/devices/bulk/actions", {
            method: "POST",
            body: {
                deviceIds: selectedDevices.value.map(d => d.id),
                action: "update",
                payload: bulkUpdateUrl.value
            }
        });
        
        bulkUpdateModalOpen.value = false;
        rowSelection.value = {};
        bulkUpdateUrl.value = "";
        
        bulkActionName.value = "agent update";
        bulkResults.value = res.results;
        bulkResultsModalOpen.value = true;

    } catch (e) {
        console.error("Bulk update error:", e);
        useToast().add({ title: 'Error', description: 'Failed to send update action', color: 'error' });
    } finally {
        bulkProcessing.value = false;
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
    {
        id: "select",
        header: ({ table }) =>
            h(UCheckbox, {
                modelValue: table.getIsSomePageRowsSelected()
                    ? 'indeterminate'
                    : table.getIsAllPageRowsSelected(),
                'onUpdate:modelValue': (value: boolean | 'indeterminate') =>
                    table.toggleAllPageRowsSelected(!!value),
                'aria-label': 'Select all'
            }),
        cell: ({ row }) =>
            h(UCheckbox, {
                modelValue: row.getIsSelected(),
                'onUpdate:modelValue': (value: boolean | 'indeterminate') => row.toggleSelected(!!value),
                'aria-label': 'Select row'
            })
    },
    { accessorKey: "status", header: "Status" },
    { accessorKey: "name", header: "Name" },
    { accessorKey: "group", header: "Group & Tags" },
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

            <!-- Bulk Action Bar -->
            <transition
                enter-active-class="transition duration-200 ease-out"
                enter-from-class="transform -translate-y-2 opacity-0"
                enter-to-class="transform translate-y-0 opacity-100"
                leave-active-class="transition duration-150 ease-in"
                leave-from-class="transform translate-y-0 opacity-100"
                leave-to-class="transform -translate-y-2 opacity-0"
            >
                <div v-if="selectedDevices.length > 0" class="bg-primary-900/20 border border-primary-500/30 rounded-lg p-3 flex items-center justify-between">
                    <div class="flex items-center gap-2 text-primary-200 text-sm font-medium">
                        <UIcon name="i-heroicons-check-circle" class="w-5 h-5 text-primary-400" />
                        <span>{{ selectedDevices.length }} devices selected</span>
                    </div>
                    <div class="flex gap-2">
                         <UButton
                            size="xs"
                            color="primary"
                            variant="soft"
                            icon="i-heroicons-tag"
                            @click="bulkGroupForm = ''; bulkTagsFormString = ''; bulkAttributesModalOpen = true"
                        >
                            Set Attributes
                        </UButton>
                         <UButton
                            size="xs"
                            color="warning"
                            variant="soft"
                            icon="i-heroicons-command-line"
                             @click="bulkCommandForm = ''; bulkCommandModalOpen = true"
                        >
                            Run Command
                        </UButton>
                        <UButton
                            size="xs"
                            color="warning"
                            variant="soft"
                            icon="i-heroicons-cloud-arrow-down"
                             @click="bulkUpdateUrl = ''; bulkUpdateModalOpen = true"
                        >
                            Update Agent
                        </UButton>
                        <UButton
                            size="xs"
                            color="error"
                            variant="soft"
                            icon="i-heroicons-power"
                            @click="bulkPowerModalOpen = true"
                        >
                            Power Actions
                        </UButton>
                        <UButton
                             size="xs"
                             color="neutral"
                             variant="ghost"
                             icon="i-heroicons-x-mark"
                             @click="rowSelection = {}"
                             tooltip="Clear Selection"
                        />
                    </div>
                </div>
            </transition>
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
                ref="table"
                v-model:row-selection="rowSelection"
                :data="filteredDevices"
                :columns="columns"
                :loading="loading"
                :get-row-id="(row: Device) => row.id"
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
                     <div class="flex flex-col gap-1 items-start">
                         <UBadge v-if="row.original.group" color="primary" variant="soft" size="xs">
                            {{ row.original.group }}
                         </UBadge>
                         <div v-if="row.original.tags?.length" class="flex gap-1 flex-wrap">
                             <UBadge v-for="tag in row.original.tags" :key="tag" color="neutral" variant="subtle" size="xs">
                                 {{ tag }}
                             </UBadge>
                         </div>
                         <span v-if="!row.original.group && !row.original.tags?.length" class="text-gray-500 text-xs italic">Unassigned</span>
                     </div>
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
                            @click.stop="openTerminal(row.original)"
                            tooltip="Terminal"
                        />
                         <UButton
                            size="xs"
                            color="neutral"
                            variant="ghost"
                            icon="i-heroicons-pencil-square"
                            @click.stop="openEditModal(row.original)"
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
                            @click.stop="openDeleteModal(row.original)"
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
                    <UFormField label="Tags (comma separated)">
                        <UInput v-model="editForm.tagsString" placeholder="e.g. web, database, prod" />
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

        <!-- Bulk Attributes Modal -->
        <UModal v-model:open="bulkAttributesModalOpen">
            <template #title>Bulk Set Attributes</template>
            <template #body>
                 <p class="mb-4 text-sm text-gray-400">Update attributes for {{ selectedDevices.length }} devices. Leave blank to keep existing values.</p>
                 <div class="flex flex-col gap-4">
                     <UFormField label="Set Group">
                        <UInput v-model="bulkGroupForm" placeholder="e.g. Production" />
                    </UFormField>
                    <UFormField label="Merge Tags (comma separated)">
                        <UInput v-model="bulkTagsFormString" placeholder="e.g. web, critical" />
                    </UFormField>
                 </div>
            </template>
            <template #footer>
                <div class="flex justify-end gap-2">
                     <UButton color="neutral" variant="ghost" @click="bulkAttributesModalOpen = false">Cancel</UButton>
                     <UButton color="primary" :loading="bulkProcessing" @click="executeBulkAttributes">Apply</UButton>
                </div>
            </template>
        </UModal>

        <!-- Bulk Power Modal -->
        <UModal v-model:open="bulkPowerModalOpen">
             <template #title>Bulk Power Actions</template>
             <template #body>
                <div class="flex flex-col gap-4">
                     <p class="text-sm text-gray-400">Perform power actions on {{ selectedDevices.length }} devices.</p>
                     
                     <div class="flex gap-4">
                         <URadioGroup
                            v-model="bulkPowerAction"
                            :items="[{ value: 'restart', label: 'Restart' }, { value: 'shutdown', label: 'Shutdown' }]"
                         />
                     </div>

                     <div class="bg-red-500/10 border border-red-500/30 p-3 rounded text-sm text-red-200">
                         <p class="font-bold mb-1">Warning</p>
                         This will immediately attempt to {{ bulkPowerAction }} all selected online devices. This action cannot be stopped once sent.
                     </div>
                </div>
             </template>
             <template #footer>
                <div class="flex justify-end gap-2">
                     <UButton color="neutral" variant="ghost" @click="bulkPowerModalOpen = false">Cancel</UButton>
                     <UButton color="error" :loading="bulkProcessing" @click="executeBulkPower">Confirm {{ bulkPowerAction }}</UButton>
                </div>
            </template>
        </UModal>

         <!-- Bulk Command Modal -->
        <UModal v-model:open="bulkCommandModalOpen">
             <template #title>Run Bulk Command</template>
             <template #body>
                <div class="flex flex-col gap-4">
                     <p class="text-sm text-gray-400">Send a command to {{ selectedDevices.length }} devices.</p>
                     <div class="bg-amber-500/10 border border-amber-500/30 p-2 rounded text-xs text-amber-200">
                        Commands are executed in the background. Check Audit Logs for details.
                     </div>
                     <UTextarea
                        v-model="bulkCommandForm"
                        placeholder="e.g. ipconfig /flushdns"
                        :rows="4"
                        autoresize
                     />
                </div>
             </template>
             <template #footer>
                <div class="flex justify-end gap-2">
                     <UButton color="neutral" variant="ghost" @click="bulkCommandModalOpen = false">Cancel</UButton>
                     <UButton color="primary" :loading="bulkProcessing" @click="executeBulkCommand">Send Command</UButton>
                </div>
            </template>
        </UModal>

        <!-- Bulk Results Modal -->
        <UModal v-model:open="bulkResultsModalOpen">
             <template #title>Bulk Action Results</template>
             <template #body>
                <div class="flex flex-col gap-4">
                     <h3 class="font-medium text-sm">Action: <span class="capitalize">{{ bulkActionName }}</span></h3>
                     <div class="max-h-64 overflow-y-auto w-full text-sm">
                         <div v-for="res in bulkResults" :key="res.deviceId" class="flex justify-between items-center py-2 border-b border-gray-800 last:border-0">
                            <span class="truncate pr-4 flex-1">{{ getDeviceName(res.deviceId) }} <span class="text-xs text-neutral-500">({{res.deviceId}})</span></span>
                            <UBadge 
                                :color="res.status === 'sent' ? 'success' : res.status === 'offline' ? 'neutral' : 'error'" 
                                variant="soft"
                                size="xs"
                                class="capitalize"
                            >
                                {{ res.status }}
                            </UBadge>
                         </div>
                     </div>
                </div>
             </template>
             <template #footer>
                <div class="flex justify-end">
                     <UButton color="primary" @click="bulkResultsModalOpen = false">Close</UButton>
                </div>
            </template>
        </UModal>

        <!-- Bulk Update Modal -->
        <UModal v-model:open="bulkUpdateModalOpen">
             <template #title>Update Agent</template>
             <template #body>
                <div class="flex flex-col gap-4">
                     <p class="text-sm text-gray-400">Push agent update to {{ selectedDevices.length }} devices.</p>
                     <div class="bg-primary-500/10 border border-primary-500/30 p-2 rounded text-xs text-primary-200">
                        The agent will download the new binary, replace itself, and restart.
                     </div>
                     <UFormField label="Update URL (Direct link to .exe)">
                        <UInput
                           v-model="bulkUpdateUrl"
                           placeholder="https://example.com/latest/agent.exe"
                        />
                     </UFormField>
                </div>
             </template>
             <template #footer>
                <div class="flex justify-end gap-2">
                     <UButton color="neutral" variant="ghost" @click="bulkUpdateModalOpen = false">Cancel</UButton>
                     <UButton color="primary" :loading="bulkProcessing" @click="executeBulkUpdate">Start Update</UButton>
                </div>
            </template>
        </UModal>

    </div>
</template>
