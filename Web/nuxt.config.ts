// https://nuxt.com/docs/api/configuration/nuxt-config
import tailwindcss from "@tailwindcss/vite";

export default defineNuxtConfig({
    compatibilityDate: "2025-07-15",
    devtools: { enabled: true },
    modules: ["@nuxt/ui"],
    css: ["./app/assets/css/main.css"],
    routeRules: {
        "/lynx/**": {
            proxy: "http://localhost:9991/**",
            cors: true,
        },
    },

    runtimeConfig: {
        public: {
            base_url: process.env.BASE_URL || "localhost:3000",
            websocket_url: process.env.WEBSOCKET_URL || "localhost:9991",
        },
    },
    nitro: {
        preset: "node-server",
        minify: true,
        sourceMap: false,
    },
    vite: {
        plugins: [tailwindcss()],
    },
});
