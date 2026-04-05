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

    const id = event.context.params?.id;
    if (!id) {
        throw createError({
            statusCode: 400,
            statusMessage: "ID is required",
        });
    }

    const userId = session.user.id;

    const result = await db
        .delete(webhooks)
        .where(and(eq(webhooks.id, parseInt(id)), eq(webhooks.userId, userId)))
        .run();

    await logAudit(event, "webhook_delete", { id: parseInt(id) });

    return { success: true };
});
