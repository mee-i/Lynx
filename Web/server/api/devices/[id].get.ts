
import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { devices } from "@@/db/schema";
import { eq, and } from "drizzle-orm";

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

    const id = getRouterParam(event, 'id');

    if (!id) {
        throw createError({ statusCode: 400, statusMessage: "Missing ID" });
    }

    const device = await db.select().from(devices).where(and(eq(devices.id, id), eq(devices.userId, session.user.id)));

    if (!device) {
        throw createError({ statusCode: 404, statusMessage: "Device not found" });
    }

    return device;
})
