<script setup lang="ts">
import { ref, onMounted, onBeforeUnmount, watch, computed, nextTick } from 'vue';

const props = defineProps<{
    deviceId: string;
    deviceName: string;
}>();

const emit = defineEmits(['remove']);

const ws = ref<WebSocket | null>(null);
const status = ref("disconnected");
const activeLiveVideo = ref<"screen" | "cam" | null>(null);
const isMicOn = ref(false);
const audioCtx = ref<AudioContext | null>(null);
const gainNode = ref<GainNode | null>(null);
const volume = ref(Number(typeof window !== 'undefined' ? localStorage.getItem('stream_volume') || '1' : '1'));
const streamBitrates = ref({ video: 0, audio: 0 });
const selectedMic = ref<string>("");
const streamRetryKey = ref(0);
const availableCameras = ref<string[]>([]);
const availableMics = ref<string[]>([]);
const selectedCamera = ref<string>("");

const displayedFrame = ref<string>("");

onBeforeUnmount(() => {
    if (displayedFrame.value) URL.revokeObjectURL(displayedFrame.value);
});

const cardRef = ref<HTMLElement | null>(null);
const isExpanded = ref(false);

let audioAbortController: AbortController | null = null;

const runtimeConfig = useRuntimeConfig();

const videoStreamUrl = computed(() => {
    if (!activeLiveVideo.value) return "";
    return `/lynx/api/devices/${props.deviceId}/stream/video_down?type=${activeLiveVideo.value}&t=${streamRetryKey.value}`;
});

function formatSpeed(bytesPerSec: number) {
    if (bytesPerSec === 0) return '0 B/s';
    if (bytesPerSec < 1024) return `${bytesPerSec} B/s`;
    if (bytesPerSec < 1024 * 1024) return `${(bytesPerSec / 1024).toFixed(1)} KB/s`;
    return `${(bytesPerSec / (1024 * 1024)).toFixed(1)} MB/s`;
}

function connect() {
    status.value = "connecting";
    const clientId = `cctv-${props.deviceId}-${Math.random().toString(36).substr(2, 5)}`;
    // Note: session is handled by cookies, but we pass id for the relay to know who we are
    ws.value = new WebSocket(
        `${runtimeConfig.public.websocket_url}/ws?type=client&id=${clientId}`,
    );
    ws.value.binaryType = 'arraybuffer';

    ws.value.onopen = () => {
        status.value = "connected";
        ws.value?.send(JSON.stringify({ type: "subscribe", deviceId: props.deviceId }));
    };

    ws.value.onmessage = (event) => {
        if (event.data instanceof ArrayBuffer) {
            const buffer = event.data;
            if (buffer.byteLength < 1) return;
            const typeByte = new Uint8Array(buffer, 0, 1)[0];
            const mediaData = buffer.slice(1);

            if (typeByte === 0x01 || typeByte === 0x02) {
                if (displayedFrame.value) URL.revokeObjectURL(displayedFrame.value);
                const blob = new Blob([mediaData], { type: 'image/jpeg' });
                displayedFrame.value = URL.createObjectURL(blob);
            } else if (typeByte === 0x03) {
                handleIncomingAudio(mediaData);
            }
            return;
        }

        try {
            const msg = JSON.parse(event.data);
            if (msg.type === "status") {
                if (msg.status === "online") {
                    ws.value?.send(JSON.stringify({ type: "action", action: "list_media_devices" }));
                }
            } else if (msg.type === "media_devices_list") {
                availableCameras.value = msg.data.cameras;
                if (availableCameras.value.length > 0 && !selectedCamera.value) selectedCamera.value = availableCameras.value[0]!;
                availableMics.value = msg.data.mics;
                if (availableMics.value.length > 0 && !selectedMic.value) selectedMic.value = availableMics.value[0]!;
            } else if (msg.type === "metrics") {
                streamBitrates.value = {
                    video: msg.data.videoBitrate || 0,
                    audio: msg.data.audioBitrate || 0
                };
            }
        } catch (e) {
            console.error("CctvCell WS Error:", e);
        }
    };

    ws.value.onclose = () => {
        status.value = "disconnected";
    };

    ws.value.onerror = () => {
        status.value = "error";
    };
}

