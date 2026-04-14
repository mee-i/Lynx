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
    webhooks,
    type Device,
} from "./db";
import { eq, and, gt, sql, inArray } from "drizzle-orm";

const deviceSockets = new Map<string, ServerWebSocket<WebSocketData>>();
const clients = new Map<string, ServerWebSocket<WebSocketData>>();
const subscriptions = new Map<string, Set<ServerWebSocket<WebSocketData>>>();

// Relay state for live streaming
interface MediaStream {
    videoClients: Set<ReadableStreamDefaultController>;
    audioClients: Set<ReadableStreamDefaultController>;
    videoBytesReceived: number;
    audioBytesReceived: number;
    videoBitrate: number; // bytes per second
    audioBitrate: number; // bytes per second
    lastUpdate: number;
}
const activeStreams = new Map<string, MediaStream>();

function getOrCreateStream(deviceId: string): MediaStream {
    if (!activeStreams.has(deviceId)) {
        activeStreams.set(deviceId, {
            videoClients: new Set(),
            audioClients: new Set(),
            videoBytesReceived: 0,
            audioBytesReceived: 0,
            videoBitrate: 0,
            audioBitrate: 0,
            lastUpdate: Date.now(),
        });
    }
    return activeStreams.get(deviceId)!;
}

// Periodic bitrate calculation (runs every 2 seconds)
setInterval(() => {
    const now = Date.now();
    for (const [deviceId, stream] of activeStreams) {
        const deltaSec = (now - stream.lastUpdate) / 1000;
        if (deltaSec > 0) {
            stream.videoBitrate = Math.round(stream.videoBytesReceived / deltaSec);
            stream.audioBitrate = Math.round(stream.audioBytesReceived / deltaSec);
            stream.videoBytesReceived = 0;
            stream.audioBytesReceived = 0;
            stream.lastUpdate = now;
        }
    }
}, 2000);

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

