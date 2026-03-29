---
name: lynx-dev
description: >
  Use this skill for any development task on the Lynx RMM project — a real-time remote
  management system built with a C++ Windows agent, a Bun/TypeScript WebSocket server,
  and a Nuxt.js dashboard. Trigger this skill whenever the user asks to add features,
  fix bugs, design UI components, write WebSocket message handlers, modify the database
  schema, work on the REST API, security hardening, authentication, or touch any part
  of the agent/server/web stack. Also trigger for architecture questions, code review,
  or anything referencing Lynx, the agent, the dashboard, or the RMM server.
---

# Lynx RMM — Developer Skill

---

## ⚙️ Core Coding Philosophy

These rules apply to **every line of code** written for this project, across all layers.

### 1. Security First — Always
Before writing any feature, ask:
- Who can call this? Is it authenticated and authorized?
- What happens if this input is malicious or malformed?
- Does this expose sensitive data in logs, errors, or responses?
- Could this be abused at scale (rate limiting, replay attacks)?

**Security is not a step you add at the end. It is the first design constraint.**

### 2. Write Clean, Self-Documenting Code
- **Name things well** — a good name eliminates the need for a comment.
- **No obvious comments** — never write `// increment i` or `// return the result`. If it needs explanation, the code is unclear; rewrite it.
- **Only comment *why*, never *what*** — the only acceptable comments explain non-obvious decisions, tradeoffs, or external constraints (e.g., a WinAPI quirk, a spec requirement).
- **Keep functions small and single-purpose** — if a function does two things, split it.

```typescript
// ❌ Bad — comment explains what the code obviously does
// Check if device exists before inserting
const existing = await db.select().from(devices).where(eq(devices.id, id))
if (existing.length) return

// ✅ Good — no comment needed; the code reads as prose
const exists = await db.select().from(devices).where(eq(devices.id, id))
if (exists.length) return
```

```typescript
// ✅ Acceptable comment — explains a non-obvious why
// WinAPI reports idle time in 100ns intervals; divide by 10000 for ms
const idleMs = idleInfo.dwTime / 10000
```

### 3. Optimize with Purpose
- **Profile before optimizing** — don't guess at bottlenecks.
- **Prefer clarity over micro-optimization** — readable code that is fast enough beats unreadable code that is marginally faster.
- **Optimize the right layer** — DB queries, WebSocket message frequency, and re-renders are the real bottlenecks in this stack, not JS micro-ops.
- **Avoid premature abstraction** — don't generalize until you have at least two concrete use cases.

### 4. Fail Loudly in Dev, Gracefully in Prod
- Throw specific, descriptive errors during development.
- In production, catch errors at boundaries (WS handler, API route), log the full error server-side, and return a safe generic message to the client.
- **Never swallow errors silently** (`catch (e) {}` is always wrong).

### 5. Keep the Surface Area Small
- Every new API endpoint, WS event, and DB column is a liability. Add only what is needed.
- Prefer extending existing patterns over introducing new ones.
- Delete dead code immediately — don't comment it out.

---

## Project Overview

Lynx is a **real-time Remote Management & Monitoring (RMM)** system for Windows endpoints.

```
Client[Web Dashboard — Nuxt.js] <-->|WebSocket + REST| Server[Lynx Server — Bun]
Server <-->|WebSocket| Agent[Remote Agent — C++]
Server <-->|SQLite / Drizzle| DB[(Database)]
```

| Layer | Runtime | Entry Point |
|---|---|---|
| Remote Agent | C++ / WinAPI | `App/App/Main.cpp` |
| Lynx Server | Bun (TypeScript) | `Server/index.ts` |
| Web Dashboard | Nuxt 3 | `Web/app/pages/index.vue` |

---

## Directory Map

### Agent — `App/App/`
- `Main.cpp` — Metric collection loop, WinAPI calls, WebSocket client, screenshot capture, PTY execution.

