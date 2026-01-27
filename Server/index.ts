import { serve } from "bun";
import type { ServerWebSocket } from "bun";
import { readdir } from "node:fs/promises";
import { db, devices, type Device } from "./db";
import { eq, sql } from "drizzle-orm";

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

// Upsert device to SQLite
async function upsertDevice(device: Partial<Device> & { id: string, userId?: string | null }) {
    try {
        const existing = db.select().from(devices).where(eq(devices.id, device.id)).get();
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
                const firstUser = db.select({ id: devices.userId }).from(sql`user`).limit(1).get() as { id: string } | undefined;
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
                        os: device.os || null,
                        version: device.version || null,
                        createdAt: new Date(),
                        updatedAt: new Date(),
                    })
                    .run();
                console.log(`[device] Registered ${device.id} to user ${targetUserId}`);
            } else {
                console.error(`[device] Cannot register ${device.id}: No users found in database.`);
            }
        }
    } catch (e) {
        console.error("Failed to upsert device:", e);
    }
}

// Load on startup
await loadDevices();

type WebSocketData = {
    type: "device" | "client";
    id: string;
    name?: string;
    deviceId?: string;
    os?: string;
    version?: string;
    userId?: string;
};

const server = serve<WebSocketData>({
    port: 9991,
    async fetch(req, server) {
        const url = new URL(req.url);

        if (url.pathname === "/api/devices" && req.method === "GET") {
            try {
                const dbDevices = db.select().from(devices).all();
                const deviceList = dbDevices.map(d => ({
                    ...d,
                    status: (deviceSockets.has(d.id) ? "online" : "offline") as "online" | "offline",
                    lastSeen: d.lastSeen || new Date(d.updatedAt)
                }));
                return new Response(JSON.stringify(deviceList), {
                    headers: {
                        "Content-Type": "application/json",
                        "Access-Control-Allow-Origin": "*",
                    },
                });
            } catch (e) {
                return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
            }
        }

        if (url.pathname.startsWith("/api/devices/")) {
            const parts = url.pathname.split("/");
            
            // Screenshots endpoint: /api/devices/{id}/screenshots
            if (parts.length === 5 && parts[4] === "screenshots" && req.method === "GET") {
                const deviceId = parts[3];
                const imagesDir = `images/${deviceId}`;
                try {
                    const files = await readdir(imagesDir);
                    const screenshots = files.filter((f: string) => f.endsWith(".png")).sort().reverse(); 
                    return new Response(JSON.stringify(screenshots), {
                        headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
                    });
                } catch {
                     return new Response(JSON.stringify([]), {
                        headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
                    });
                }
            }

            const id = parts[3] || ""; // /api/devices/:id
            
            // GET /api/devices/:id
            if (req.method === "GET") {
                try {
                    const device = db.select().from(devices).where(eq(devices.id, id)).get();
                    if (!device) {
                        return new Response(JSON.stringify({ error: "Device not found" }), { status: 404, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
                    }
                    const enriched = {
                        ...device,
                        status: (deviceSockets.has(device.id) ? "online" : "offline") as "online" | "offline",
                        lastSeen: device.lastSeen || new Date(device.updatedAt)
                    };
                    return new Response(JSON.stringify(enriched), {
                        headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
                    });
                } catch (e) {
                    return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
                }
            }

            // POST /api/devices/:id - Update Device
            if (req.method === "POST") {
                try {
                    const device = db.select().from(devices).where(eq(devices.id, id)).get();
                    if (!device) {
                         return new Response(JSON.stringify({ error: "Device not found" }), { status: 404, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
                    }
                    
                    const body = await req.json() as any;
                    const updates: Partial<Device> = {};
                    if (body.name) updates.name = body.name;
                    if (body.group !== undefined) updates.group = body.group;

                    // Persist changes
                    await upsertDevice({ id, ...updates });
                    
                    return new Response(JSON.stringify({ ...device, ...updates }), {
                        headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
                    });
                } catch (e) {
                    return new Response(JSON.stringify({ error: "Invalid JSON or DB error" }), { status: 400, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
                }
            }

            // DELETE /api/devices/:id - Delete Device
            if (req.method === "DELETE") {
                try {
                    const existing = db.select().from(devices).where(eq(devices.id, id)).get();
                    if (!existing) {
                         return new Response(JSON.stringify({ error: "Device not found" }), { status: 404, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
                    }
                    db.delete(devices).where(eq(devices.id, id)).run();
                    return new Response(JSON.stringify({ success: true }), {
                        headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" },
                    });
                } catch(e) {
                     return new Response(JSON.stringify({ error: "Database error" }), { status: 500, headers: { "Content-Type": "application/json", "Access-Control-Allow-Origin": "*" } });
                }
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
                await upsertDevice(updates);

                // Get the final device state (either existing or newly created)
                const finalDevice = db.select().from(devices).where(eq(devices.id, id)).get();
                if (!finalDevice) {
                    console.warn(`[device] Failed to register/find device: ${id}`);
                    ws.close(1008, "Registration failed");
                    return;
                }

                // Sync with in-memory registry
                deviceRegistry.set(id, { ...finalDevice, status: "online" });
                
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
                                deviceWs.send(
                                    JSON.stringify({
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
                                            `echo   Device ID : ${targetDeviceId} & ` +
                                            `echo. & ` +
                                            `echo   Type 'help' to list available commands & ` +
                                            `echo ============================================ & ` +
                                            `echo on\r`,
                                    }),
                                );
                            }
                        }
                    } else if (msg.type === "input" || msg.type === "resize" || msg.type === "action") {
                        const targetDeviceId = ws.data.deviceId;
                        if (targetDeviceId) {
                            const deviceWs = deviceSockets.get(targetDeviceId);
                            if (deviceWs) {
                                deviceWs.send(JSON.stringify(msg));
                            }
                        }
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
                            upsertDevice({ id, lastSeen: now });
                        }
                    } else if (msg.type === "screenshot" && msg.data) {
                         const buffer = Buffer.from(msg.data, "base64");
                         const timestamp = Date.now();
                         const dir = `images/${id}`;
                         await Bun.write(`${dir}/${timestamp}.png`, buffer);
                         console.log(`[Screenshot] Saved ${dir}/${timestamp}.png`);

                         const subs = subscriptions.get(id);
                         if (subs) {
                            subs.forEach(client => 
                                client.send(JSON.stringify({
                                    type: "screenshot_saved",
                                    url: `/images/${id}/${timestamp}.png`,
                                    filename: `${timestamp}.png`
                                }))
                            );
                         }
                    }
                }
            } catch (err) {
                console.error("Failed to parse message", err);
            }
        },
        close(ws) {
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
