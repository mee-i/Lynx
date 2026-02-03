import { drizzle } from 'drizzle-orm/bun-sqlite';
import { Database } from 'bun:sqlite';
import { sqliteTable, text, integer } from 'drizzle-orm/sqlite-core';
import { sql, eq, relations } from 'drizzle-orm';

// Path to the Web project's database
const DB_PATH = "../Web/mydb.sqlite";

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

export type Device = typeof devices.$inferSelect;
export type NewDevice = typeof devices.$inferInsert;
export type DeviceLog = typeof deviceLogs.$inferSelect;
export type NewDeviceLog = typeof deviceLogs.$inferInsert;
