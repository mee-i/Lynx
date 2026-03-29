import { auth } from "@@/lib/auth";
import { db } from "@@/db";
import { auditLog, user, devices } from "@@/db/schema";
import { eq, and, gte, lte, sql, desc } from "drizzle-orm";

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
    const page = Math.max(1, Number(query.page) || 1);
    const limit = Math.min(100, Math.max(1, Number(query.limit) || 20));
    const offset = (page - 1) * limit;

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

    // Build conditions
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

    // Get total count
    const countResult = db
        .select({ count: sql<number>`count(*)` })
        .from(auditLog)
        .where(whereClause)
        .get();
    const total = countResult?.count || 0;

    // Get logs with user name joined
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
        .limit(limit)
        .offset(offset)
        .all();

    // Get distinct actions for filter dropdown
    const actions = db
        .selectDistinct({ action: auditLog.action })
        .from(auditLog)
        .all()
        .map((a) => a.action);

    return {
        data: logs,
        pagination: {
            page,
            limit,
            total,
            totalPages: Math.ceil(total / limit),
        },
        actions,
    };
});
