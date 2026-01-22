<script setup lang="ts">
import type { NavigationMenuItem } from "@nuxt/ui";

const items: NavigationMenuItem[] = [
    {
        label: "Home",
        icon: "i-material-symbols-dashboard-rounded",
        to: "/dashboard",
    },
    {
        label: "Devices",
        icon: "i-material-symbols-devices",
        to: "/dashboard/devices",
    },
    {
        label: "Logout",
        icon: "i-material-symbols-admin",
        to: "/logout",
    }
];
</script>
<template>
    <UDashboardGroup>
        <UDashboardSidebar collapsible resizable>
            <template #header="{ collapsed }">
                <div
                    class="flex flex-row gap-2 items-center cursor-pointer w-full"
                    :class="{ 'justify-center': collapsed }"
                    @click="navigateTo('/dashboard/admin')"
                >
                    <img src="/logo.png" alt="Logo" width="45" height="45" />
                    <h1
                        v-if="!collapsed"
                        class="text-2xl font-bold text-primary truncate"
                    >
                        Lynx
                    </h1>
                </div>
            </template>

            <template #default="{ collapsed }">
                <UNavigationMenu
                    :collapsed="collapsed"
                    :items="items"
                    orientation="vertical"
                />
            </template>

            <template #footer="{ collapsed }">
                <UColorModeSelect />
            </template>
        </UDashboardSidebar>
        <UDashboardPanel grow>
            <UDashboardNavbar>
            </UDashboardNavbar>

            <div class="p-4 overflow-y-auto w-full h-full">
                <slot />
            </div>
        </UDashboardPanel>
    </UDashboardGroup>
</template>
