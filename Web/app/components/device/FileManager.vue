<script setup lang="ts">
import type { TableColumn } from "@nuxt/ui";

const props = defineProps<{
    ws: WebSocket | null;
    status: string;
    deviceId: string;
}>();

interface FileItem {
    name: string;
    isDir: boolean;
    size: number;
    modifiedAt: number;
}

const currentPath = ref("C:\\");
const files = ref<FileItem[]>([]);
const drives = ref<string[]>([]);
const isLoading = ref(false);
const error = ref<string | null>(null);
const pendingRequests = new Map<string, (data: any) => void>();

// Upload state
const isUploading = ref(false);
const uploadProgress = ref(0);
const fileInputRef = ref<HTMLInputElement | null>(null);

// Download state
interface DownloadTask {
    id: string;
    name: string;
    progress: number;
    status: 'downloading' | 'completed' | 'error';
    error?: string;
}

const activeDownloads = ref<DownloadTask[]>([]);
const CHUNK_SIZE = 1024 * 1024; // 1MB

// Modals
const deleteModalOpen = ref(false);
const renameModalOpen = ref(false);
const selectedFile = ref<FileItem | null>(null);
const newName = ref("");
const selected = ref<FileItem[]>([]);

const toast = useToast();

// Breadcrumb segments
const pathSegments = computed(() => {
    const parts = currentPath.value.split("\\").filter(Boolean);
    const segments: { label: string; path: string }[] = [];
    let accumulated = "";
    for (const part of parts) {
        accumulated += part + "\\";
        segments.push({ label: part, path: accumulated });
    }
    return segments;
});

// Table columns
const columns: TableColumn<FileItem>[] = [
    { id: "select" },
    { accessorKey: "name", header: "Name" },
    { accessorKey: "size", header: "Size" },
    { accessorKey: "modifiedAt", header: "Modified" },
    { id: "actions" },
];

function generateRequestId() {
    return Math.random().toString(36).substring(2, 15);
}

function sendFsCommand(action: string, params: Record<string, any> = {}): Promise<any> {
    return new Promise((resolve, reject) => {
        if (!props.ws || props.status !== "connected") {
            reject(new Error("Not connected"));
            return;
        }

        const requestId = generateRequestId();
        const timeout = setTimeout(() => {
            pendingRequests.delete(requestId);
            reject(new Error("Request timeout"));
        }, 30000);

        pendingRequests.set(requestId, (data) => {
            clearTimeout(timeout);
            if (data.success) {
                resolve(data);
            } else {
                reject(new Error(data.error || "Unknown error"));
            }
        });

        props.ws.send(JSON.stringify({
            type: "filesystem",
            action,
            requestId,
            ...params,
        }));
    });
}

// Handle incoming filesystem messages
function handleFsMessage(msg: any) {
    if (msg.type === "filesystem" && msg.requestId) {
        const handler = pendingRequests.get(msg.requestId);
        if (handler) {
            pendingRequests.delete(msg.requestId);
            handler(msg);
        }
    }
}

// Expose handler to parent
defineExpose({ handleFsMessage });

async function fetchDrives() {
    try {
        const result = await sendFsCommand("drives");
        drives.value = result.data || [];
        if (drives.value.length > 0 && !drives.value.includes(currentPath.value.substring(0, 3))) {
            const firstDrive = drives.value[0];
            if (firstDrive) currentPath.value = firstDrive;
        }
    } catch (e: any) {
        console.error("Failed to fetch drives:", e);
    }
}

async function fetchDirectory(path: string) {
    isLoading.value = true;
    error.value = null;
    selected.value = []; // Reset selection on navigation
    try {
        const result = await sendFsCommand("ls", { path });
        files.value = result.data || [];
        currentPath.value = path;
    } catch (e: any) {
        error.value = e.message;
        toast.add({
            title: "Error",
            description: e.message,
            color: "error",
            icon: "i-heroicons-exclamation-triangle",
        });
    } finally {
        isLoading.value = false;
    }
}

