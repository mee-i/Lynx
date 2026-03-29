import { serve } from "bun";
import type { ServerWebSocket } from "bun";
import { readdir } from "node:fs/promises";
import {
    db,
    devices,
    deviceLogs,
    deviceMetrics,
    sessions,
    auditLog,
    type Device,
} from "./db";
import { eq, and, gt, sql, inArray } from "drizzle-orm";

const deviceSockets = new Map<string, ServerWebSocket<WebSocketData>>();
const clients = new Map<string, ServerWebSocket<WebSocketData>>();
const subscriptions = new Map<string, Set<ServerWebSocket<WebSocketData>>>();

// In-memory cache for device registry (synced with DB)
export const deviceRegistry = new Map<string, Device>();

// Load devices from SQLite on startup
async function loadDevices() {
    try {
        const allDevices = db.select().from(devices).all();
        for (const d of allDevices) {
            deviceRegistry.set(d.id, { ...d, status: "offline" });
        }
        console.log(`Loaded ${deviceRegistry.size} devices from SQLite`);
    } catch (e) {
        console.error("Failed to load devices:", e);
    }
}

// Log device event
async function logDeviceEvent(
    deviceId: string,
    type: "connect" | "disconnect" | "reconnect",
    message?: string,
) {
    try {
        db.insert(deviceLogs)
            .values({
                deviceId,
                type,
                message,
                timestamp: new Date(),
            })
            .run();
        console.log(
            `[log] ${deviceId} ${type}${message ? `: ${message}` : ""}`,
        );
    } catch (e) {
        console.error("Failed to log device event:", e);
    }
}

// Upsert device to SQLite
async function upsertDevice(
    device: Partial<Device> & { id: string; userId?: string | null },
    auditContext?: { ipAddress: string; userId?: string },
) {
    try {
        const existing = db
            .select()
            .from(devices)
            .where(eq(devices.id, device.id))
            .get();
        if (existing) {
            db.update(devices)
                .set({
                    ...device,
                    updatedAt: new Date(),
                })
                .where(eq(devices.id, device.id))
                .run();
        } else {
            // Use provided userId or fallback to the first user found in the DB
            let targetUserId = device.userId;
            if (!targetUserId) {
                const firstUser = db
                    .select({ id: devices.userId })
                    .from(sql`user`)
                    .limit(1)
                    .get() as { id: string } | undefined;
                targetUserId = firstUser?.id;
            }

            if (targetUserId) {
                db.insert(devices)
                    .values({
                        id: device.id,
                        userId: targetUserId,
                        name: device.name || device.id,
                        status: device.status || "offline",
                        lastSeen: device.lastSeen || new Date(),
                        group: device.group || null,
                        tags: device.tags || null,
                        os: device.os || null,
                        version: device.version || null,
                        createdAt: new Date(),
                        updatedAt: new Date(),
                    })
                    .run();
                console.log(
                    `[device] Registered ${device.id} to user ${targetUserId}`,
                );

                // Audit Log for Auto-Registration
                if (auditContext) {
                    insertAuditLog(
                        auditContext.userId || targetUserId,
                        "device_register",
                        auditContext.ipAddress,
                        device.id,
                        JSON.stringify({ name: device.name || device.id }),
                    );
                }
            } else {
                console.error(
                    `[device] Cannot register ${device.id}: No users found in database.`,
                );
            }
        }
    } catch (e) {
        console.error("Failed to upsert device:", e);
    }
}

// Load on startup
await loadDevices();

// Helper: Extract user from request cookies by validating session
async function getUserFromRequest(
    req: Request,
): Promise<{ userId: string; ipAddress: string } | null> {
    try {
        const cookieHeader = req.headers.get("cookie");
        if (!cookieHeader) {
            console.log("[Auth] No cookie header found");
            return null;
        }

        // Parse cookies to find 'better-auth.session_token'
        const cookies = Object.fromEntries(
            cookieHeader.split(";").map((c) => {
                const [key, ...val] = c.trim().split("=");
                return [key, val.join("=")];
            }),
        );

        let token = cookies["better-auth.session_token"];
        if (!token) {
            console.log("[Auth] No session token found in cookies");
            return null;
        }

        // Handle signed tokens (token.signature) - take the first part
        if (token.includes(".")) {
            token = token.split(".")[0];
        }

        // Validate session against DB
        const sess = db
            .select()
            .from(sessions)
            .where(
                and(
                    eq(sessions.token, token),
                    gt(sessions.expiresAt, new Date()),
                ),
            )
            .get();

        if (!sess) {
            console.log("[Auth] Session invalid or expired");
            return null;
        }

        // Extract IP from request headers
        const ipAddress =
            req.headers.get("x-forwarded-for")?.split(",")[0]?.trim() ||
            req.headers.get("x-real-ip") ||
            "unknown";

        return { userId: sess.userId, ipAddress };
    } catch (e) {
        console.error("Failed to validate session:", e);
        return null;
    }
}

