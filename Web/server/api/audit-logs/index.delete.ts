import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { auditLog } from "@@/db/schema";

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

    // RBAC: Check user role - Only admin can delete logs
    // @ts-ignore
    const userRole = session.user.role || 'user';
    if (userRole !== 'admin') {
        throw createError({
            statusCode: 403,
            statusMessage: "Forbidden: Only admins can clear audit logs",
        });
    }

    try {
        // Delete all audit logs
        await db.delete(auditLog).run();
        
        return { success: true, message: "Audit logs cleared" };
    } catch (e) {
        console.error("Failed to clear audit logs:", e);
        throw createError({
            statusCode: 500,
            statusMessage: "Failed to clear audit logs",
        });
    }
});
