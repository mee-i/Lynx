<script setup lang="ts">
import type { TableColumn } from "@nuxt/ui";
import { authClient } from "~/utils/auth-client";

definePageMeta({
    layout: "dashboard",
    title: "Audit Logs",
});

const { data: session } = await authClient.useSession(useFetch);
const isAdmin = computed(() => (session.value?.user as any)?.role === "admin");

interface AuditLog {
    id: number;
    userId: string;
    userName: string;
    action: string;
    deviceId?: string;
    payload?: string;
    ipAddress?: string;
    timestamp: string;
}

const loading = ref(true);
const logs = ref<AuditLog[]>([]);
const actions = ref<string[]>([]);
const page = ref(1);
const limit = ref(20);
const total = ref(0);

const filters = reactive({
    userId: "",
    action: "",
    startDate: undefined as Date | undefined,
    endDate: undefined as Date | undefined,
});

const columns: TableColumn<AuditLog>[] = [
    { accessorKey: "userName", header: "User" },
    { accessorKey: "action", header: "Action" },
    { accessorKey: "payload", header: "Details" },
    { accessorKey: "ipAddress", header: "IP Address" },
    { accessorKey: "timestamp", header: "Time" },
];

async function fetchLogs() {
    loading.value = true;
    try {
        const res = await $fetch<any>("/api/audit-logs", {
            query: {
                page: page.value,
                limit: limit.value,
                userId: filters.userId || undefined,
                action: filters.action || undefined,
                startDate: filters.startDate?.toISOString(),
                endDate: filters.endDate?.toISOString(),
            },
        });
        logs.value = res.data;
        total.value = res.pagination.total;
        actions.value = res.actions;
    } catch (err) {
        console.error("Failed to fetch audit logs:", err);
    } finally {
        loading.value = false;
    }
}

function exportCsv() {
    const query = new URLSearchParams();
    if (filters.userId) query.append("userId", filters.userId);
    if (filters.action) query.append("action", filters.action);
    if (filters.startDate)
        query.append("startDate", filters.startDate.toISOString());
    if (filters.endDate) query.append("endDate", filters.endDate.toISOString());

    window.open(`/api/audit-logs/export?${query.toString()}`, "_blank");
}

function resetFilters() {
    filters.userId = "";
    filters.action = "";
    filters.startDate = undefined;
    filters.endDate = undefined;
    page.value = 1;
    fetchLogs();
}

const isClearModalOpen = ref(false);
const clearing = ref(false);

async function clearLogs() {
    clearing.value = true;
    try {
        await $fetch("/api/audit-logs", {
            method: "DELETE",
        });
        isClearModalOpen.value = false;
        fetchLogs();
        // Optional: Show toast
    } catch (err) {
        console.error("Failed to clear logs:", err);
    } finally {
        clearing.value = false;
    }
}

watch([page, () => filters.action], () => {
    fetchLogs();
});

onMounted(() => {
    fetchLogs();
});

// Format payload for display
function formatPayload(payload?: string) {
    if (!payload) return "-";
    try {
        const obj = JSON.parse(payload);
        return Object.entries(obj)
            .map(([k, v]) => `${k}: ${v}`)
            .join(", ");
    } catch {
        return payload;
    }
}
</script>

