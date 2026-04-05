import { drizzle } from 'drizzle-orm/bun-sqlite';
import { Database } from 'bun:sqlite';
import { sqliteTable, text, integer } from 'drizzle-orm/sqlite-core';
import { sql, eq, relations } from 'drizzle-orm';

// Path to the Web project's database
const DB_PATH = "../Web/mysql.db";

const sqlite = new Database(DB_PATH);
export const db = drizzle(sqlite);

// Define the devices table schema (must match Web/db/schema.ts)
export const devices = sqliteTable("device", {
  id: text("id").primaryKey(),
  userId: text("user_id").notNull(),
  name: text("name").notNull(),
  status: text("status", { enum: ["online", "offline"] }).default("offline").notNull(),
  lastSeen: integer("last_seen", { mode: "timestamp_ms" }),
  group: text("group"),
  tags: text("tags", { mode: "json" }).$type<string[]>(),
  os: text("os"),
  version: text("version"),
  createdAt: integer("created_at", { mode: "timestamp_ms" }).notNull(),
  updatedAt: integer("updated_at", { mode: "timestamp_ms" }).notNull(),
  uptime: integer("uptime"),
});

export const deviceLogs = sqliteTable("device_log", {
  id: integer("id").primaryKey({ autoIncrement: true }),
  deviceId: text("device_id").notNull(),
  type: text("type", { enum: ["connect", "disconnect", "reconnect"] }).notNull(),
  timestamp: integer("timestamp", { mode: "timestamp_ms" }).notNull(),
  message: text("message"),
});

export const deviceMetrics = sqliteTable("device_metrics", {
  id: integer("id").primaryKey({ autoIncrement: true }),
  deviceId: text("device_id").notNull(),
  cpuUsage: integer("cpu_usage"),
  ramUsage: integer("ram_usage"),
  diskUsage: integer("disk_usage"),
  networkUp: integer("network_up"), // KB/s
  networkDown: integer("network_down"), // KB/s
  timestamp: integer("timestamp", { mode: "timestamp_ms" }).notNull(),
});

export const sessions = sqliteTable("session", {
  id: text("id").primaryKey(),
  expiresAt: integer("expires_at", { mode: "timestamp_ms" }).notNull(),
  token: text("token").notNull(),
  userId: text("user_id").notNull(),
  ipAddress: text("ip_address"),
});

export const auditLog = sqliteTable("audit_log", {
  id: integer("id").primaryKey({ autoIncrement: true }),
  userId: text("user_id").notNull(),
  deviceId: text("device_id"),
  action: text("action").notNull(),
  payload: text("payload"),
  ipAddress: text("ip_address"),
  timestamp: integer("timestamp", { mode: "timestamp_ms" }).notNull(),
});

export const webhooks = sqliteTable("webhook", {
  id: integer("id").primaryKey({ autoIncrement: true }),
  userId: text("user_id").notNull(),
  url: text("url").notNull(),
  events: text("events", { mode: "json" }).$type<string[]>().notNull(),
  isEnabled: integer("is_enabled", { mode: "boolean" }).default(true).notNull(),
  createdAt: integer("created_at", { mode: "timestamp_ms" }).notNull(),
  updatedAt: integer("updated_at", { mode: "timestamp_ms" }).notNull(),
});

export type Device = typeof devices.$inferSelect;
export type NewDevice = typeof devices.$inferInsert;
export type DeviceLog = typeof deviceLogs.$inferSelect;
export type NewDeviceLog = typeof deviceLogs.$inferInsert;
export type AuditLog = typeof auditLog.$inferSelect;
export type NewAuditLog = typeof auditLog.$inferInsert;
export type Webhook = typeof webhooks.$inferSelect;
export type NewWebhook = typeof webhooks.$inferInsert;