### Server — `Server/`
- `index.ts` — WebSocket relay, REST API, audit logging, static asset serving.
- `db.ts` — SQLite connection and transaction helpers (Drizzle ORM).

### Web Dashboard — `Web/`
```
Web/
├── app/
│   ├── layouts/        # Sidebar + navbar shell
│   ├── pages/
│   │   ├── index.vue                   # Auth / login
│   │   ├── dashboard/index.vue         # Main dashboard
│   │   └── dashboard/devices/[id].vue  # Per-device detail
│   └── components/
│       ├── FileManager.vue
│       └── TerminalModal.vue
├── server/api/         # Nuxt server routes (Auth, Audit Logs)
└── auth-schema.ts      # Drizzle schema definitions
```

---

## Core Data Flows

### Real-time Metrics
1. Agent collects CPU / RAM / Network / Disk via WinAPI.
2. Agent → Server: JSON payload over WebSocket.
3. Server broadcasts to Web clients and persists heartbeat to SQLite.

### Remote PTY
1. Web keystroke → WS `input` → Server → Agent PTY.
2. Agent PTY output → Server → Web (XTerm.js).

### Files & Screenshots
- Screenshots captured on-demand; served as static assets via Server.
- File operations proxied through the Server WS relay.

---

## Database Schema (`Web/auth-schema.ts`)

| Table | Purpose | Key Relations |
|---|---|---|
| `devices` | Registered remote endpoints | → metrics, logs |
| `users` | Dashboard administrators | multi-tenant |
| `audit_logs` | User action history | → user, device |
| `metrics` | Historical performance data | → device |

> Always read `auth-schema.ts` before adding columns or tables. Migrate via Drizzle Kit — never touch the SQLite file directly.

---

## WebSocket Protocol

JSON messages shared across Server↔Agent and Server↔Web. When adding a new event:
1. Define a TypeScript interface for the payload in `Server/index.ts`.
2. Add a handler in the `ws.onmessage` switch block.
3. Mirror the handler in `Main.cpp`.
4. Add the Vue-side listener in the relevant component.

Existing events (extend, never rename):
- `metrics` — telemetry payload
- `input` / `output` — PTY I/O
- `screenshot` — capture request / response
- `file_*` — file manager operations

---

## 🖥️ Backend Skill

> Files: `Server/index.ts`, `Server/db.ts`, `Web/server/api/`

### Bun Runtime
- HTTP + WebSocket via `Bun.serve()` — not Express, not the `ws` package.
- Env vars via `Bun.env.VAR_NAME`.
- Top-level `await` is fine.

### REST API
```typescript
// Nuxt server route (Web/server/api/)
export default defineEventHandler(async (event) => {
  const body = await readBody(event)
  // validate → authorize → query → return
})
```
- Validate bodies before any DB access.
- Consistent response shape: `{ data, error }`.
- Correct status codes: `201` create, `204` delete, `422` validation, `403` forbidden.

### Drizzle ORM
```typescript
// Query
const rows = await db.select().from(devices).where(eq(devices.id, id))

// Insert
await db.insert(metrics).values({ deviceId, cpu, ram, timestamp: Date.now() })

// Transaction — use for any multi-step write
await db.transaction(async (tx) => {
  await tx.insert(audit_logs).values(...)
  await tx.update(devices).set({ lastSeen: Date.now() }).where(eq(devices.id, id))
})
```
- Never use raw SQL strings.
- Index `deviceId` and `timestamp` columns — these are hot query paths.
- `bunx drizzle-kit generate` → `bunx drizzle-kit migrate` after every schema change.

### WebSocket Handlers
```typescript
case 'my_event': {
  const payload = parseAndValidate(message.data) // zod — throws on bad input
  broadcastToWebClients({ type: 'my_event_result', data: ... })
  break
}
```
- Keep agent and web client connections in separate `Map<id, WebSocket>` registries.
- Every handler must be wrapped in `try/catch` — an uncaught error closes the socket.
- Heartbeat: agents ping every N seconds; mark offline if missed.

