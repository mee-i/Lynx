<script setup lang="ts">
import type { TableColumn } from "@nuxt/ui";
import { authClient } from "~/utils/auth-client";

const toast = useToast();

definePageMeta({
    layout: "dashboard",
    title: "Webhooks",
});

const { data: session } = await authClient.useSession(useFetch);

interface Webhook {
    id: number;
    userId: string;
    url: string;
    events: string[];
    isEnabled: boolean;
    createdAt: string;
    updatedAt: string;
}

const loading = ref(true);
const webhooks = ref<Webhook[]>([]);
const isModalOpen = ref(false);
const isDeleteModalOpen = ref(false);
const isDeleting = ref(false);
const isSaving = ref(false);
const isTesting = ref<number | null>(null);
const urlError = ref("");
const selectedWebhook = ref<Partial<Webhook> | null>(null);
const webhookToDelete = ref<Webhook | null>(null);

const columns: TableColumn<Webhook>[] = [
    { accessorKey: "url", header: "Destination URL" },
    { accessorKey: "events", header: "Events" },
    { accessorKey: "isEnabled", header: "Status" },
    { accessorKey: "createdAt", header: "Created At" },
    { accessorKey: "actions", header: "" },
];

async function fetchWebhooks() {
    loading.value = true;
    try {
        const res = await $fetch<Webhook[]>("/api/webhooks");
        webhooks.value = res;
    } catch (err) {
        console.error("Failed to fetch webhooks:", err);
    } finally {
        loading.value = false;
    }
}

function openCreateModal() {
    selectedWebhook.value = {
        url: "",
        events: ["device.connect", "device.disconnect"],
        isEnabled: true,
    };
    isModalOpen.value = true;
}

function openEditModal(webhook: Webhook) {
    selectedWebhook.value = { ...webhook };
    isModalOpen.value = true;
}

async function saveWebhook() {
    if (!selectedWebhook.value?.url || !selectedWebhook.value?.events?.length) return;
    
    urlError.value = "";
    try {
        new URL(selectedWebhook.value.url);
    } catch (e) {
        urlError.value = "Please enter a valid URL (starting with http:// or https://)";
        return;
    }
    
    isSaving.value = true;
    try {
        await $fetch("/api/webhooks", {
            method: "POST",
            body: selectedWebhook.value,
        });
        isModalOpen.value = false;
        fetchWebhooks();
    } catch (err) {
        console.error("Failed to save webhook:", err);
    } finally {
        isSaving.value = false;
    }
}

function openDeleteModal(webhook: Webhook) {
    webhookToDelete.value = webhook;
    isDeleteModalOpen.value = true;
}

async function confirmDelete() {
    if (!webhookToDelete.value) return;
    
    isDeleting.value = true;
    try {
        await $fetch(`/api/webhooks/${webhookToDelete.value.id}`, {
            method: "DELETE",
        });
        isDeleteModalOpen.value = false;
        fetchWebhooks();
    } catch (err) {
        console.error("Failed to delete webhook:", err);
    } finally {
        isDeleting.value = false;
        webhookToDelete.value = null;
    }
}

async function testWebhook(webhook: Webhook) {
    isTesting.value = webhook.id;
    try {
        await $fetch("/api/webhooks/test", {
            method: "POST",
            body: { url: webhook.url },
        });
        toast.add({
            title: "Success",
            description: "Test notification sent successfully!",
            color: "success",
        });
    } catch (err: any) {
        console.error("Webhook test failed:", err);
        toast.add({
            title: "Error",
            description: "Webhook test failed: " + (err.statusMessage || err.message || "Unknown error"),
            color: "error",
        });
    } finally {
        isTesting.value = null;
    }
}

async function toggleEnabled(webhook: Webhook) {
    try {
        await $fetch("/api/webhooks", {
            method: "POST",
            body: {
                id: webhook.id,
                url: webhook.url,
                events: webhook.events,
                isEnabled: !webhook.isEnabled,
            },
        });
        fetchWebhooks();
    } catch (err) {
        console.error("Failed to toggle webhook status:", err);
    }
}

onMounted(() => {
    fetchWebhooks();
});

const eventOptions = [
    { label: "Device Connected", value: "device.connect" },
    { label: "Device Disconnected", value: "device.disconnect" },
];
</script>

