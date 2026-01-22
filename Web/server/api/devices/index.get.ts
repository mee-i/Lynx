
import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { devices } from "@@/db/schema";
import { eq } from "drizzle-orm";

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

    const userDevices = await db.select().from(devices).where(eq(devices.userId, session.user.id));

    return userDevices;
})
