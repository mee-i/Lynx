<script setup lang="ts">
import type { TableColumn } from "@nuxt/ui";
import { authClient } from "~/utils/auth-client";

definePageMeta({
    layout: "dashboard",
    title: "Lynx Dashboard",
});

const { data: session } = await authClient.useSession(useFetch);

interface Device {
    id: string;
    name: string;
    status: "online" | "offline";
    lastSeen: Date;
}

const devices = ref<Device[]>([]);
const loading = ref(true);
const toast = useToast();

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

function copyId() {
    if (session.value?.session.userId) {
        navigator.clipboard.writeText(String(session.value?.session.userId));
        toast.add({
            title: "Copied",
            description: "User ID copied to clipboard",
            color: "success",
        });
    }
}
</script>
<template>
    <div class="grid grid-cols-1 md:grid-cols-4 gap-4">
        <UCard>
            <template #header>
                <h2 class="text-md font-bold">Currently Connected</h2>
            </template>
            <span class="text-2xl font-bold">{{
                devices.filter((d) => d.status === "online").length
            }}</span>
        </UCard>
        <UCard>
            <template #header>
                <h2 class="text-md font-bold">Registered Devices</h2>
            </template>
            <span class="text-2xl font-bold">{{ devices.length }}</span>
        </UCard>
        <UCard class="md:col-span-2">
            <template #header>
                <div class="flex items-center justify-between">
                    <h2 class="text-md font-bold">User Configuration</h2>
                    <UBadge variant="subtle" color="primary"
                        >Linking to App</UBadge
                    >
                </div>
            </template>
            <div class="flex flex-col gap-2">
                <p class="text-sm text-neutral-400">
                    Use this ID in your agent's
                    <code class="text-primary-400">Main.cpp</code>
                    configuration:
                </p>
                <UInput
                    :model-value="session?.session?.userId"
                    readonly
                    class="font-mono"
                >
                    <template #trailing>
                        <UButton
                            icon="i-lucide-copy"
                            variant="ghost"
                            color="neutral"
                            size="xs"
                            class="cursor-pointer"
                            @click="copyId"
                        />
                    </template>
                </UInput>
            </div>
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