<template>
    <div>
        <div class="flex flex-col gap-4 mb-6">
            <div class="flex justify-between items-center">
                <div>
                    <h1 class="text-2xl font-bold">Webhooks</h1>
                    <p class="text-sm text-gray-500">Receive real-time notifications for device events.</p>
                </div>
                <div class="flex gap-2">
                    <UButton
                        variant="soft"
                        icon="i-heroicons-arrow-path"
                        :loading="loading"
                        @click="fetchWebhooks"
                    >
                        Refresh
                    </UButton>
                    <UButton
                        color="primary"
                        icon="i-heroicons-plus"
                        @click="openCreateModal"
                    >
                        Add Webhook
                    </UButton>
                </div>
            </div>
        </div>

        <UCard :ui="{ body: 'p-0' }">
            <UTable
                :data="webhooks"
                :columns="columns"
                :loading="loading"
                class="w-full"
            >
                <template #url-cell="{ row }">
                    <div class="flex items-center gap-2">
                        <UIcon name="i-material-symbols-link" class="text-gray-500" />
                        <span class="font-mono text-xs text-white truncate max-w-md">{{ row.original.url }}</span>
                    </div>
                </template>

                <template #events-cell="{ row }">
                    <div class="flex gap-1 flex-wrap">
                        <UBadge
                            v-for="event in row.original.events"
                            :key="event"
                            variant="subtle"
                            size="sm"
                            color="neutral"
                            class="text-[10px]"
                        >
                            {{ event === 'device.connect' ? 'On Connect' : 'On Disconnect' }}
                        </UBadge>
                    </div>
                </template>

                <template #isEnabled-cell="{ row }">
                    <div class="flex items-center gap-2">
                        <USwitch
                            :model-value="!!row.original.isEnabled"
                            @update:model-value="() => toggleEnabled(row.original)"
                        />
                        <span :class="row.original.isEnabled ? 'text-primary' : 'text-gray-500'" class="text-xs">
                            {{ row.original.isEnabled ? 'Enabled' : 'Disabled' }}
                        </span>
                    </div>
                </template>

                <template #createdAt-cell="{ row }">
                    <span class="text-xs text-gray-400">
                        {{ new Date(row.original.createdAt).toLocaleString() }}
                    </span>
                </template>

                <template #actions-cell="{ row }">
                    <div class="flex justify-end gap-1">
                        <UButton
                            icon="i-heroicons-paper-airplane"
                            variant="ghost"
                            color="primary"
                            size="xs"
                            :loading="isTesting === row.original.id"
                            title="Send Test Notification"
                            @click="testWebhook(row.original)"
                        />
                        <UButton
                            icon="i-heroicons-pencil-square"
                            variant="ghost"
                            color="neutral"
                            size="xs"
                            @click="openEditModal(row.original)"
                        />
                        <UButton
                            icon="i-heroicons-trash"
                            variant="ghost"
                            color="error"
                            size="xs"
                            @click="openDeleteModal(row.original)"
                        />
                    </div>
                </template>
            </UTable>

            <div v-if="!loading && webhooks.length === 0" class="p-12 text-center">
                <UIcon name="i-material-symbols-webhook" class="text-4xl text-gray-700 mb-2" />
                <p class="text-gray-500">No webhooks configured yet.</p>
                <UButton
                    variant="link"
                    color="primary"
                    @click="openCreateModal"
                >
                    Create your first webhook
                </UButton>
            </div>
        </UCard>

        <!-- Create/Edit Modal -->
        <UModal v-model:open="isModalOpen">
            <template #header>
                <h3 class="text-base font-semibold leading-6 text-white">
                    {{ selectedWebhook?.id ? 'Edit Webhook' : 'Add Webhook' }}
                </h3>
            </template>
            
            <template #body>
                <div class="space-y-4" v-if="selectedWebhook">
                    <UFormField
                        label="Destination URL"
                        required
                        :error="urlError"
                        description="The URL where the POST notification will be sent."
                    >
                        <UInput
                            v-model="selectedWebhook.url"
                            placeholder="https://your-server.com/webhook"
                            icon="i-material-symbols-link"
                            autofocus
                            @input="urlError = ''"
                        />
                    </UFormField>

                    <UFormField label="Trigger Events" required description="Select which actions should trigger this webhook.">
                        <div class="flex flex-col gap-2">
                            <UCheckbox
                                v-for="option in eventOptions"
                                :key="option.value"
                                :label="option.label"
                                :model-value="selectedWebhook.events?.includes(option.value)"
                                @update:model-value="(val) => {
                                    if (val) {
                                        selectedWebhook!.events!.push(option.value);
                                    } else {
                                        selectedWebhook!.events = selectedWebhook!.events!.filter(e => e !== option.value);
                                    }
                                }"
                            />
                        </div>
                    </UFormField>

                    <UFormField label="Status">
                        <div class="flex items-center gap-2">
                            <USwitch v-model="selectedWebhook.isEnabled" />
                            <span class="text-sm">{{ selectedWebhook.isEnabled ? 'Active' : 'Inactive' }}</span>
                        </div>
                    </UFormField>
                </div>
            </template>

            <template #footer>
                <div class="flex justify-end gap-2">
                    <UButton
                        color="neutral"
                        variant="ghost"
                        @click="isModalOpen = false"
                    >
                        Cancel
                    </UButton>
                    <UButton
                        color="primary"
                        :loading="isSaving"
                        :disabled="!selectedWebhook?.url || !selectedWebhook?.events?.length"
                        @click="saveWebhook"
                    >
                        {{ selectedWebhook?.id ? 'Update Webhook' : 'Create Webhook' }}
                    </UButton>
                </div>
            </template>
        </UModal>

        <!-- Delete Confirmation Modal -->
        <UModal v-model:open="isDeleteModalOpen">
            <template #header>
                <div class="flex items-center justify-between">
                    <h3 class="text-base font-semibold leading-6 text-white">
                        Delete Webhook
                    </h3>
                </div>
            </template>
            <template #body>
                <div class="text-sm text-gray-400">
                    Are you sure you want to delete the webhook for <strong>{{ webhookToDelete?.url }}</strong>? This action cannot be undone.
                </div>
            </template>
            <template #footer>
                <div class="flex justify-end gap-2">
                    <UButton
                        color="neutral"
                        variant="ghost"
                        @click="isDeleteModalOpen = false"
                    >
                        Cancel
                    </UButton>
                    <UButton
                        color="error"
                        :loading="isDeleting"
                        @click="confirmDelete"
                    >
                        Delete
                    </UButton>
                </div>
            </template>
        </UModal>
    </div>
</template>
