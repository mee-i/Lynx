<script setup lang="ts">
import { authClient } from "@/utils/auth-client";
const session = authClient.useSession();
const loggedIn = computed(() => !!session.value?.data);

const route = useRoute();
const callbackURL = route.query.callbackUrl as string || '/dashboard';

watchEffect(() => {
    if (loggedIn.value) {
        navigateTo('/dashboard');
    }
});

const handleSignIn = async (provider: 'google' | 'github') => {
    await authClient.signIn.social({
        provider,
        callbackURL
    })
}

const colorMode = useColorMode()
</script>

<template>
    <div class="min-h-screen bg-default flex items-center justify-center px-6">
        <div class="w-full max-w-sm">
            <div class="text-center mb-8">
                <h1 class="text-2xl font-bold text-highlighted">Welcome to <span class="text-primary">Lynx</span></h1>
                <p class="text-muted mt-1">Sign in to access your devices</p>
            </div>

            <UCard>
                <div class="space-y-3">
                    <UButton 
                        block 
                        size="lg" 
                        variant="outline" 
                        @click="handleSignIn('google')"
                    >
                        <UIcon name="i-simple-icons-google" class="w-5 h-5 mr-2" />
                        Continue with Google
                    </UButton>

                    <UButton 
                        block 
                        size="lg" 
                        variant="outline" 
                        @click="handleSignIn('github')"
                    >
                        <UIcon name="i-simple-icons-github" class="w-5 h-5 mr-2" />
                        Continue with GitHub
                    </UButton>
                </div>
            </UCard>

            <!-- Dark Mode Toggle -->
            <div class="mt-8 flex justify-center">
                <UColorModeSwitch />
            </div>
        </div>
    </div>
</template>