<template>
    <div>
        <div class="flex flex-col gap-4 mb-6">
            <div class="flex justify-between items-center">
                <h1 class="text-2xl font-bold">Audit Logs</h1>
                <div class="flex gap-2">
                    <UButton
                        v-if="isAdmin"
                        color="error"
                        variant="soft"
                        icon="i-heroicons-trash"
                        @click="isClearModalOpen = true"
                    >
                        Clear Logs
                    </UButton>
                    <UButton
                        variant="soft"
                        icon="i-heroicons-arrow-path"
                        :loading="loading"
                        @click="fetchLogs"
                    >
                        Refresh
                    </UButton>
                    <UButton
                        variant="solid"
                        color="neutral"
                        icon="i-heroicons-arrow-down-tray"
                        @click="exportCsv"
                    >
                        Export CSV
                    </UButton>
                </div>
            </div>

            <!-- Filters -->
            <UCard :ui="{ body: 'p-4' }">
                <div class="grid grid-cols-1 md:grid-cols-4 gap-4 items-end">
                    <UFormField label="Action">
                        <USelect
                            v-model="filters.action"
                            :items="actions"
                            placeholder="All Actions"
                            class="w-full"
                        />
                    </UFormField>
                    <UFormField label="User ID" v-if="isAdmin">
                        <UInput
                            v-model="filters.userId"
                            placeholder="Filter by User ID"
                            icon="i-heroicons-user"
                        />
                    </UFormField>
                    <UFormField label="Time Range">
                        <div class="flex gap-2">
                            <!-- Simple workaround for date range, using two date pickers would be better but keeping it simple for now or usage of a specific component if available -->
                            <!-- Using raw input type date for compatibility -->
                            <input
                                type="date"
                                class="bg-neutral-900 border border-neutral-800 rounded px-2 py-1 text-sm text-gray-300 w-full"
                                :value="
                                    filters.startDate &&
                                    filters.startDate
                                        .toISOString()
                                        .split('T')[0]
                                "
                                @input="
                                    (e: any) =>
                                        (filters.startDate = e.target.value
                                            ? new Date(e.target.value)
                                            : undefined)
                                "
                            />
                            <input
                                type="date"
                                class="bg-neutral-900 border border-neutral-800 rounded px-2 py-1 text-sm text-gray-300 w-full"
                                :value="
                                    filters.endDate &&
                                    filters.endDate.toISOString().split('T')[0]
                                "
                                @input="
                                    (e: any) =>
                                        (filters.endDate = e.target.value
                                            ? new Date(e.target.value)
                                            : undefined)
                                "
                            />
                        </div>
                    </UFormField>
                    <div class="flex gap-2">
                        <UButton
                            color="primary"
                            block
                            @click="
                                () => {
                                    page = 1;
                                    fetchLogs();
                                }
                            "
                            >Apply</UButton
                        >
                        <UButton
                            color="neutral"
                            variant="ghost"
                            block
                            @click="resetFilters"
                            >Reset</UButton
                        >
                    </div>
                </div>
            </UCard>
        </div>

        <UCard :ui="{ body: 'p-0' }">
            <UTable
                :data="logs"
                :columns="columns"
                :loading="loading"
                class="w-full"
            >
                <template #userName-cell="{ row }">
                    <div class="flex flex-col">
                        <span class="font-medium text-white">{{
                            row.original.userName || "Unknown"
                        }}</span>
                        <code class="text-xs text-gray-500">{{
                            row.original.userId
                        }}</code>
                    </div>
                </template>

                <template #action-cell="{ row }">
                    <UBadge variant="subtle" color="primary" class="capitalize">
                        {{ row.original.action.replace(/_/g, " ") }}
                    </UBadge>
                </template>

                <template #payload-cell="{ row }">
                    <span
                        class="text-xs text-gray-400 font-mono truncate max-w-xs block"
                        :title="row.original.payload"
                    >
                        {{ formatPayload(row.original.payload) }}
                    </span>
                </template>

                <template #ipAddress-cell="{ row }">
                    <span class="text-xs text-gray-500 font-mono">
                        {{ row.original.ipAddress }}
                    </span>
                </template>

                <template #timestamp-cell="{ row }">
                    <div class="text-sm text-gray-400">
                        {{ new Date(row.original.timestamp).toLocaleString() }}
                    </div>
                </template>
            </UTable>

            <!-- Pagination -->
            <div
                class="p-4 border-t border-neutral-800 flex justify-between items-center"
                v-if="total > 0"
            >
                <span class="text-sm text-gray-500">
                    Showing {{ (page - 1) * limit + 1 }} to
                    {{ Math.min(page * limit, total) }} of {{ total }} entries
                </span>
                <UPagination
                    v-model:page="page"
                    :total="total"
                    :items-per-page="limit"
                    size="sm"
                />
            </div>
        </UCard>

        <!-- Clear Logs Confirmation Modal -->
        <UModal
            v-model:open="isClearModalOpen"
        >
            <template #header>
                <div class="flex items-center justify-between">
                    <h3 class="text-base font-semibold leading-6 text-white">
                        Clear Audit Logs
                    </h3>
                </div>
            </template>
            <template #body>
                <div class="text-sm text-gray-400">
                    Are you sure you want to clear <strong>ALL</strong> audit
                    logs? This action cannot be undone.
                </div>
            </template>
            <template #footer>
                <div class="flex justify-end gap-2">
                    <UButton
                        color="neutral"
                        variant="ghost"
                        @click="isClearModalOpen = false"
                        >Cancel</UButton
                    >
                    <UButton
                        color="error"
                        :loading="clearing"
                        @click="clearLogs"
                        >Clear All</UButton
                    >
                </div>
            </template>
        </UModal>
    </div>
</template>