// Trigger Webhooks
async function triggerWebhooks(
    userId: string,
    event: "device.connect" | "device.disconnect",
    deviceData: any,
) {
    try {
        const hooks = db
            .select()
            .from(webhooks)
            .where(
                and(
                    eq(webhooks.userId, userId),
                    eq(webhooks.isEnabled, true),
                ),
            )
            .all();

        for (const hook of hooks) {
            const events = hook.events;
            if (events.includes(event)) {
                try {
                    // Basic URL validation
                    new URL(hook.url);

                    console.log(`[webhook] Triggering for ${hook.url} (${event})`);
                    fetch(hook.url, {
                        method: "POST",
                        headers: {
                            "Content-Type": "application/json",
                        },
                        body: JSON.stringify({
                            event,
                            timestamp: Date.now(),
                            device: deviceData,
                        }),
                    }).catch((err) => {
                        console.error(`[webhook] Request to ${hook.url} failed:`, err);
                    });
                } catch (urlErr) {
                    console.error(`[webhook] Invalid URL for hook ${hook.id}: ${hook.url}`);
                }
            }
        }
    } catch (e) {
        console.error("Failed to trigger webhooks:", e);
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
                
            // Update in-memory registry
            const reg = deviceRegistry.get(device.id);
            if (reg) {
                deviceRegistry.set(device.id, { ...reg, ...device, updatedAt: new Date() });
            }
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
                    
                // Update in-memory registry
                const allDevices = db.select().from(devices).where(eq(devices.id, device.id)).get();
                if (allDevices) {
                    deviceRegistry.set(device.id, { ...allDevices, status: device.status || "offline" });
                }
                
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

        // Find session token (handle prefixes like __Host- and __Secure-)
        let token = cookies["better-auth.session_token"] || 
                    cookies["__Host-better-auth.session_token"] || 
                    cookies["__Secure-better-auth.session_token"];

        if (!token) {
            console.log("[Auth] No session token found in cookies. Available cookies:", Object.keys(cookies).join(", "));
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

        // 1. Root/General Routes
        if (url.pathname === "/api/devices" && req.method === "GET") {
            try {
                const dbDevices = db.select().from(devices).all();
                const deviceList = dbDevices.map((d) => ({
                    ...d,
                    status: (deviceSockets.has(d.id) ? "online" : "offline") as "online" | "offline",
                    lastSeen: d.lastSeen || new Date(d.updatedAt),
                }));
                return new Response(JSON.stringify(deviceList), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
            } catch (e) {
                return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
            }
        }

        if (url.pathname === "/api/devices" && req.method === "POST") {
            try {
                const userInfo = await getUserFromRequest(req);
                if (!userInfo) return new Response("Unauthorized", { status: 401 });
                const body = (await req.json()) as any;
                if (!body.name) return new Response("Name is required", { status: 400 });

                const newDevice = {
                    id: crypto.randomUUID(),
                    userId: userInfo.userId,
                    name: body.name,
                    status: "offline" as const,
                    createdAt: new Date(),
                    updatedAt: new Date(),
                };
                db.insert(devices).values(newDevice).run();
                insertAuditLog(userInfo.userId, "device_create", userInfo.ipAddress, newDevice.id, JSON.stringify({ name: newDevice.name }));
                return new Response(JSON.stringify(newDevice), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
            } catch (e) {
                return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
            }
        }

        // 2. Sub-routes under /api/devices/
        if (url.pathname.startsWith("/api/devices/")) {
            const parts = url.pathname.split("/");

            // Priority 0: Live Streaming Routes (moved from below for better routing)
            if (parts[4] === "stream") {
                const deviceId = parts[3];
                const action = parts[5]; // media_up, video_down, audio_down, frame_up
                
                if (!deviceId || !action) return new Response("Bad request", { status: 400 });

                if (action === "frame_up" && req.method === "POST") {
                    const frame = new Uint8Array(await req.arrayBuffer());
                    const stream = getOrCreateStream(deviceId);
                    stream.videoBytesReceived += frame.length;

                    const boundary = `--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${frame.length}\r\n\r\n`;
                    const encoder = new TextEncoder();
                    const header = encoder.encode(boundary);
                    const footer = encoder.encode("\r\n");

                    stream.videoClients.forEach(controller => {
                        try {
                            controller.enqueue(header);
                            controller.enqueue(frame);
                            controller.enqueue(footer);
                        } catch (e) {
                            stream.videoClients.delete(controller);
                        }
                    });

                    return new Response("OK", { status: 200, headers: { "Access-Control-Allow-Origin": "*" } });
                }

                if (action === "audio_up" && req.method === "POST") {
                    const audioData = new Uint8Array(await req.arrayBuffer());
                    const stream = getOrCreateStream(deviceId);
                    stream.audioBytesReceived += audioData.length;

                    stream.audioClients.forEach(controller => {
                        try {
                            controller.enqueue(audioData);
                        } catch (e) {
                            stream.audioClients.delete(controller);
                        }
                    });

                    return new Response("OK", { status: 200, headers: { "Access-Control-Allow-Origin": "*" } });
                }

                if (action === "video_down" && req.method === "GET") {
                    const stream = getOrCreateStream(deviceId);
                    let controllerRef: any;
                    const body = new ReadableStream({
                        start(controller) {
                            controllerRef = controller;
                            stream.videoClients.add(controller);
                        },
                        cancel() {
                            if (controllerRef) stream.videoClients.delete(controllerRef);
                        }
                    });

                    return new Response(body, {
                        headers: {
                            "Content-Type": "multipart/x-mixed-replace; boundary=frame",
                            "Cache-Control": "no-cache",
                            "Connection": "keep-alive",
                            "Access-Control-Allow-Origin": "*",
                        }
                    });
                }

                if (action === "audio_down" && req.method === "GET") {
                    const stream = getOrCreateStream(deviceId);
                    let controllerRef: any;
                    const body = new ReadableStream({
                        start(controller) {
                            controllerRef = controller;
                            stream.audioClients.add(controller);
                        },
                        cancel() {
                            if (controllerRef) stream.audioClients.delete(controllerRef);
                        }
                    });

                    return new Response(body, {
                        headers: {
                            "Content-Type": "audio/l16; rate=44100; channels=1",
                            "Cache-Control": "no-cache",
                            "Connection": "keep-alive",
                            "Access-Control-Allow-Origin": "*",
                        }
                    });
                }
            }

            // Priority 1: Bulk Actions
            if (url.pathname === "/api/devices/bulk/attributes" && req.method === "POST") {
                try {
                    const userInfo = await getUserFromRequest(req);
                    if (!userInfo) return new Response("Unauthorized", { status: 401 });
                    const body = (await req.json()) as { deviceIds: string[]; group?: string; tags?: string[] };
                    if (!body.deviceIds?.length) return new Response("Invalid deviceIds", { status: 400 });

                    const updates: Partial<Device> = { updatedAt: new Date() };
                    if (body.group !== undefined) updates.group = body.group;

                    if (body.tags?.length) {
                        const targetDevices = db.select({ id: devices.id, tags: devices.tags }).from(devices).where(inArray(devices.id, body.deviceIds)).all();
                        for (const d of targetDevices) {
                            const existingTags = Array.isArray(d.tags) ? d.tags : [];
                            const mergedTags = Array.from(new Set([...existingTags, ...body.tags]));
                            db.update(devices).set({ tags: mergedTags, updatedAt: new Date() }).where(eq(devices.id, d.id)).run();
                        }
                    }

                    if (Object.keys(updates).length > 1) {
                        await db.update(devices).set(updates).where(inArray(devices.id, body.deviceIds)).run();
                    }

                    insertAuditLog(userInfo.userId, "bulk_update_attributes", userInfo.ipAddress, "bulk", JSON.stringify({ count: body.deviceIds.length, updates, ids: body.deviceIds }));
                    return new Response(JSON.stringify({ success: true }), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                } catch (e) {
                    return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                }
            }

            if (url.pathname === "/api/devices/bulk/actions" && req.method === "POST") {
                try {
                    const userInfo = await getUserFromRequest(req);
                    if (!userInfo) return new Response("Unauthorized", { status: 401 });
                    const body = (await req.json()) as { deviceIds: string[]; action: "restart" | "shutdown" | "command" | "update"; payload?: string };
                    if (!body.deviceIds?.length) return new Response("Invalid deviceIds", { status: 400 });

                    const results: { deviceId: string; status: "sent" | "offline" | "failed" }[] = [];
                    let successCount = 0;

                    for (const deviceId of body.deviceIds) {
                        const ws = deviceSockets.get(deviceId);
                        if (ws) {
                            try {
                                if (body.action === "update") {
                                    ws.send(JSON.stringify({ type: "action", action: "update", url: body.payload || "" }));
                                } else {
                                    const cmd = body.action === "command" ? (body.payload + "\r") : (body.action === "restart" ? "shutdown /r /t 0\r" : "shutdown /s /t 0\r");
                                    ws.send(JSON.stringify({ type: "input", data: cmd }));
                                }
                                results.push({ deviceId, status: "sent" });
                                successCount++;
                            } catch (e) { results.push({ deviceId, status: "failed" }); }
                        } else { results.push({ deviceId, status: "offline" }); }
                    }

                    insertAuditLog(userInfo.userId, "bulk_action", userInfo.ipAddress, "bulk", JSON.stringify({ action: body.action, count: body.deviceIds.length, success: successCount, payload: body.payload }));
                    return new Response(JSON.stringify({ total: body.deviceIds.length, success: successCount, results }), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                } catch (e) {
                    return new Response(JSON.stringify({ error: "Server error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                }
            }

            // Priority 2: Individual Device Actions (:id/...)
            const deviceIdPathParam = parts[3];
            if (!deviceIdPathParam) return new Response("Device ID missing", { status: 400 });

            if (parts[4] === "screenshots" && req.method === "GET") {
                try {
                    const files = await readdir(`images/${deviceIdPathParam}`);
                    const screenshots = files.filter((f: string) => f.endsWith(".png")).sort().reverse();
                    return new Response(JSON.stringify(screenshots), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                } catch { return new Response(JSON.stringify([]), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }}); }
            }

            if (parts[4] === "logs" && req.method === "GET") {
                try {
                    const logs = db.select().from(deviceLogs).where(eq(deviceLogs.deviceId, deviceIdPathParam)).orderBy(sql`${deviceLogs.timestamp} DESC`).limit(50).all();
                    return new Response(JSON.stringify(logs), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                } catch (e) { return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }}); }
            }

            if (parts[4] === "metrics" && req.method === "GET") {
                try {
                    const oneDayAgo = new Date(Date.now() - 24 * 60 * 60 * 1000);
                    const metrics = db.select().from(deviceMetrics).where(sql`${deviceMetrics.deviceId} = ${deviceIdPathParam} AND ${deviceMetrics.timestamp} > ${oneDayAgo}`).orderBy(sql`${deviceMetrics.timestamp} ASC`).all();
                    const downsampled = metrics.length > 200 ? metrics.filter((_, i) => i % Math.ceil(metrics.length / 200) === 0) : metrics;
                    return new Response(JSON.stringify(downsampled), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                } catch (e) { return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }}); }
            }

            // Priority 3: Base Device Actions (:id)
            if (parts.length === 4) {
                if (req.method === "GET") {
                    try {
                        const device = db.select().from(devices).where(eq(devices.id, deviceIdPathParam)).get();
                        if (!device) return new Response(JSON.stringify({ error: "Not found" }), { status: 404, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                        return new Response(JSON.stringify({ ...device, status: deviceSockets.has(device.id) ? "online" : "offline", lastSeen: device.lastSeen || new Date(device.updatedAt) }), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                    } catch (e) { return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }}); }
                }

                if (req.method === "POST") {
                    try {
                        const device = db.select().from(devices).where(eq(devices.id, deviceIdPathParam)).get();
                        if (!device) return new Response(JSON.stringify({ error: "Not found" }), { status: 404, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                        const body = (await req.json()) as any;
                        const updates: Partial<Device> = {};
                        if (body.name) updates.name = body.name;
                        if (body.group !== undefined) updates.group = body.group;
                        if (body.tags !== undefined) updates.tags = body.tags;
                        await upsertDevice({ id: deviceIdPathParam, ...updates });
                        const userInfo = await getUserFromRequest(req);
                        if (userInfo) insertAuditLog(userInfo.userId, "device_update", userInfo.ipAddress, deviceIdPathParam, JSON.stringify(updates));
                        return new Response(JSON.stringify({ ...device, ...updates }), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                    } catch (e) { return new Response(JSON.stringify({ error: "Bad request" }), { status: 400, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }}); }
                }

                if (req.method === "DELETE") {
                    try {
                        const existing = db.select().from(devices).where(eq(devices.id, deviceIdPathParam)).get();
                        if (!existing) return new Response(JSON.stringify({ error: "Not found" }), { status: 404, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                        const userInfo = await getUserFromRequest(req);
                        if (userInfo) insertAuditLog(userInfo.userId, "device_delete", userInfo.ipAddress, deviceIdPathParam, JSON.stringify({ name: existing.name }));
                        db.delete(devices).where(eq(devices.id, deviceIdPathParam)).run();
                        return new Response(JSON.stringify({ success: true }), { headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }});
                    } catch (e) { return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" }}); }
                }
            }
        }

        // 3. Static/Asset Routes
        if (url.pathname.startsWith("/images/") && req.method === "GET") {
            const file = Bun.file(`.${url.pathname}`);
            if (await file.exists()) return new Response(file);
            return new Response("Not found", { status: 404 });
        }


        // 4. CORS Options
        if (req.method === "OPTIONS") {
            return new Response(null, { headers: { "Access-Control-Allow-Origin": "*", "Access-Control-Allow-Methods": "GET, POST, DELETE, OPTIONS", "Access-Control-Allow-Headers": "Content-Type" }});
        }

        // 5. WebSocket Upgrade
        const type = url.searchParams.get("type") as "device" | "client";
        const id = url.searchParams.get("id");
        if (type && id) {
            const success = server.upgrade(req, {
                data: {
                    type, id,
                    name: url.searchParams.get("name") || id,
                    os: url.searchParams.get("os") || undefined,
                    version: url.searchParams.get("version") || undefined,
                    userId: url.searchParams.get("userId") || undefined,
                },
            });
if (success) return undefined;
        }

        return new Response("Not found", { status: 404 });
    },
    websocket: {
        async open(ws) {
            const { type, id, name, os, version, userId } = ws.data;
            console.log(`[${type}] connected: ${id} (${name})`);

            if (type === "device") {
                deviceSockets.set(id, ws);

                const updates = {
                    id,
                    userId,
                    name: name || id,
                    status: "online" as const,
                    lastSeen: new Date(),
                    os: os || null,
                    version: version || null,
                };

                await upsertDevice(updates, {
                    ipAddress: ws.remoteAddress || "unknown",
                    userId: userId,
                });

                const finalDevice = db.select().from(devices).where(eq(devices.id, id)).get();
                if (!finalDevice) {
                    ws.close(1008, "Registration failed");
                    return;
                }

                const wasOffline = !deviceRegistry.get(id) || deviceRegistry.get(id)?.status === "offline";
                deviceRegistry.set(id, { ...finalDevice, status: "online" });
                await logDeviceEvent(id, wasOffline ? "connect" : "reconnect");
                
                // Trigger Webhook on any connection (connect or reconnect)
                await triggerWebhooks(finalDevice.userId, "device.connect", { ...finalDevice, status: "online" });

                const subs = subscriptions.get(id);
                if (subs) {
                    subs.forEach((client) =>
                        client.send(JSON.stringify({ type: "status", status: "online", deviceId: id }))
                    );
                }
            } else {
                clients.set(id, ws);
            }
        },

        async message(ws, message) {
            const { type, id } = ws.data;

            // 1. Binary Media Handling (Device -> Clients)
            if (typeof message !== "string") {
                const buffer = message as Uint8Array;
                if (buffer.length > 0 && type === "device") {
                    const mediaTypeByte = buffer[0];
                    const stream = getOrCreateStream(id);
                    
                    if (mediaTypeByte === 0x01 || mediaTypeByte === 0x02) {
                        stream.videoBytesReceived += buffer.length - 1;
                    } else if (mediaTypeByte === 0x03) {
                        stream.audioBytesReceived += buffer.length - 1;
                    }

                    const subs = subscriptions.get(id);
                    if (subs) {
                        subs.forEach((client) => client.send(buffer));
                    }
                    return;
                }
            }

            // 2. JSON Message Handling
            try {
                const msg = JSON.parse(typeof message === "string" ? message : message.toString());

                if (type === "client") {
                    if (msg.type === "subscribe") {
                        const targetDeviceId = msg.deviceId;
                        if (!subscriptions.has(targetDeviceId)) {
                            subscriptions.set(targetDeviceId, new Set());
                        }
                        subscriptions.get(targetDeviceId)?.add(ws);
                        ws.data.deviceId = targetDeviceId;

                        const isOnline = deviceSockets.has(targetDeviceId);
                        ws.send(JSON.stringify({ type: "status", status: isOnline ? "online" : "offline", deviceId: targetDeviceId }));

                        if (isOnline) {
                            deviceSockets.get(targetDeviceId)?.send(JSON.stringify({ type: "action", action: "list_media_devices" }));
                        }
                    } else if (["action", "command", "input", "resize"].includes(msg.type)) {
                        const targetDeviceId = ws.data.deviceId;
                        if (targetDeviceId) {
                            const deviceWs = deviceSockets.get(targetDeviceId);
                            if (deviceWs) {
                                // console.log(`[Relay] Forwarding ${msg.type} to device ${targetDeviceId}`);
                                deviceWs.send(JSON.stringify(msg));
                            } else {
                                console.log(`[Relay] Target device ${targetDeviceId} not found or disconnected`);
                            }
                        } else {
                            console.log(`[Relay] Client ${ws.data.id} attempted to send ${msg.type} without device subscription`);
                        }
                    }

                    if (ws.data.userId) {
                        insertAuditLog(ws.data.userId, msg.type, "unknown", ws.data.deviceId, JSON.stringify({ msg: JSON.stringify(msg) }));
                    }
                } else if (type === "device") {
                    if (msg.type === "output") {
                        subscriptions.get(id)?.forEach(c => c.send(JSON.stringify({ type: "output", output: msg.output })));
                    } else if (msg.type === "ping") {
                        ws.send(JSON.stringify({ type: "pong" }));
                        const device = deviceRegistry.get(id);
                        if (device) {
                            device.lastSeen = new Date();
                            if (msg.uptime) device.uptime = msg.uptime;
                            upsertDevice({ id, lastSeen: device.lastSeen, uptime: device.uptime });
                        }
                    } else if (msg.type === "metrics" && msg.data) {
                        try {
                            db.insert(deviceMetrics).values({
                                deviceId: id,
                                cpuUsage: Math.round(msg.data.cpu),
                                ramUsage: Math.round(msg.data.ram),
                                diskUsage: Math.round(msg.data.disk),
                                networkUp: Math.round(msg.data.netUp),
                                networkDown: Math.round(msg.data.netDown),
                                timestamp: new Date(),
                            }).run();
                        } catch (e) {}

                        const stream = activeStreams.get(id);
                        const data = { ...msg.data, videoBitrate: stream?.videoBitrate || 0, audioBitrate: stream?.audioBitrate || 0 };
                        subscriptions.get(id)?.forEach(c => c.send(JSON.stringify({ type: "metrics", data, deviceId: id })));
                    } else if (msg.type === "screenshot" && msg.data) {
                        const buffer = Buffer.from(msg.data, "base64");
                        const timestamp = Date.now();
                        await Bun.write(`images/${id}/${timestamp}.png`, buffer);
                        subscriptions.get(id)?.forEach(c => c.send(JSON.stringify({ 
                            type: "screenshot_saved", 
                            url: `/images/${id}/${timestamp}.png`, 
                            filename: `${timestamp}.png` 
                        })));
                    } else if (["filesystem", "media_devices_list", "screenshot_saved"].includes(msg.type)) {
                        subscriptions.get(id)?.forEach(c => c.send(JSON.stringify(msg)));
                    }
                }
            } catch (e) {
                console.error("WS processing error:", e);
            }
        },

        async close(ws) {
            const { type, id } = ws.data;
            console.log(`[${type}] disconnected: ${id}`);

            if (type === "device") {
                deviceSockets.delete(id);
                const device = db.select().from(devices).where(eq(devices.id, id)).get();
                if (device) {
                    await triggerWebhooks(device.userId, "device.disconnect", { ...device, status: "offline" });
                }
                
                // Update registry to offline immediately
                const reg = deviceRegistry.get(id);
                if (reg) {
                    deviceRegistry.set(id, { ...reg, status: "offline", lastSeen: new Date() });
                }

                upsertDevice({ id, status: "offline", lastSeen: new Date() });
            } else if (type === "client") {
                const targetDeviceId = ws.data.deviceId;
                if (targetDeviceId) subscriptions.get(targetDeviceId)?.delete(ws);
            }
        },
    },
});

console.log(`🚀 Relay server running on ${server.hostname}:${server.port}`);
