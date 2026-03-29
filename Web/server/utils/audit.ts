import { db } from "@@/db";
import { auditLog } from "@@/db/schema";
import { auth } from "@@/lib/auth";
import type { H3Event } from "h3";

export async function logAudit(
    event: H3Event,
    action: string,
    payload?: Record<string, any>,
    deviceId?: string,
) {
    const session = await auth.api.getSession({
        headers: event.headers,
    });

    if (!session) return;

    const ipAddress =
        getRequestHeader(event, "x-forwarded-for")?.split(",")[0]?.trim() ||
        getRequestHeader(event, "x-real-ip") ||
        "unknown";

    db.insert(auditLog)
        .values({
            userId: session.user.id,
            action,
            deviceId: deviceId || null,
            payload: payload ? JSON.stringify(payload) : null,
            ipAddress,
            timestamp: new Date(),
        })
        .run();
}
