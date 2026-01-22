
import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { devices } from "@@/db/schema";

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
    
    if (!body.name) {
        throw createError({
            statusCode: 400,
            statusMessage: "Name is required",
        });
    }

    const newDevice = {
        id: crypto.randomUUID(),
        userId: session.user.id,
        name: body.name,
        status: "offline" as const,
        createdAt: new Date(),
        updatedAt: new Date(),
    };

    await db.insert(devices).values(newDevice);

    return newDevice;
})