function navigateTo(path: string) {
    fetchDirectory(path);
}

function openItem(item: FileItem) {
    if (item.isDir) {
        const newPath = currentPath.value.endsWith("\\")
            ? currentPath.value + item.name
            : currentPath.value + "\\" + item.name;
        navigateTo(newPath);
    } else {
        downloadFile(item);
    }
}

function goUp() {
    const parts = currentPath.value.split("\\").filter(Boolean);
    if (parts.length > 1) {
        parts.pop();
        navigateTo(parts.join("\\") + "\\");
    }
}

async function downloadFile(item: FileItem) {
    if (item.isDir) return;
    
    const downloadId = generateRequestId();
    const task: DownloadTask = reactive({
        id: downloadId,
        name: item.name,
        progress: 0,
        status: 'downloading'
    });
    activeDownloads.value.push(task);
    
    const chunks: Uint8Array[] = [];
    
    try {
        const filePath = currentPath.value.endsWith("\\")
            ? currentPath.value + item.name
            : currentPath.value + "\\" + item.name;
        
        let offset = 0;
        let totalSize = item.size;
        
        while (offset < totalSize) {
            let retryCount = 0;
            let success = false;
            let result: any = null;

            while (retryCount < 3 && !success) {
                try {
                    result = await sendFsCommand("read", { 
                        path: filePath, 
                        offset, 
                        length: CHUNK_SIZE 
                    });
                    success = true;
                } catch (e) {
                    retryCount++;
                    if (retryCount >= 3) throw e;
                    await new Promise(r => setTimeout(r, 1000)); // Wait 1s before retry
                }
            }

            // Decode base64 chunk
            const binaryString = atob(result.data);
            const bytes = new Uint8Array(binaryString.length);
            for (let i = 0; i < binaryString.length; i++) {
                bytes[i] = binaryString.charCodeAt(i);
            }
            chunks.push(bytes);
            
            offset += result.size;
            totalSize = result.totalSize; // Update from server response
            task.progress = Math.round((offset / totalSize) * 100);
        }
        
        const blob = new Blob(chunks as any);
        const url = URL.createObjectURL(blob);
        const a = document.createElement("a");
        a.href = url;
        a.download = item.name;
        document.body.appendChild(a);
        a.click();
        document.body.removeChild(a);
        URL.revokeObjectURL(url);
        
        task.status = 'completed';
        toast.add({
            title: "Downloaded",
            description: item.name,
            color: "success",
            icon: "i-heroicons-arrow-down-tray",
        });
        
        // Remove from list after a short delay
        setTimeout(() => {
            const index = activeDownloads.value.findIndex(d => d.id === downloadId);
            if (index !== -1) activeDownloads.value.splice(index, 1);
        }, 5000);
    } catch (e: any) {
        task.status = 'error';
        task.error = e.message;
        toast.add({
            title: "Download failed",
            description: e.message,
            color: "error",
            icon: "i-heroicons-exclamation-triangle",
        });
    }
}

async function downloadSelected() {
    const filesToDownload = selected.value.filter(item => !item.isDir);
    if (filesToDownload.length === 0) {
        toast.add({
            title: "No files selected",
            description: "Please select one or more files to download.",
            color: "warning"
        });
        return;
    }

    // Start all downloads
    for (const item of filesToDownload) {
        downloadFile(item);
    }
    
    selected.value = []; // Clear selection after starting
}

function toggleSelectAll() {
    if (selected.value.length === files.value.length) {
        selected.value = [];
    } else {
        selected.value = [...files.value];
    }
}

function toggleItemSelection(item: FileItem) {
    const index = selected.value.findIndex(i => i.name === item.name);
    if (index === -1) {
        selected.value.push(item);
    } else {
        selected.value.splice(index, 1);
    }
}

function triggerUpload() {
    fileInputRef.value?.click();
}

