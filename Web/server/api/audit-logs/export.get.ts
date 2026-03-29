import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { auditLog, user } from "@@/db/schema";
import { eq, and, gte, lte, desc } from "drizzle-orm";

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

    const query = getQuery(event);
    
    // RBAC: Check user role
    // @ts-ignore
    const userRole = session.user.role || 'user';
    
    let filterUserId = query.userId as string | undefined;

    // If not admin, force filter to current user
    if (userRole !== 'admin') {
        filterUserId = session.user.id;
    }

    const filterAction = query.action as string | undefined;
    const filterStartDate = query.startDate as string | undefined;
    const filterEndDate = query.endDate as string | undefined;

    const conditions: any[] = [];

    if (filterUserId) {
        conditions.push(eq(auditLog.userId, filterUserId));
    }
    if (filterAction) {
        conditions.push(eq(auditLog.action, filterAction));
    }
    if (filterStartDate) {
        conditions.push(gte(auditLog.timestamp, new Date(filterStartDate)));
    }
    if (filterEndDate) {
        conditions.push(lte(auditLog.timestamp, new Date(filterEndDate)));
    }

    const whereClause = conditions.length > 0 ? and(...conditions) : undefined;

    const logs = db
        .select({
            id: auditLog.id,
            userId: auditLog.userId,
            userName: user.name,
            deviceId: auditLog.deviceId,
            action: auditLog.action,
            payload: auditLog.payload,
            ipAddress: auditLog.ipAddress,
            timestamp: auditLog.timestamp,
        })
        .from(auditLog)
        .leftJoin(user, eq(auditLog.userId, user.id))
        .where(whereClause)
        .orderBy(desc(auditLog.timestamp))
        .all();

    // Build CSV
    const headers = ["ID", "User", "User ID", "Action", "Device ID", "Payload", "IP Address", "Timestamp"];
    const rows = logs.map((log) => [
        log.id,
        `"${(log.userName || "").replace(/"/g, '""')}"`,
        log.userId,
        log.action,
        log.deviceId || "",
        `"${(log.payload || "").replace(/"/g, '""')}"`,
        log.ipAddress || "",
        log.timestamp ? new Date(log.timestamp).toISOString() : "",
    ]);

    const csv = [headers.join(","), ...rows.map((r) => r.join(","))].join("\n");

    setResponseHeaders(event, {
        "Content-Type": "text/csv",
        "Content-Disposition": `attachment; filename="audit-logs-${new Date().toISOString().split("T")[0]}.csv"`,
    });

    return csv;
});
