import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { webhooks } from "@@/db/schema";
import { eq, desc } from "drizzle-orm";

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

    const userId = session.user.id;

    const data = await db
        .select()
        .from(webhooks)
        .where(eq(webhooks.userId, userId))
        .orderBy(desc(webhooks.createdAt))
        .all();

    return data;
});