### Audit Logging
- Log: delete device, run command, file read/write/delete, auth events.
- Fields: `userId`, `deviceId`, `action`, `metadata` (JSON), `timestamp`.
- Log in `Server/index.ts` for WS actions; in `Web/server/api/` for REST actions.

---

## 🔒 Security Skill

> Security is designed in — not bolted on. Apply this thinking before writing any handler.

### Pre-Code Security Checklist
Before implementing any feature, answer:
- [ ] What authentication is required?
- [ ] What authorization checks are needed (is this user allowed to touch this device)?
- [ ] What inputs could be malicious? How are they validated?
- [ ] What gets logged? Does the log contain anything sensitive?
- [ ] Could this endpoint be abused at volume?

### Authentication
- Sessions: **HTTP-only cookies** — never `localStorage`.
- Passwords: `Bun.password.hash()` — never store plaintext or use MD5/SHA1.
- Tokens: short-lived access (15 min) + refresh tokens. Invalidate on logout.

### Authorization Pattern
```typescript
export default defineEventHandler(async (event) => {
  const user = await requireAuth(event)              // 401 if missing/invalid
  const deviceId = getRouterParam(event, 'id')
  await requireDeviceAccess(user, deviceId)           // 403 if not owner
  // safe to proceed
})
```
Multi-tenant rule: **a user must never access another user's devices**, even with a valid session.

### WebSocket Security
- Validate the auth token on WS handshake — drop unauthenticated connections immediately.
- Agent auth: pre-shared device secret (stored hashed in DB).
- Never relay PTY output to a client not authorized for that device.
- Rate-limit `input` events — max N messages/second per connection.

### Input Validation (all layers)
```typescript
import { z } from 'zod'

const MetricsPayload = z.object({
  cpu: z.number().min(0).max(100),
  ram: z.number().min(0),
  timestamp: z.number().int().positive(),
})

const data = MetricsPayload.parse(raw) // throws ZodError on bad input
```
- Validate every WS payload and REST body with a Zod schema.
- File paths: reject any path containing `..` or absolute roots.
- PTY input: enforce max payload size (4 KB); do not restrict characters.

### C++ Agent Security
- Always verify the server's TLS cert — never disable cert verification.
- Store device secret with Windows DPAPI — not plaintext in config.
- Validate every inbound WS message schema before acting on it.

### HTTP Hardening
- Required response headers on every route:
  - `X-Content-Type-Options: nosniff`
  - `X-Frame-Options: DENY`
  - `Content-Security-Policy: default-src 'self'`
- Screenshots: serve via signed, expiring URLs — not open static paths.
- Logs: never include passwords, tokens, PTY content, or file contents.

---

## 🎨 Frontend Skill

> Files: `Web/app/pages/`, `Web/app/components/`, `Web/app/layouts/`

### Stack Rules
- **Nuxt UI first** — `UButton`, `UCard`, `UTable`, `UModal`, `UBadge`, etc. Never raw `<button>` or `<div>` where a Nuxt UI component exists.
- **Tailwind only** — no inline styles, no `<style>` blocks unless unavoidable.
- **No hardcoded colors** — always use semantic tokens.

### Semantic Color Tokens
| Token | Use |
|---|---|
| `success` | Online, confirmed, positive trend |
| `error` / `danger` | Offline, failed, destructive action |
| `warning` | Pending, degraded, caution |
| `primary` | Primary CTA, active nav, accent |
| `neutral` / `gray` | Secondary text, borders, disabled |

### Component Patterns
```vue
<!-- Status badge -->
<UBadge :color="device.online ? 'success' : 'error'">
  {{ device.online ? 'Online' : 'Offline' }}
</UBadge>

<!-- Metric card -->
<UCard>
  <template #header><span class="text-sm text-gray-500">CPU</span></template>
  <p class="text-3xl font-bold text-primary">{{ cpu }}%</p>
</UCard>

<!-- Destructive action — always confirm -->
<UButton color="error" variant="soft" @click="openConfirmModal">Delete Device</UButton>
```