// Helper: Insert audit log entry
function insertAuditLog(
    userId: string,
    action: string,
    ipAddress: string,
    deviceId?: string,
    payload?: string,
) {
    try {
        db.insert(auditLog)
            .values({
                userId,
                action,
                deviceId: deviceId || null,
                payload: payload || null,
                ipAddress,
                timestamp: new Date(),
            })
            .run();
        console.log(
            `[audit] ${userId} ${action}${deviceId ? ` device:${deviceId}` : ""}`,
        );
    } catch (e) {
        console.error("Failed to insert audit log:", e);
    }
}

type WebSocketData = {
    type: "device" | "client";
    id: string;
    name?: string;
    deviceId?: string;
    os?: string;
    version?: string;
    userId?: string;
};

const welcomingMessage = {
    type: "input",
    data:
        `@echo off & cls & echo. & echo. & echo. & echo. & ` +
        `echo          M                       LM          & ` +
        `echo          MML                    MMM          & ` +
        `echo          MNMML                MMOUM          & ` +
        `echo          MW UMM             LMN  VM          & ` +
        `echo          MR   NMM         MMMZ   RM          & ` +
        `echo          MPZ  UMMMMMMMMMMMMMMMN  OM          & ` +
        `echo          MMMMMMMMMMMMMMMMMMMMMMMOMM          & ` +
        `echo          MO TMMMMMMMMMMMMMMMMMM  OM          & ` +
        `echo          MO  ZMMMMMMMMMMMMMMMM   OM          & ` +
        `echo          MO  ZNMMMMMMMMMMMMMMM   OM          & ` +
        `echo          MOZ OMMMMMMMMMMMMMMMMMX OL          & ` +
        `echo          NOUMMMMMMMMMMMMMMMMMMMMMMM          & ` +
        `echo           MMMMMMMMMMMMMMMMMMMMMMMM           & ` +
        `echo            MMMMMMMMMMMMMMMMMMMMMM            & ` +
        `echo             MMMMMMMMMMMMMMMMMMMM             & ` +
        `echo               MMMMMMMMMMMMMMMM               & ` +
        `echo                  MMMMMMMMMM                  & ` +
        `echo. & echo. & echo. & ` +
        `echo ============================================ & ` +
        `echo                     Lynx & ` +
        `echo ============================================ & ` +
        `echo   Status    : Connected & ` +
        `echo. & ` +
        `echo   Type 'help' to list available commands & ` +
        `echo ============================================ & ` +
        `echo on\r`,
};
const server = serve<WebSocketData>({
    port: 9991,
    async fetch(req, server) {
        const url = new URL(req.url);

        if (url.pathname === "/api/devices" && req.method === "GET") {
            try {
                const dbDevices = db.select().from(devices).all();
                const deviceList = dbDevices.map((d) => ({
                    ...d,
                    status: (deviceSockets.has(d.id) ? "online" : "offline") as
                        | "online"
                        | "offline",
                    lastSeen: d.lastSeen || new Date(d.updatedAt),
                }));
                return new Response(JSON.stringify(deviceList), {
                    headers: {
                        "Content-Type": "application/json",
                        "Access-Control-Allow-Origin": "*",
                    },
                });
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Database error" }),
                    {
                        status: 500,
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    },
                );
            }
        }

        // POST /api/devices - Create Manual Device
        if (url.pathname === "/api/devices" && req.method === "POST") {
            try {
                const userInfo = await getUserFromRequest(req);
                if (!userInfo) {
                    return new Response("Unauthorized", { status: 401 });
                }

                const body = (await req.json()) as any;
                if (!body.name) {
                    return new Response("Name is required", { status: 400 });
                }

                const newDevice = {
                    id: crypto.randomUUID(),
                    userId: userInfo.userId, // Link to creator
                    name: body.name,
                    status: "offline" as const,
                    createdAt: new Date(),
                    updatedAt: new Date(),
                };

                db.insert(devices).values(newDevice).run();

                insertAuditLog(
                    userInfo.userId,
                    "device_create",
                    userInfo.ipAddress,
                    newDevice.id,
                    JSON.stringify({ name: newDevice.name }),
                );

                return new Response(JSON.stringify(newDevice), {
                    headers: {
                        "Content-Type": "application/json",
                        "Access-Control-Allow-Origin": "*",
                    },
                });
            } catch (e) {
                return new Response(
                    JSON.stringify({ error: "Database error" }),
                    {
                        status: 500,
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    },
                );
            }
        }

        if (url.pathname.startsWith("/api/devices/")) {
            const parts = url.pathname.split("/");

            // Screenshots endpoint: /api/devices/{id}/screenshots
            if (
                parts.length === 5 &&
                parts[4] === "screenshots" &&
                req.method === "GET"
            ) {
                const deviceId = parts[3];
                const imagesDir = `images/${deviceId}`;
                try {
                    const files = await readdir(imagesDir);
                    const screenshots = files
                        .filter((f: string) => f.endsWith(".png"))
                        .sort()
                        .reverse();
                    return new Response(JSON.stringify(screenshots), {
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    });
                } catch {
                    return new Response(JSON.stringify([]), {
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    });
                }
            }

            // GET /api/devices/:id/logs
            if (
                parts.length === 5 &&
                parts[4] === "logs" &&
                req.method === "GET"
            ) {
                const deviceId = parts[3];
                if (!deviceId)
                    return new Response("Device ID missing", { status: 400 });
                try {
                    const logs = db
                        .select()
                        .from(deviceLogs)
                        .where(eq(deviceLogs.deviceId, deviceId))
                        .orderBy(sql`${deviceLogs.timestamp} DESC`)
                        .limit(50)
                        .all();
                    return new Response(JSON.stringify(logs), {
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    });
                } catch (e) {
                    return new Response(
                        JSON.stringify({ error: "Database error" }),
                        {
                            status: 500,
                            headers: {
                                "Content-Type": "application/json",
                                "Access-Control-Allow-Origin": "*",
                            },
                        },
                    );
                }
            }

            // GET /api/devices/:id/metrics
            if (
                parts.length === 5 &&
                parts[4] === "metrics" &&
                req.method === "GET"
            ) {
                const deviceId = parts[3];
                if (!deviceId)
                    return new Response("Device ID missing", { status: 400 });
                try {
                    // Get last 24h metrics
                    const oneDayAgo = new Date(
                        Date.now() - 24 * 60 * 60 * 1000,
                    );
                    const metrics = db
                        .select()
                        .from(deviceMetrics)
                        .where(
                            sql`${deviceMetrics.deviceId} = ${deviceId} AND ${deviceMetrics.timestamp} > ${oneDayAgo}`,
                        )
                        .orderBy(sql`${deviceMetrics.timestamp} ASC`)
                        .all();

                    // Downsample
                    const result =
                        metrics.length > 200
                            ? metrics.filter(
                                  (_, i) =>
                                      i % Math.ceil(metrics.length / 200) === 0,
                              )
                            : metrics;

                    return new Response(JSON.stringify(result), {
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    });
                } catch (e) {
                    return new Response(
                        JSON.stringify({ error: "Database error" }),
                        {
                            status: 500,
                            headers: {
                                "Content-Type": "application/json",
                                "Access-Control-Allow-Origin": "*",
                            },
                        },
                    );
                }
            }

            const id = parts[3] || ""; // /api/devices/:id

            // GET /api/devices/:id
            if (parts.length === 4 && req.method === "GET") {
                try {
                    const device = db
                        .select()
                        .from(devices)
                        .where(eq(devices.id, id))
                        .get();
                    if (!device) {
                        return new Response(
                            JSON.stringify({ error: "Device not found" }),
                            {
                                status: 404,
                                headers: {
                                    "Content-Type": "application/json",
                                    "Access-Control-Allow-Origin": "*",
                                },
                            },
                        );
                    }
                    const enriched = {
                        ...device,
                        status: (deviceSockets.has(device.id)
                            ? "online"
                            : "offline") as "online" | "offline",
                        lastSeen: device.lastSeen || new Date(device.updatedAt),
                    };
                    return new Response(JSON.stringify(enriched), {
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    });
                } catch (e) {
                    return new Response(
                        JSON.stringify({ error: "Database error" }),
                        {
                            status: 500,
                            headers: {
                                "Content-Type": "application/json",
                                "Access-Control-Allow-Origin": "*",
                            },
                        },
                    );
                }
            }

            // POST /api/devices/:id - Update Device
            if (req.method === "POST") {
                try {
                    const device = db
                        .select()
                        .from(devices)
                        .where(eq(devices.id, id))
                        .get();
                    if (!device) {
                        return new Response(
                            JSON.stringify({ error: "Device not found" }),
                            {
                                status: 404,
                                headers: {
                                    "Content-Type": "application/json",
                                    "Access-Control-Allow-Origin": "*",
                                },
                            },
                        );
                    }

                    const body = (await req.json()) as any;
                    const updates: Partial<Device> = {};
                    if (body.name) updates.name = body.name;
                    if (body.group !== undefined) updates.group = body.group;
                    if (body.tags !== undefined) updates.tags = body.tags;

                    // Persist changes
                    await upsertDevice({ id, ...updates });

                    // Audit log
                    const userInfo = await getUserFromRequest(req);
                    if (userInfo) {
                        insertAuditLog(
                            userInfo.userId,
                            "device_update",
                            userInfo.ipAddress,
                            id,
                            JSON.stringify(updates),
                        );
                    }

                    return new Response(
                        JSON.stringify({ ...device, ...updates }),
                        {
                            headers: {
                                "Content-Type": "application/json",
                                "Access-Control-Allow-Origin": "*",
                            },
                        },
                    );
                } catch (e) {
                    return new Response(
                        JSON.stringify({ error: "Invalid JSON or DB error" }),
                        {
                            status: 400,
                            headers: {
                                "Content-Type": "application/json",
                                "Access-Control-Allow-Origin": "*",
                            },
                        },
                    );
                }
            }

            // DELETE /api/devices/:id - Delete Device
            if (req.method === "DELETE") {
                try {
                    const existing = db
                        .select()
                        .from(devices)
                        .where(eq(devices.id, id))
                        .get();
                    if (!existing) {
                        return new Response(
                            JSON.stringify({ error: "Device not found" }),
                            {
                                status: 404,
                                headers: {
                                    "Content-Type": "application/json",
                                    "Access-Control-Allow-Origin": "*",
                                },
                            },
                        );
                    }

                    // Audit log before deletion
                    const userInfo = await getUserFromRequest(req);
                    if (userInfo) {
                        insertAuditLog(
                            userInfo.userId,
                            "device_delete",
                            userInfo.ipAddress,
                            id,
                            JSON.stringify({ name: existing.name }),
                        );
                    }

                    db.delete(devices).where(eq(devices.id, id)).run();
                    return new Response(JSON.stringify({ success: true }), {
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    });
                } catch (e) {
                    return new Response(
                        JSON.stringify({ error: "Database error" }),
                        {
                            status: 500,
                            headers: {
                                "Content-Type": "application/json",
                                "Access-Control-Allow-Origin": "*",
                            },
                        },
                    );
                }
            }
        }

        // POST /api/devices/bulk/attributes - Bulk Update Attributes
        if (
            url.pathname === "/api/devices/bulk/attributes" &&
            req.method === "POST"
        ) {
            try {
                const userInfo = await getUserFromRequest(req);
                if (!userInfo) {
                    return new Response("Unauthorized", { status: 401 });
                }

                const body = (await req.json()) as {
                    deviceIds: string[];
                    group?: string;
                    tags?: string[];
                };

                if (
                    !body.deviceIds ||
                    !Array.isArray(body.deviceIds) ||
                    body.deviceIds.length === 0
                ) {
                    return new Response("Invalid deviceIds", { status: 400 });
                }

                const updates: Partial<Device> = { updatedAt: new Date() };
                if (body.group !== undefined) updates.group = body.group;

                if (body.tags && Array.isArray(body.tags) && body.tags.length > 0) {
                    // We need to merge tags for each device individually since SQLite JSON operations
                    // in Drizzle can be tricky for arrays in a simple update.
                    const targetDevices = db.select({ id: devices.id, tags: devices.tags }).from(devices).where(inArray(devices.id, body.deviceIds)).all();
                    for (const d of targetDevices) {
                        const existingTags = Array.isArray(d.tags) ? d.tags : [];
                        const mergedTags = Array.from(new Set([...existingTags, ...body.tags]));
                        db.update(devices).set({ tags: mergedTags, updatedAt: new Date() }).where(eq(devices.id, d.id)).run();
                    }
                }

                if (Object.keys(updates).length > 1) { // More than just updatedAt
                    await db
                        .update(devices)
                        .set(updates)
                        .where(inArray(devices.id, body.deviceIds))
                        .run();
                }

                insertAuditLog(
                    userInfo.userId,
                    "bulk_update_attributes",
                    userInfo.ipAddress,
                    "bulk",
                    JSON.stringify({
                        count: body.deviceIds.length,
                        updates,
                        ids: body.deviceIds,
                    }),
                );

                return new Response(JSON.stringify({ success: true }), {
                    headers: {
                        "Content-Type": "application/json",
                        "Access-Control-Allow-Origin": "*",
                    },
                });
            } catch (e) {
                console.error("Bulk attributes error:", e);
                return new Response(
                    JSON.stringify({ error: "Database error" }),
                    {
                        status: 500,
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    },
                );
            }
        }

        // POST /api/devices/bulk/actions - Bulk Actions (Power/Command)
        if (
            url.pathname === "/api/devices/bulk/actions" &&
            req.method === "POST"
        ) {
            try {
                const userInfo = await getUserFromRequest(req);
                if (!userInfo) {
                    return new Response("Unauthorized", { status: 401 });
                }

                const body = (await req.json()) as {
                    deviceIds: string[];
                    action: "restart" | "shutdown" | "command";
                    payload?: string;
                };

                if (
                    !body.deviceIds ||
                    !Array.isArray(body.deviceIds) ||
                    body.deviceIds.length === 0
                ) {
                    return new Response("Invalid deviceIds", { status: 400 });
                }

                const results: {
                    deviceId: string;
                    status: "sent" | "offline" | "failed";
                }[] = [];
                let successCount = 0;

                for (const deviceId of body.deviceIds) {
                    const ws = deviceSockets.get(deviceId);
                    if (ws) {
                        try {
                            if (body.action === "command") {
                                ws.send(
                                    JSON.stringify({
                                        type: "input", // Sending as input for now, acting as a command injection
                                        data: body.payload + "\r",
                                    }),
                                );
                            } else if (body.action === "restart") {
                                // Send specific restart command or script
                                ws.send(
                                    JSON.stringify({
                                        type: "input",
                                        data: "shutdown /r /t 0\r",
                                    }),
                                );
                            } else if (body.action === "shutdown") {
                                ws.send(
                                    JSON.stringify({
                                        type: "input",
                                        data: "shutdown /s /t 0\r",
                                    }),
                                );
                            }
                            results.push({ deviceId, status: "sent" });
                            successCount++;
                        } catch (e) {
                            results.push({ deviceId, status: "failed" });
                        }
                    } else {
                        results.push({ deviceId, status: "offline" });
                    }
                }

                insertAuditLog(
                    userInfo.userId,
                    "bulk_action",
                    userInfo.ipAddress,
                    "bulk",
                    JSON.stringify({
                        action: body.action,
                        count: body.deviceIds.length,
                        success: successCount,
                        payload: body.payload,
                    }),
                );

                return new Response(
                    JSON.stringify({
                        total: body.deviceIds.length,
                        success: successCount,
                        results,
                    }),
                    {
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    },
                );
            } catch (e) {
                console.error("Bulk action error:", e);
                return new Response(
                    JSON.stringify({ error: "Server error" }),
                    {
                        status: 500,
                        headers: {
                            "Content-Type": "application/json",
                            "Access-Control-Allow-Origin": "*",
                        },
                    },
                );
            }
        }

        // Static file serving for images
        if (url.pathname.startsWith("/images/") && req.method === "GET") {
            const filePath = `.${url.pathname}`;
            const file = Bun.file(filePath);
            if (await file.exists()) {
                return new Response(file);
            }
            return new Response("Not found", { status: 404 });
        }

        if (req.method === "OPTIONS") {
            return new Response(null, {
                headers: {
                    "Access-Control-Allow-Origin": "*",
                    "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
                    "Access-Control-Allow-Headers": "Content-Type",
                },
            });
        }

        const type = url.searchParams.get("type") as "device" | "client";
        const id = url.searchParams.get("id");
        const name = url.searchParams.get("name") || id || "Unknown Device";
        const os = url.searchParams.get("os") || undefined;
        const version = url.searchParams.get("version") || undefined;
        const userId = url.searchParams.get("userId") || undefined;

        if (!type || !id) {
            return new Response("Missing type or id", { status: 400 });
        }

        const success = server.upgrade(req, {
            data: { type, id, name, os, version, userId },
        });

        return success
            ? undefined
            : new Response("WebSocket upgrade error", { status: 400 });
    },
    websocket: {
        async open(ws) {
            const { type, id, name, os, version, userId } = ws.data;
            console.log(`[${type}] connected: ${id} (${name})`);

            if (type === "device") {
                deviceSockets.set(id, ws);

                // Auto-register occurs inside upsertDevice if missing
                const updates = {
                    id, // pass ID for lookup/insert
                    userId, // manual linking if provided
                    name: name || id,
                    status: "online" as const,
                    lastSeen: new Date(),
                    os: os || null,
                    version: version || null,
                };

                // Sync with DB (will create if missing now)
                await upsertDevice(updates, {
                    ipAddress: ws.remoteAddress || "unknown",
                    userId: userId,
                });

                // Get the final device state (either existing or newly created)
                const finalDevice = db
                    .select()
                    .from(devices)
                    .where(eq(devices.id, id))
                    .get();
                if (!finalDevice) {
                    console.warn(
                        `[device] Failed to register/find device: ${id}`,
                    );
                    ws.close(1008, "Registration failed");
                    return;
                }

                // Sync with in-memory registry
                const wasOffline =
                    !deviceRegistry.get(id) ||
                    deviceRegistry.get(id)?.status === "offline";
                deviceRegistry.set(id, { ...finalDevice, status: "online" });

                // Log event
                await logDeviceEvent(id, wasOffline ? "connect" : "reconnect");

                const subs = subscriptions.get(id);
                if (subs) {
                    subs.forEach((client) =>
                        client.send(
                            JSON.stringify({
                                type: "status",
                                status: "online",
                                deviceId: id,
                            }),
                        ),
                    );
                }
            } else {
                clients.set(id, ws);
            }
        },
        async message(ws, message) {
            const { type, id } = ws.data;

            console.log(`[${type}] received message: ${message}`);

            try {
                const msg = JSON.parse(
                    typeof message === "string" ? message : message.toString(),
                );

                if (type === "client") {
                    if (msg.type === "subscribe") {
                        const targetDeviceId = msg.deviceId;
                        if (!subscriptions.has(targetDeviceId)) {
                            subscriptions.set(targetDeviceId, new Set());
                        }
                        subscriptions.get(targetDeviceId)?.add(ws);
                        ws.data.deviceId = targetDeviceId;
                        console.log(
                            `Client ${id} subscribed to ${targetDeviceId}`,
                        );

                        // Send current device status
                        const isOnline = deviceSockets.has(targetDeviceId);
                        ws.send(
                            JSON.stringify({
                                type: "status",
                                status: isOnline ? "online" : "offline",
                                deviceId: targetDeviceId,
                            }),
                        );

                        if (isOnline) {
                            console.log("Sending test command to device");
                            const deviceWs = deviceSockets.get(targetDeviceId);
                            if (deviceWs) {
                                deviceWs.send(JSON.stringify(welcomingMessage));
                            }
                        }
                    } else if (
                        msg.type === "input" ||
                        msg.type === "resize" ||
                        msg.type === "action" ||
                        msg.type === "command" ||
                        msg.type === "filesystem"
                    ) {
                        const targetDeviceId = ws.data.deviceId;
                        if (targetDeviceId) {
                            const deviceWs = deviceSockets.get(targetDeviceId);
                            if (deviceWs) {
                                deviceWs.send(JSON.stringify(msg));
                            }
                        }
                    } else if (msg.type === "hello") {
                        const targetDeviceId = ws.data.deviceId;
                        if (targetDeviceId) {
                            const deviceWs = deviceSockets.get(targetDeviceId);
                            if (deviceWs) {
                                deviceWs.send(JSON.stringify(welcomingMessage));
                            }
                        }
                    }
                    const { userId } = ws.data;
                    if (
                        userId
                    ) {
                        console.log("USER ID ===================", userId);
                        const targetDeviceId = ws.data.deviceId;
                        insertAuditLog(
                            userId,
                            msg.type,
                            "unknown",
                            targetDeviceId,
                            JSON.stringify({ msg: JSON.stringify(msg) }),
                        );
                    }
                } else if (type === "device") {
                    console.log(
                        `[Device Msg] From ${id}: ${JSON.stringify(msg).substring(0, 100)}...`,
                    );

                    if (msg.type === "output") {
                        const subs = subscriptions.get(id);
                        if (subs) {
                            subs.forEach((client) =>
                                client.send(
                                    JSON.stringify({
                                        type: "output",
                                        output: msg.output,
                                    }),
                                ),
                            );
                        }
                    } else if (msg.type === "ping") {
                        ws.send(JSON.stringify({ type: "pong" }));
                        const now = new Date();
                        const device = deviceRegistry.get(id);
                        if (device) {
                            device.lastSeen = now;
                            const updates: any = { id, lastSeen: now };
                            if (msg.uptime) {
                                device.uptime = msg.uptime;
                                updates.uptime = msg.uptime;
                            }
                            upsertDevice(updates);
                        }
                    } else if (msg.type === "metrics") {
                        if (msg.data) {
                            try {
                                db.insert(deviceMetrics)
                                    .values({
                                        deviceId: id,
                                        cpuUsage: Math.round(msg.data.cpu),
                                        ramUsage: Math.round(msg.data.ram),
                                        diskUsage: Math.round(msg.data.disk),
                                        networkUp: Math.round(msg.data.netUp),
                                        networkDown: Math.round(
                                            msg.data.netDown,
                                        ),
                                        timestamp: new Date(),
                                    })
                                    .run();
                            } catch (e) {
                                console.error("Failed to save metrics:", e);
                            }

                            const subs = subscriptions.get(id);
                            if (subs) {
                                subs.forEach((client) =>
                                    client.send(
                                        JSON.stringify({
                                            type: "metrics",
                                            data: msg.data,
                                            deviceId: id,
                                        }),
                                    ),
                                );
                            }
                        }
                    } else if (msg.type === "screenshot" && msg.data) {
                        const buffer = Buffer.from(msg.data, "base64");
                        const timestamp = Date.now();
                        const dir = `images/${id}`;
                        await Bun.write(`${dir}/${timestamp}.png`, buffer);
                        console.log(
                            `[Screenshot] Saved ${dir}/${timestamp}.png`,
                        );

                        const subs = subscriptions.get(id);
                        if (subs) {
                            subs.forEach((client) =>
                                client.send(
                                    JSON.stringify({
                                        type: "screenshot_saved",
                                        url: `/images/${id}/${timestamp}.png`,
                                        filename: `${timestamp}.png`,
                                    }),
                                ),
                            );
                        }
                    } else if (msg.type === "filesystem") {
                        // Relay filesystem responses back to subscribed clients
                        const subs = subscriptions.get(id);
                        if (subs) {
                            subs.forEach((client) =>
                                client.send(JSON.stringify(msg)),
                            );
                        }
                    }
                }
            } catch (err) {
                console.error("Failed to parse message", err);
            }
        },
        async close(ws) {
            const { type, id } = ws.data;
            console.log(`[${type}] disconnected: ${id}`);

            if (type === "device") {
                deviceSockets.delete(id);

                const now = new Date();
                const device = deviceRegistry.get(id);
                if (device) {
                    device.status = "offline";
                    device.lastSeen = now;
                    upsertDevice({ id, status: "offline", lastSeen: now });
                }

                // Log event
                await logDeviceEvent(id, "disconnect");

                const subs = subscriptions.get(id);
                if (subs) {
                    subs.forEach((client) =>
                        client.send(
                            JSON.stringify({
                                type: "status",
                                status: "offline",
                                deviceId: id,
                            }),
                        ),
                    );
                }
            } else {
                clients.delete(id);
                subscriptions.forEach((set) => set.delete(ws));
            }
        },
    },
});

console.log(`🚀 Lynx Server listening on localhost:${server.port}`);