async function handleFileUpload(event: Event) {
    const input = event.target as HTMLInputElement;
    const file = input.files?.[0];
    if (!file) return;

    isUploading.value = true;
    uploadProgress.value = 0;

    try {
        // Read file as base64
        const reader = new FileReader();
        const base64 = await new Promise<string>((resolve, reject) => {
            reader.onload = () => {
                const result = reader.result as string;
                const base64Data = result.split(",")[1] || result;
                resolve(base64Data);
            };
            reader.onerror = reject;
            reader.readAsDataURL(file);
        });

        uploadProgress.value = 50;

        const filePath = currentPath.value.endsWith("\\")
            ? currentPath.value + file.name
            : currentPath.value + "\\" + file.name;

        await sendFsCommand("write", { path: filePath, data: base64 });
        
        uploadProgress.value = 100;
        
        toast.add({
            title: "Uploaded",
            description: file.name,
            color: "success",
            icon: "i-heroicons-arrow-up-tray",
        });
        
        fetchDirectory(currentPath.value);
    } catch (e: any) {
        toast.add({
            title: "Upload failed",
            description: e.message,
            color: "error",
            icon: "i-heroicons-exclamation-triangle",
        });
    } finally {
        isUploading.value = false;
        uploadProgress.value = 0;
        input.value = "";
    }
}

function confirmDelete(item: FileItem) {
    selectedFile.value = item;
    deleteModalOpen.value = true;
}

async function deleteFile() {
    if (!selectedFile.value) return;
    
    try {
        const filePath = currentPath.value.endsWith("\\")
            ? currentPath.value + selectedFile.value.name
            : currentPath.value + "\\" + selectedFile.value.name;
        
        await sendFsCommand("delete", { path: filePath });
        
        toast.add({
            title: "Deleted",
            description: selectedFile.value.name,
            color: "success",
            icon: "i-heroicons-trash",
        });
        
        deleteModalOpen.value = false;
        selectedFile.value = null;
        fetchDirectory(currentPath.value);
    } catch (e: any) {
        toast.add({
            title: "Delete failed",
            description: e.message,
            color: "error",
            icon: "i-heroicons-exclamation-triangle",
        });
    }
}

function openRenameModal(item: FileItem) {
    selectedFile.value = item;
    newName.value = item.name;
    renameModalOpen.value = true;
}

async function renameFile() {
    if (!selectedFile.value || !newName.value) return;
    
    try {
        const oldPath = currentPath.value.endsWith("\\")
            ? currentPath.value + selectedFile.value.name
            : currentPath.value + "\\" + selectedFile.value.name;
        const newPath = currentPath.value.endsWith("\\")
            ? currentPath.value + newName.value
            : currentPath.value + "\\" + newName.value;
        
        await sendFsCommand("rename", { path: oldPath, newPath });
        
        toast.add({
            title: "Renamed",
            description: `${selectedFile.value.name} → ${newName.value}`,
            color: "success",
            icon: "i-heroicons-pencil",
        });
        
        renameModalOpen.value = false;
        selectedFile.value = null;
        newName.value = "";
        fetchDirectory(currentPath.value);
    } catch (e: any) {
        toast.add({
            title: "Rename failed",
            description: e.message,
            color: "error",
            icon: "i-heroicons-exclamation-triangle",
        });
    }
}

function formatSize(bytes: number): string {
    if (bytes === 0) return "-";
    const units = ["B", "KB", "MB", "GB"];
    let i = 0;
    while (bytes >= 1024 && i < units.length - 1) {
        bytes /= 1024;
        i++;
    }
    return `${bytes.toFixed(1)} ${units[i]}`;
}

function formatDate(timestamp: number): string {
    if (!timestamp) return "-";
    return new Date(timestamp).toLocaleString();
}

// Watch for connection and fetch initial data
watch(() => props.status, async (newStatus) => {
    if (newStatus === "connected") {
        await fetchDrives();
        if (files.value.length === 0) {
            fetchDirectory(currentPath.value);
        }
    }
}, { immediate: true });
</script>

