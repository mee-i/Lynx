<script setup lang="ts">
import { authClient } from "@/utils/auth-client";

const email = ref('')
const password = ref('')
const isLoading = ref(false)
const error = ref('')

const route = useRoute();
const callbackURL = route.query.callbackUrl as string || '/dashboard';

const handleSignIn = async () => {
    if (!email.value || !password.value) {
        error.value = 'Please fill in all fields'
        return
    }

    isLoading.value = true
    error.value = ''

    try {
        await authClient.signIn.email({
            email: email.value,
            password: password.value,
            callbackURL,
            fetchOptions: {
                onResponse: async (context) => {
                    isLoading.value = false
                    if (!context.response.ok) {
                        error.value = await context.response.text() || 'Invalid credentials'
                    }
                }
            }
        })
    } catch (e) {
        isLoading.value = false
        error.value = 'An error occurred during sign in'
    }
}
</script>

<template>
    <div class="min-h-screen bg-default flex items-center justify-center px-6">
        <div class="w-full max-w-sm">
            <div class="text-center mb-8">
                <h1 class="text-2xl font-bold text-highlighted">Welcome to <span class="text-primary">Lynx</span></h1>
                <p class="text-muted mt-1">Sign in to access your devices</p>
            </div>

            <UCard>
                <form @submit.prevent="handleSignIn" class="space-y-4">
                    <UFormGroup label="Email" name="email">
                        <UInput 
                            v-model="email" 
                            type="email" 
                            placeholder="admin@localhost"
                            autofocus
                        />
                    </UFormGroup>

                    <UFormGroup label="Password" name="password">
                        <UInput 
                            v-model="password" 
                            type="password" 
                            placeholder="Current password"
                        />
                    </UFormGroup>

                    <UAlert
                        v-if="error"
                        icon="i-heroicons-exclamation-triangle"
                        color="red"
                        variant="subtle"
                        :title="error"
                        class="mb-4"
                    />

                    <UButton 
                        type="submit"
                        block 
                        size="lg" 
                        :loading="isLoading"
                    >
                        Sign In
                    </UButton>
                </form>
            </UCard>
        </div>
    </div>
</template>