function toggleLiveVideo(type: "screen" | "cam") {
    if (!ws.value) return;

    if (activeLiveVideo.value === type) {
        ws.value.send(JSON.stringify({ type: "action", action: "stop_stream", stream: type }));
        activeLiveVideo.value = null;
    } else {
        const prevType = activeLiveVideo.value;
        activeLiveVideo.value = null; // Force unmount/stop to clear browser cache
        
        if (prevType && ws.value) {
            ws.value.send(JSON.stringify({ type: "action", action: "stop_stream", stream: prevType }));
        }

        nextTick(() => {
            activeLiveVideo.value = type;
            streamRetryKey.value++;
            
            // Small delay to allow previous stream to close on the agent side
            setTimeout(() => {
                if (activeLiveVideo.value !== type) return;
                ws.value?.send(JSON.stringify({ 
                    type: "action", 
                    action: "start_stream", 
                    stream: type, 
                    deviceIndex: type === 'cam' ? Math.max(0, availableCameras.value.indexOf(selectedCamera.value)) : undefined 
                }));
            }, 400);
        });
    }
}

// Watch device changes to restart streams if active
watch(selectedCamera, (newCam) => {
    if (activeLiveVideo.value === 'cam' && ws.value) {
        ws.value.send(JSON.stringify({ type: "action", action: "stop_stream", stream: "cam" }));
        streamRetryKey.value++;
        setTimeout(() => {
            if (activeLiveVideo.value === 'cam' && ws.value) {
                ws.value.send(JSON.stringify({ 
                    type: "action", 
                    action: "start_stream", 
                    stream: "cam", 
                    deviceIndex: Math.max(0, availableCameras.value.indexOf(newCam))
                }));
            }
        }, 500);
    }
});

watch(selectedMic, (newMic) => {
    if (isMicOn.value && ws.value) {
        ws.value.send(JSON.stringify({ type: "action", action: "stop_stream", stream: "mic" }));
        setTimeout(() => {
            if (isMicOn.value && ws.value) {
                ws.value.send(JSON.stringify({ 
                    type: "action", 
                    action: "start_stream", 
                    stream: "mic", 
                    deviceIndex: Math.max(0, availableMics.value.indexOf(newMic))
                }));
            }
        }, 500);
    }
});

function toggleMic() {
    if (!ws.value) return;
    isMicOn.value = !isMicOn.value;
    
    ws.value.send(JSON.stringify({ 
        type: "action", 
        action: isMicOn.value ? "start_stream" : "stop_stream", 
        stream: "mic",
        deviceIndex: Math.max(0, availableMics.value.indexOf(selectedMic.value)) 
    }));

    if (isMicOn.value) {
        startAudioPlayback();
    } else {
        stopAudioPlayback();
    }
}

async function startAudioPlayback() {
    if (!audioCtx.value || audioCtx.value.state === 'closed') {
        audioCtx.value = new AudioContext({ sampleRate: 44100 });
    }
    if (audioCtx.value.state === 'suspended') {
        await audioCtx.value.resume();
    }
    
    if (!gainNode.value) {
        gainNode.value = audioCtx.value.createGain();
        gainNode.value.gain.value = volume.value;
        gainNode.value.connect(audioCtx.value.destination);
    }
    lastAudioScheduleTime = 0;
}

let lastAudioScheduleTime = 0;
const AUDIO_BUFFER_DELAY = 0.15; // 150ms buffer

function handleIncomingAudio(value: ArrayBuffer) {
    if (!isMicOn.value || !audioCtx.value || audioCtx.value.state === 'suspended') return;

    const int16 = new Int16Array(value);
    const float32 = new Float32Array(int16.length);
    for (let i = 0; i < int16.length; i++) {
        float32[i] = int16[i]! / 32768;
    }

    const audioBuffer = audioCtx.value.createBuffer(1, float32.length, 44100);
    audioBuffer.getChannelData(0).set(float32);

    const source = audioCtx.value.createBufferSource();
    source.buffer = audioBuffer;
    source.connect(gainNode.value!);

    const now = audioCtx.value.currentTime;
    if (lastAudioScheduleTime < now) {
        lastAudioScheduleTime = now + AUDIO_BUFFER_DELAY;
    }
    
    source.start(lastAudioScheduleTime);
    lastAudioScheduleTime += audioBuffer.duration;
}

function stopAudioPlayback() {
    if (audioCtx.value) {
        audioCtx.value.suspend();
    }
    lastAudioScheduleTime = 0;
}