<template>
    <div class="flex flex-col h-full">
        <!-- Toolbar -->
        <div class="flex items-center gap-2 p-3 border-b border-gray-800">
            <!-- Drive Selector -->
            <USelect
                v-model="currentPath"
                :items="drives"
                class="w-20"
                size="xs"
                @update:model-value="fetchDirectory($event)"
            />

            <UButton
                icon="i-heroicons-arrow-up"
                variant="ghost"
                color="neutral"
                size="xs"
                :disabled="pathSegments.length <= 1"
                @click="goUp"
            />
            
            <!-- Breadcrumb -->
            <div class="flex items-center gap-1 flex-1 overflow-x-auto text-sm">
                <template v-for="(segment, i) in pathSegments" :key="i">
                    <span v-if="i > 0" class="text-gray-600">/</span>
                    <UButton
                        variant="link"
                        color="neutral"
                        size="xs"
                        class="font-mono"
                        @click="navigateTo(segment.path)"
                    >
                        {{ segment.label }}
                    </UButton>
                </template>
            </div>
            
            <UButton
                icon="i-heroicons-arrow-path"
                variant="ghost"
                color="neutral"
                size="xs"
                :loading="isLoading"
                @click="fetchDirectory(currentPath)"
            />

            <UButton
                v-if="selected.length > 0"
                icon="i-heroicons-arrow-down-tray"
                color="primary"
                variant="solid"
                size="xs"
                @click="downloadSelected"
            >
                Download ({{ selected.filter(i => !i.isDir).length }})
            </UButton>
            
            <UButton
                icon="i-heroicons-arrow-up-tray"
                color="primary"
                variant="soft"
                size="xs"
                :loading="isUploading"
                @click="triggerUpload"
            >
                Upload
            </UButton>
            
            <input
                ref="fileInputRef"
                type="file"
                class="hidden"
                @change="handleFileUpload"
            />
        </div>
        
        <!-- Upload Progress -->
        <div v-if="isUploading" class="px-3 py-2 bg-primary-500/10 border-b border-primary-500/20">
            <div class="flex items-center gap-2 text-xs text-primary-400">
                <UIcon name="i-heroicons-arrow-up-tray" class="animate-pulse" />
                <span>Uploading...</span>
                <div class="flex-1 h-1 bg-gray-800 rounded-full overflow-hidden">
                    <div 
                        class="h-full bg-primary-500 transition-all"
                        :style="{ width: `${uploadProgress}%` }"
                    />
                </div>
                <span>{{ uploadProgress }}%</span>
            </div>
        </div>

        <!-- Download Progress List -->
        <div v-if="activeDownloads.length > 0" class="flex flex-col border-b border-gray-800">
            <div 
                v-for="task in activeDownloads" 
                :key="task.id"
                class="px-3 py-2 bg-primary-500/5 border-b last:border-0 border-primary-500/10"
            >
                <div class="flex items-center gap-2 text-[10px] text-primary-400">
                    <UIcon 
                        :name="task.status === 'completed' ? 'i-heroicons-check-circle' : (task.status === 'error' ? 'i-heroicons-x-circle' : 'i-heroicons-arrow-down-tray')" 
                        :class="{ 'animate-pulse': task.status === 'downloading', 'text-success-400': task.status === 'completed', 'text-error-400': task.status === 'error' }"
                    />
                    <span class="font-mono truncate max-w-[150px]">{{ task.name }}</span>
                    <div class="flex-1 h-1 bg-gray-800 rounded-full overflow-hidden">
                        <div 
                            class="h-full transition-all"
                            :class="task.status === 'error' ? 'bg-error-500' : 'bg-primary-500'"
                            :style="{ width: `${task.progress}%` }"
                        />
                    </div>
                    <span class="min-w-[30px] text-right">{{ task.progress }}%</span>
                </div>
            </div>
        </div>
        
        <!-- Error State -->
        <div v-if="error" class="p-4 text-center text-red-400 text-sm">
            <UIcon name="i-heroicons-exclamation-triangle" class="w-6 h-6 mb-2" />
            <p>{{ error }}</p>
            <UButton
                variant="link"
                color="primary"
                size="xs"
                class="mt-2"
                @click="fetchDirectory(currentPath)"
            >
                Retry
            </UButton>
        </div>
        
        <!-- File List -->
        <div v-else class="flex-1 overflow-auto">
            <UTable
                :data="files"
                :columns="columns"
                :loading="isLoading"
                :ui="{ td: 'px-4 py-2', th: 'px-4 py-2' }"
            >
                <template #select-header>
                    <UCheckbox
                        :model-value="selected.length === files.length && files.length > 0"
                        @update:model-value="toggleSelectAll"
                    />
                </template>

                <template #select-cell="{ row }">
                    <UCheckbox
                        :model-value="selected.some(i => i.name === row.original.name)"
                        @update:model-value="toggleItemSelection(row.original)"
                        @click.stop
                    />
                </template>
                <template #name-cell="{ row }">
                    <div
                        class="flex items-center gap-2 cursor-pointer hover:text-primary-400 transition-colors"
                        @click="openItem(row.original)"
                    >
                        <UIcon
                            :name="row.original.isDir ? 'i-heroicons-folder' : 'i-heroicons-document'"
                            :class="row.original.isDir ? 'text-yellow-400' : 'text-gray-400'"
                        />
                        <span class="font-mono text-sm">{{ row.original.name }}</span>
                    </div>
                </template>
                
                <template #size-cell="{ row }">
                    <span class="text-xs text-gray-500 font-mono">
                        {{ row.original.isDir ? "-" : formatSize(row.original.size) }}
                    </span>
                </template>
                
                <template #modifiedAt-cell="{ row }">
                    <span class="text-xs text-gray-500">
                        {{ formatDate(row.original.modifiedAt) }}
                    </span>
                </template>
                
                <template #actions-cell="{ row }">
                    <div class="flex items-center gap-1 justify-end">
                        <UButton
                            v-if="!row.original.isDir"
                            icon="i-heroicons-arrow-down-tray"
                            variant="ghost"
                            color="neutral"
                            size="xs"
                            @click.stop="downloadFile(row.original)"
                        />
                        <UButton
                            icon="i-heroicons-pencil"
                            variant="ghost"
                            color="neutral"
                            size="xs"
                            @click.stop="openRenameModal(row.original)"
                        />
                        <UButton
                            icon="i-heroicons-trash"
                            variant="ghost"
                            color="error"
                            size="xs"
                            @click.stop="confirmDelete(row.original)"
                        />
                    </div>
                </template>
            </UTable>
            
            <!-- Empty State -->
            <div v-if="!isLoading && files.length === 0 && !error" class="flex flex-col items-center justify-center h-full text-gray-500">
                <UIcon name="i-heroicons-folder-open" class="w-12 h-12 mb-2 opacity-20" />
                <span class="text-sm">Empty folder</span>
            </div>
        </div>
        
        <!-- Delete Modal -->
        <UModal v-model:open="deleteModalOpen">
            <template #title>Delete {{ selectedFile?.isDir ? 'Folder' : 'File' }}</template>
            <template #body>
                <p>Are you sure you want to delete <span class="font-bold text-white">{{ selectedFile?.name }}</span>?</p>
                <p class="text-sm text-gray-400 mt-2">This action cannot be undone.</p>
            </template>
            <template #footer>
                <div class="flex justify-end gap-2">
                    <UButton color="neutral" variant="ghost" @click="deleteModalOpen = false">Cancel</UButton>
                    <UButton color="error" @click="deleteFile">Delete</UButton>
                </div>
            </template>
        </UModal>
        
        <!-- Rename Modal -->
        <UModal v-model:open="renameModalOpen">
            <template #title>Rename</template>
            <template #body>
                <UFormField label="New name">
                    <UInput v-model="newName" />
                </UFormField>
            </template>
            <template #footer>
                <div class="flex justify-end gap-2">
                    <UButton color="neutral" variant="ghost" @click="renameModalOpen = false">Cancel</UButton>
                    <UButton color="primary" @click="renameFile">Rename</UButton>
                </div>
            </template>
        </UModal>
    </div>
</template>
