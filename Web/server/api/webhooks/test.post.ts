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
            statusCode: 400,
            statusMessage: "URL is required",
        });
    }

    try {
        // Validate URL
        new URL(body.url);

        const payload = {
            event: "webhook.test",
            timestamp: Date.now(),
            message: "This is a test notification from Lynx RMM",
            userId: session.user.id,
        };

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
