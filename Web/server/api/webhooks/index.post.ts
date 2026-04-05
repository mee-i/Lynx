import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { webhooks } from "@@/db/schema";
import { eq, and } from "drizzle-orm";
import { logAudit } from "@@/server/utils/audit";

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
    const userId = session.user.id;

    if (!body.url || !body.events || !Array.isArray(body.events)) {
        throw createError({
            statusCode: 400,
            statusMessage: "URL and events are required",
        });
    }

    const { id, url, events, isEnabled } = body;

    if (id) {
        // Update
        const updated = await db
            .update(webhooks)
            .set({
                url,
                events,
                isEnabled: isEnabled ?? true,
                updatedAt: new Date(),
            })
            .where(and(eq(webhooks.id, id), eq(webhooks.userId, userId)))
            .run();
        
        await logAudit(event, "webhook_update", { id, url, events, isEnabled });
        
        return { success: true };
    } else {
        // Create
        const result = await db
            .insert(webhooks)
            .values({
                userId,
                url,
                events,
                isEnabled: isEnabled ?? true,
                createdAt: new Date(),
                updatedAt: new Date(),
            })
            .run();
        
        const newId = Number(result.lastInsertRowid);
        await logAudit(event, "webhook_create", { id: newId, url, events, isEnabled });

        return { success: true, id: newId };
    }
});
