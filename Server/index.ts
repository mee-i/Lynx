import { serve } from "bun";
import type { ServerWebSocket } from "bun";

const devices = new Map<string, ServerWebSocket<WebSocketData>>();
const clients = new Map<string, ServerWebSocket<WebSocketData>>();
const subscriptions = new Map<string, Set<ServerWebSocket<WebSocketData>>>();

export const deviceRegistry = new Map<
    string,
    { id: string; name: string; status: "online" | "offline"; lastSeen: Date }
>();

type WebSocketData = {
    type: "device" | "client";
    id: string;
    name?: string;
    deviceId?: string;
};

const server = serve<WebSocketData>({
    port: 9991,
    fetch(req, server) {
        const url = new URL(req.url);

        if (url.pathname === "/api/devices" && req.method === "GET") {
            const deviceList = Array.from(deviceRegistry.values());
            return new Response(JSON.stringify(deviceList), {
                headers: {
                    "Content-Type": "application/json",
                    "Access-Control-Allow-Origin": "*",
                },
            });
        }

        if (url.pathname.startsWith("/api/devices/") && req.method === "GET") {
            const id = url.pathname.split("/").pop();
            const device = deviceRegistry.get(id || "");
            if (!device) {
                return new Response(
                    JSON.stringify({ error: "Device not found" }),
                    { status: 404 },
                );
            }
            return new Response(JSON.stringify(device), {
                headers: {
                    "Content-Type": "application/json",
                    "Access-Control-Allow-Origin": "*",
                },
            });
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

        if (!type || !id) {
            return new Response("Missing type or id", { status: 400 });
        }

        const success = server.upgrade(req, {
            data: { type, id, name },
        });

        return success
            ? undefined
            : new Response("WebSocket upgrade error", { status: 400 });
    },
    websocket: {
        open(ws) {
            const { type, id, name } = ws.data;
            console.log(`[${type}] connected: ${id} (${name})`);

            if (type === "device") {
                devices.set(id, ws);

                deviceRegistry.set(id, {
                    id,
                    name: name || id,
                    status: "online",
                    lastSeen: new Date(),
                });

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
        message(ws, message) {
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
                        const device = deviceRegistry.get(targetDeviceId);
                        ws.send(
                            JSON.stringify({
                                type: "status",
                                status: device?.status || "offline",
                                deviceId: targetDeviceId,
                            }),
                        );

                        console.log("Sending test command to device");
                        const deviceWs = devices.get(targetDeviceId);
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
                    } else if (msg.type === "input" || msg.type === "resize") {
                        // Relay input and resize events to the device
                        const targetDeviceId = ws.data.deviceId;
                        if (targetDeviceId) {
                            const deviceWs = devices.get(targetDeviceId);
                            if (deviceWs) {
                                // Forward the entire message (type, data/cols/rows)
                                deviceWs.send(JSON.stringify(msg));
                            }
                            // We don't send error if device offline for input as it might be spammy,
                            // or maybe we should? For now keeping it silent for inputs to match typical behavior
                            // unless it's a critical command failure which is hard to distinguish here.
                        }
                    } else if (msg.type === "command") {
                        // kept for backward compatibility if needed
                        const targetDeviceId = ws.data.deviceId;
                        if (targetDeviceId) {
                            const deviceWs = devices.get(targetDeviceId);
                            if (deviceWs) {
                                // translate old command to input
                                deviceWs.send(
                                    JSON.stringify({
                                        type: "input",
                                        data: msg.command + "\n",
                                    }),
                                );
                            }
                        }
                    }
                } else if (type === "device") {
                    // Log ALL messages from device for debugging
                    console.log(
                        `[Device Msg] From ${id}: ${JSON.stringify(msg).substring(0, 100)}...`,
                    );

                    if (msg.type === "output") {
                        const subs = subscriptions.get(id);
                        if (subs) {
                            console.log(
                                `[Device Output] Re-broadcasting ${msg.output.length} bytes from ${id}`,
                            );
                            subs.forEach((client) =>
                                client.send(
                                    JSON.stringify({
                                        type: "output",
                                        output: msg.output,
                                    }),
                                ),
                            );
                        } else {
                            console.log(
                                `[Device Output] Received output from ${id} but no subscribers`,
                            );
                        }
                    } else if (msg.type === "ping") {
                        // Keep-alive ping from device - respond with pong
                        ws.send(JSON.stringify({ type: "pong" }));
                        
                        // Update lastSeen timestamp
                        const device = deviceRegistry.get(id);
                        if (device) {
                            device.lastSeen = new Date();
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
                devices.delete(id);

                const device = deviceRegistry.get(id);
                if (device) {
                    device.status = "offline";
                    device.lastSeen = new Date();
                }

                // Notify subscribers
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