watch(volume, (val) => {
    if (typeof window !== 'undefined') {
        localStorage.setItem('stream_volume', val.toString());
    }
    if (gainNode.value) {
        gainNode.value.gain.setTargetAtTime(val, audioCtx.value?.currentTime || 0, 0.1);
    }
});

function toggleExpanded() {
    isExpanded.value = !isExpanded.value;
}

onMounted(() => {
    connect();
});

onBeforeUnmount(() => {
    if (ws.value && activeLiveVideo.value) {
        ws.value.send(JSON.stringify({ type: "action", action: "stop_stream", stream: activeLiveVideo.value }));
    }
    if (ws.value && isMicOn.value) {
        ws.value.send(JSON.stringify({ type: "action", action: "stop_stream", stream: "mic" }));
    }
    ws.value?.close();
    stopAudioPlayback();
});
</script>

<template>
    <UCard ref="cardRef" class="group relative overflow-hidden flex flex-col" :ui="{ body: 'p-0 flex-1 min-h-0 relative' }">
        <!-- Header Overlay -->
        <div class="absolute top-0 left-0 right-0 z-10 p-2 bg-black/80 opacity-0 group-hover:opacity-100 transition-opacity flex justify-between items-center border-b border-white/5">
            <div class="flex items-center gap-2">
                <span class="text-xs font-bold text-white font-mono">{{ deviceName }}</span>
                <UBadge :color="status === 'connected' ? 'success' : 'neutral'" size="xs" variant="subtle" class="scale-75">
                    {{ status }}
                </UBadge>
            </div>
            <div class="flex items-center gap-1">
                <UButton 
                    icon="i-heroicons-arrows-pointing-out" 
                    color="neutral" 
                    variant="ghost" 
                    size="xs" 
                    @click="toggleExpanded" 
                />
                <UButton icon="i-heroicons-x-mark" color="error" variant="ghost" size="xs" @click="$emit('remove')" />
            </div>
        </div>

        <!-- Video Area -->
        <div class="flex-1 flex items-center justify-center relative overflow-hidden h-full min-h-[140px]">
            <img 
                v-if="activeLiveVideo" 
                :key="`${activeLiveVideo}-frame`"
                :src="displayedFrame" 
                class="w-full h-full object-contain"
            />
            <div v-else class="flex flex-col items-center gap-2 text-neutral-700">
                <UIcon name="i-heroicons-video-camera-slash" class="w-10 h-10" />
                <span class="text-[9px] tracking-tighter font-black">No Active Stream</span>
            </div>

            <!-- Bitrate Overlay -->
            <div v-if="activeLiveVideo" class="absolute bottom-2 left-2 text-[10px] font-mono text-white bg-black/60 px-2 py-0.5 rounded shadow-lg backdrop-blur-sm">
                V: {{ formatSpeed(streamBitrates.video) }} | A: {{ formatSpeed(streamBitrates.audio) }}
            </div>
        </div>

        <!-- Bottom Controls Overlay -->
        <div class="absolute bottom-0 left-0 right-0 z-10 p-2 bg-black/90 opacity-0 group-hover:opacity-100 transition-opacity flex justify-between items-end gap-2 border-t border-white/5">
            <div class="flex flex-col gap-2 flex-1 min-w-0">
                <!-- Device Selectors -->
                <div class="flex gap-1 overflow-x-auto no-scrollbar">
                    <USelect v-if="availableCameras.length > 0" v-model="selectedCamera" :items="availableCameras" size="xs" variant="soft" class="min-w-[120px] max-w-[180px]" />
                    <USelect v-if="availableMics.length > 0" v-model="selectedMic" :items="availableMics" size="xs" variant="soft" class="min-w-[120px] max-w-[180px]" />
                </div>

                <div class="flex gap-1">
                    <!-- Screen Toggle -->
                    <UTooltip text="Toggle Screen">
                        <UButton 
                            icon="i-heroicons-tv" 
                            :color="activeLiveVideo === 'screen' ? 'primary' : 'neutral'" 
                            variant="soft" 
                            size="xs" 
                            @click="toggleLiveVideo('screen')"
                        />
                    </UTooltip>
                    <!-- Cam Toggle -->
                    <UTooltip text="Toggle Camera">
                        <UButton 
                            icon="i-heroicons-video-camera" 
                            :color="activeLiveVideo === 'cam' ? 'success' : 'neutral'" 
                            variant="soft" 
                            size="xs" 
                            @click="toggleLiveVideo('cam')"
                        />
                    </UTooltip>
                    <!-- Mic Toggle -->
                    <UTooltip text="Toggle Microphone">
                        <UButton 
                            :icon="isMicOn ? 'i-heroicons-microphone' : 'i-material-symbols-mic-off'" 
                            :color="isMicOn ? 'success' : 'neutral'" 
                            variant="soft" 
                            size="xs" 
                            @click="toggleMic"
                        />
                    </UTooltip>
                </div>
            </div>

            <!-- Volume Slider -->
            <div class="flex items-center gap-1.5 w-16 mb-1">
                <UIcon name="i-heroicons-speaker-wave" class="w-3 h-3 text-white/80" />
                <USlider v-model="volume" :min="0" :max="1" :step="0.01" size="xs" color="primary" />
            </div>
        </div>
    </UCard>

    <!-- Expanded View Modal -->
    <UModal v-model:open="isExpanded" fullscreen>
        <template #header>
            <div class="flex items-center justify-between w-full">
                <div class="flex items-center gap-3">
                    <span class="text-md font-bold text-primary">{{ deviceName }}</span>
                    <UBadge :color="status === 'connected' ? 'success' : 'neutral'" size="sm" variant="subtle">
                        {{ status }}
                    </UBadge>
                </div>
                <UButton icon="i-heroicons-arrows-pointing-in" color="neutral" variant="ghost" @click="toggleExpanded" />
            </div>
        </template>

        <template #body>
            <div class="flex flex-col h-full gap-4">
                <!-- Large Video Area -->
                <div class="flex-1 bg-neutral-950 flex items-center justify-center relative overflow-hidden rounded-xl border border-neutral-800">
                    <img 
                        v-if="activeLiveVideo" 
                        :key="`expanded-${activeLiveVideo}-frame`"
                        :src="displayedFrame" 
                        class="w-full h-full object-contain"
                    />
                    <div v-else class="flex flex-col items-center gap-4 text-neutral-800">
                        <UIcon name="i-heroicons-video-camera-slash" class="w-20 h-20 opacity-10" />
                        <span class="text-xs uppercase tracking-[0.2em] font-black opacity-30">No Active Stream</span>
                    </div>

                    <!-- Large Bitrate Overlay -->
                    <div v-if="activeLiveVideo" class="absolute bottom-4 left-4 text-xs font-mono text-white bg-black/80 px-3 py-1 rounded shadow-xl backdrop-blur-md border border-white/10">
                        VIDEO: {{ formatSpeed(streamBitrates.video) }} | AUDIO: {{ formatSpeed(streamBitrates.audio) }}
                    </div>
                </div>

                <!-- Expanded Controls Bar -->
                <div class="flex items-center justify-between p-4 bg-neutral-900/50 rounded-xl border border-neutral-800">
                    <div class="flex items-center gap-4">
                        <div class="flex gap-2">
                             <USelect v-if="availableCameras.length > 0" v-model="selectedCamera" :items="availableCameras" variant="soft" class="w-48" />
                             <USelect v-if="availableMics.length > 0" v-model="selectedMic" :items="availableMics" variant="soft" class="w-48" />
                        </div>
                        <div class="w-px h-6 bg-neutral-800"></div>
                        <div class="flex gap-2">
                            <UButton icon="i-heroicons-tv" :color="activeLiveVideo === 'screen' ? 'primary' : 'neutral'" variant="soft" @click="toggleLiveVideo('screen')" label="Screen" />
                            <UButton icon="i-heroicons-video-camera" :color="activeLiveVideo === 'cam' ? 'success' : 'neutral'" variant="soft" @click="toggleLiveVideo('cam')" label="Camera" />
                            <UButton :icon="isMicOn ? 'i-heroicons-microphone' : 'i-material-symbols-mic-off'" :color="isMicOn ? 'success' : 'neutral'" variant="soft" @click="toggleMic" label="Microphone" />
                        </div>
                    </div>

                    <div class="flex items-center gap-4 w-64">
                         <UIcon name="i-heroicons-speaker-wave" class="w-5 h-5 text-neutral-500" />
                         <USlider v-model="volume" :min="0" :max="1" :step="0.01" color="primary" />
                    </div>
                </div>
            </div>
        </template>
    </UModal>
</template>
