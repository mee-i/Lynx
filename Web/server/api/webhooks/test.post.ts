import { auth } from "@@/lib/auth";

export default defineEventHandler(async (event) => {
    const session = await auth.api.getSession({
        headers: event.headers,
    });

    if (!session) {
        throw createError({
            statusCode: 401,
            statusMessage: "Unauthorized",
        });
    }

    const body = await readBody(event);
    if (!body.url) {
        throw createError({
            statusCode: 422,
            statusMessage: "URL is required",
        });
    }

    try {
        new URL(body.url);
    } catch {
        throw createError({
            statusCode: 422,
            statusMessage: "Invalid URL format",
        });
    }

    const payload = {
        event: "device.connect",
        timestamp: Date.now(),
        device: {
            id: "test-device-id",
            userId: session.user.id,
            name: "DESKTOP-TEST01",
            status: "offline",
            lastSeen: new Date().toISOString(),
            group: null,
            tags: null,
            os: "Windows 10 Enterprise",
            version: "1.0.0",
            createdAt: new Date(Date.now() - 7 * 24 * 60 * 60 * 1000).toISOString(),
            updatedAt: new Date().toISOString(),
            uptime: 168024062,
        },
    };

    try {
        const response = await fetch(body.url, {
            method: "POST",
            headers: {
                "Content-Type": "application/json",
            },
            body: JSON.stringify(payload),
        });

        if (!response.ok) {
            throw new Error(`Target responded with status ${response.status}`);
        }

        return { success: true };
    } catch (err: any) {
        throw createError({
            statusCode: 400,
            statusMessage: err.message || "Failed to send test webhook",
        });
    }
});