### Real-time WebSocket State
```typescript
const { data, send } = useWebSocket('/ws')
const metrics = ref<Metrics | null>(null)

// Debounce — don't thrash Vue reactivity on every packet
const updateMetrics = useDebounceFn((payload: Metrics) => {
  metrics.value = payload
}, 500)

watch(data, (msg) => {
  const parsed = JSON.parse(msg)
  if (parsed.type === 'metrics') updateMetrics(parsed.data)
})
```
- Update state granularly — don't replace the whole object on every tick.
- Show a `warning` banner when the WS connection drops.

### XTerm.js Terminal
```typescript
// TerminalModal.vue
onMounted(() => {
  term = new Terminal()
  term.open(containerEl.value)
  term.onData(data => send({ type: 'input', data }))
})

onUnmounted(() => term.dispose()) // critical — prevents memory leaks
```
- Never initialize outside `onMounted` (SSR safe).
- Pipe `output` WS events directly to `term.write()`.

### File Manager
- Tree: `UTree` or nested `UAccordion` for the remote directory structure.
- Upload: hidden `<input type="file">` triggered by a `UButton`; send via WS `file_upload`.
- Download: WS `file_download` response → reconstruct `Blob` from base64 chunks → trigger anchor download.
- Always confirm destructive file operations with a `UModal`.

### Layout & Performance
- Sidebar: collapsible, state persisted to `localStorage`.
- Device detail (`[id].vue`): tab layout — Overview / Terminal / Files / Logs.
- Responsive: works at 1280 px+; sidebar auto-collapses below 1024 px.
- `<LazyComponent>` for Terminal and FileManager — mount only when the tab is active.
- Paginate all history tables — never render unbounded rows.
- `v-memo` on lists updated by high-frequency metric events.

---

## Development Workflow

### Feature Checklist (in order)
1. **Security** — answer the pre-code checklist before writing anything.
2. **Schema** — `auth-schema.ts` → `drizzle-kit generate` → `drizzle-kit migrate`.
3. **Backend** — handler + validation + audit log.
4. **Agent** — WinAPI logic + WS message in `Main.cpp`, rebuild.
5. **Frontend** — page/component, WebSocket wiring, UI states (loading, error, empty).

### Running Locally
```bash
bun run Server/index.ts   # WebSocket + REST server
cd Web && bun run dev     # Nuxt dashboard
# Agent: compile (MSVC/MinGW) and run on Windows target
```

### Gotchas
- Bun WebSocket API is `Bun.serve()` — not the `ws` npm package.
- Drizzle schema lives only in `auth-schema.ts` — never duplicate it.
- Nuxt server routes (`Web/server/api/`) are Nitro — Vue composables don't work there.
- WinAPI headers (`windows.h`, `psapi.h`) must be first in `Main.cpp`.
- XTerm: always `dispose()` in `onUnmounted`.
- `catch (e) {}` is never acceptable — always handle or rethrow.

---

## Quick Reference

| Task | Skill | File |
|---|---|---|
| New metric | Backend + Frontend | `Main.cpp` → `index.ts` → dashboard page |
| New REST endpoint | Backend | `Server/index.ts` or `Web/server/api/` |
| New DB table | Backend | `auth-schema.ts` → Drizzle migration |
| New WS event | Backend + Frontend | `index.ts` switch + `Main.cpp` + Vue listener |
| Auth / session | Security + Backend | `Web/server/api/` |
| Protect an endpoint | Security | `requireAuth` + `requireDeviceAccess` |
| Input validation | Security | Zod schema on every entry point |
| Audit log | Security + Backend | `index.ts` / `server/api/` → `audit_logs` |
| New UI component | Frontend | `Web/app/components/` |
| Terminal | Frontend | `TerminalModal.vue` |
| File manager | Frontend | `FileManager.vue` |
| Agent TLS / secret | Security | `Main.cpp` + Windows DPAPI |