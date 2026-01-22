<script setup lang="ts">
import type { TableColumn } from "@nuxt/ui";

definePageMeta({
    layout: "dashboard",
    title: "Lynx Dashboard",
});

interface Device {
    id: string;
    name: string;
    status: "online" | "offline";
    lastSeen: Date;
}

const devices = ref<Device[]>([]);
const loading = ref(true);


function formatDate(dateString: string | Date | null) {
    if (!dateString) return "Never";
    return new Date(dateString).toLocaleString();
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

onMounted(() => {
    fetchDevices();
});
</script>
<template>
    <div class="grid grid-cols-1 md:grid-cols-4 gap-4">
        <UCard>
            <template #header>
                <h2 class="text-md font-bold">Currently Connected</h2>
            </template>
            <span>1</span>
        </UCard>
        <UCard>
            <template #header>
                <h2 class="text-md font-bold">Registered Devices</h2>
            </template>
            <span>2</span>
        </UCard>
    </div>

    <UCard class="mt-4">
        <template #header>
            <div class="flex flex-row items-center gap-3">
                <h2 class="text-md font-bold">Devices</h2>
                <UButton
                    icon="i-lucide-arrow-up-right"
                    class="cursor-pointer"
                    variant="soft"
                    size="sm"
                    to="/dashboard/devices"
                />
            </div>
        </template>
        <UTable :data="devices" :columns="columns">
            <template #lastSeen-cell="{ row }">
                {{ formatDate(row.original.lastSeen) }}
            </template>
            <template #status-cell="{ row }">
                <UBadge
                    :color="
                        row.original.status === 'online' ? 'success' : 'neutral'
                    "
                    variant="subtle"
                >
                    {{ row.original.status }}
                </UBadge>
            </template>
        </UTable>
    </UCard>
</template>
