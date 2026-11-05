# สถาปัตยกรรมเป้าหมาย: Central Config + Native Android UI

- **วันที่:** 29 ตุลาคม 2026
- **สถานะ:** ข้อเสนอ + ข้อตัดสินใจยืนยันแล้ว (ยังไม่ลงมือ implement)
- **เอกสารอ้างอิง:** [`PROJECT_ANALYSIS.md`](./PROJECT_ANALYSIS.md)

---

## 1. เป้าหมาย

1. **ทุกเครื่องเห็น setting เดียวกัน** — mode (auto/manual), ตารางเวลา, สถานะวาล์วล่าสุด
2. **ไม่ใช้ WebView** — Android เป็น native UI (แนะนำ Jetpack Compose)
3. **Cloudflare Worker/Pages เป็น backend กลาง** — เป็นแหล่งความจริง (source of truth)
4. **รองรับการใช้งานจริง** — auth, conflict handling, command ที่มี TTL/ack, safety ของวาล์ว
5. **ไม่ทำลายค่าที่มีอยู่** — migrate config จาก KV เดิมโดยไม่หาย

### 1.1 ข้อตัดสินใจที่ยืนยันแล้ว (29 ต.ค. 2026)

| หัวข้อ | ข้อตัดสินใจ | ผลต่อสถาปัตยกรรม |
|---|---|---|
| จำนวนวาล์ว | **1 วาล์ว** | schema เผื่อ multi-device แต่ UI v1 แสดงอุปกรณ์เดียว |
| การเชื่อมต่อ ESP32 | **ซิม 4G/5G 399 MB/เดือน ไม่ใช้ WiFi** | ต้องประหยัด data มากที่สุด; MQTT persistent เป็นช่องทาง real-time หลัก; HTTPS config poll ห่าง ๆ |
| ลำดับงาน | **ทำ backend + native UI คู่ขนาน** | แยก track backend/Android; ใช้ API contract ร่วมกัน |
| Auth | **Invite code + household password** | v1 ใช้รหัสบ้านร่วมกัน + session token; ยังไม่มีบัญชีรายคนเต็มรูปแบบ |

> ข้อตัดสินใจด้าน data ทำให้สมมติฐานเดิม ("ESP32 poll ทุก 10–15 วิ") ใช้ไม่ได้ ต้องใช้ MQTT สำหรับ command/status และลด HTTPS poll ลงมาก (ดูข้อ 4.4)

---

## 2. คำตอบเรื่องความเป็นไปได้

**ทำได้** ในทางเทคนิค และโปรเจกต์มีพื้นฐานที่ใช้ต่อได้มาก:

| มีแล้ว | ต้องเพิ่ม |
|---|---|
| Worker + KV + cron | D1 schema + revision + auth |
| ESP32 ดึง config จาก Worker | desired/ack protocol + device token |
| Schedule engine ใน firmware | แก้ overnight + ArduinoJson + safety |
| Web UI ที่มี UX ครบ | native Compose screens |
| MQTT | คงไว้เป็นช่องทาง real-time หลักของ ESP32 (ประหยัด data บนซิม) |

**สิ่งที่ยังไม่มีและเป็นหัวใจของโจทย์:** แอปไม่ได้อ่านค่าจาก Worker เลย ดังนั้น "เครื่องอื่นเห็นค่า" ยังไม่เกิด — ต้องเพิ่ม read path + polling/SSE + revision

**ข้อควรระวัง:** อย่าเพิ่งแทน WebView ด้วย native UI ก่อน backend มี read path เพราะจะได้ UI ใหม่ที่ยัง sync ไม่ได้ ต้องทำ backend v2 และ native UI ไปพร้อมกัน (หรือ backend ก่อน)

---

## 3. ภาพรวมสถาปัตยกรรมเป้าหมาย

```
┌──────────────────────────────────────────────┐
│ Android app (Kotlin + Compose)               │
│  - Login / Household                          │
│  - Dashboard (valve, mode, next schedule)     │
│  - Schedules editor                           │
│  - Settings                                   │
│  - Repository + Room cache + DataStore        │
└───────────────┬──────────────────────────────┘
                │ HTTPS JSON + Bearer token
                │ GET /v1/state (poll 15s หรือ SSE)
                │ PUT /v1/config (If-Match revision)
                │ POST /v1/commands
                ▼
┌──────────────────────────────────────────────┐
│ Cloudflare Worker API (v2)                    │
│  - Auth (login/session)                       │
│  - Config service (revision, validation)      │
│  - Command service (TTL, idempotency, ack)    │
│  - Device service (token, status, desired)    │
│  - SSE/WebSocket (optional, Durable Object)   │
└───────┬───────────────────────┬──────────────┘
        │                       │
        ▼                       ▼
┌───────────────┐       ┌──────────────────────┐
│ D1 (SQL)      │       │ KV (cache/rate limit)│
│ households    │       │ rate limit counters  │
│ sessions      │       │ short-lived cache    │
│ devices       │       └──────────────────────┘
│ configs       │
│ schedules     │       ┌──────────────────────┐
│ commands      │       │ Cron Triggers        │
│ audit_log     │       │ - expire commands    │
└───────────────┘       │ - health checks      │
                        └──────────────────────┘
        ▲
        │ HTTPS (device token) — config poll ทุก 15 นาที
        │ GET /v1/devices/desired (ETag)
        │ POST /v1/devices/status (เป็นระยะ/เมื่อเปลี่ยน)
        │ POST /v1/devices/commands/:id/ack
        │
┌───────┴──────────────────────────────────────┐
│ ESP32 firmware v2                             │
│  - 4G/5G SIM (399 MB/เดือน) + NTP             │
│  - MQTT persistent (TLS 8883)                 │
│    command/status real-time                   │
│  - HTTPS config poll ห่าง ๆ (ETag)            │
│  - ArduinoJson                                │
│  - Schedule engine (overnight fix)            │
│  - Command TTL + ack                          │
│  - Max runtime / watchdog / relay level       │
└──────────────────────────────────────────────┘
```

> **ช่องทาง command/status ใน v1:** Android -> MQTT (scoped credential) -> ESP32 สำหรับ manual control/status แบบทันที; Worker เก็บ config และประวัติคำสั่ง. หากต้องการตัด MQTT credential ออกจากแอปทั้งหมด ต้องทำ Worker->MQTT relay หรือ WSS/DO เป็น phase ถัดไป

### 3.1 หลักการออกแบบ

1. **Single source of truth:** config อยู่ใน D1 ที่เดียว; ทุก client อ่านจากที่นี่
2. **Revision-based optimistic concurrency:** ทุกการแก้ config เพิ่ม `revision`; client ส่ง revision เดิมมา; ถ้าไม่ตรง → 409
3. **Last-write-wins แบบมีสติ:** ผู้ใช้เห็นข้อความ "มีคนแก้ไขล่าสุด" แล้วเลือกโหลดใหม่/เขียนทับ
4. **Command เป็น event ที่มีอายุ:** ไม่ใช่ state ที่ค้างใน KV ตลอดไป
5. **Device เป็น first-class:** มี `device_id`, token, status, last_seen
6. **Offline-first ที่ปลอดภัย:** แอป cache ค่าได้; ESP32 รันตารางในเครื่องได้; แต่คำสั่งต้องมี TTL
7. **Safety ก่อน convenience:** max runtime, command expiry, fail-safe relay

---

## 4. Stack ที่แนะนำ

### 4.1 Android

| ส่วน | เลือก | เหตุผล |
|---|---|---|
| UI | **Jetpack Compose + Material 3** | native จริง, ลด XML, ทำ state-driven UI ง่าย |
| สถาปัตยกรรม | MVVM + Repository + StateFlow | มาตรฐาน, test ง่าย |
| HTTP | **Retrofit + OkHttp** | familiar, interceptor สำหรับ token |
| JSON | **kotlinx.serialization** (หรือ Moshi) | type-safe, ทำงานกับ Kotlin ดี |
| Local cache | **Room** (config/status) + **DataStore** (session/settings) | offline + token storage |
| Navigation | Navigation Compose | หลายหน้า |
| Background | WorkManager (optional) | refresh เป็นระยะ; อย่าหวัง real-time ตอน background |
| Push | FCM (optional, phase 5) | แจ้งเตือน |
| DI | Hilt (optional) | ถ้าโปรเจกต์โต; เริ่มด้วย manual DI ได้ |

**ทางเลือกสำรอง:** XML + ViewBinding + Material Components — เสี่ยงน้อยกว่าเรื่อง version ของ Compose แต่ UI จะพัฒนา slower และ state handling manual กว่า

**ข้อจำกัด version:** โปรเจกต์ใช้ Kotlin 1.9.24 / AGP 8.7.3 → ใช้ Compose Compiler 1.5.14 และ Compose BOM ที่เข้ากัน (ดู `MIGRATION_PLAN.md`)

### 4.2 Backend

| ส่วน | เลือก | เหตุผล |
|---|---|---|
| Runtime | Cloudflare Workers | มีอยู่แล้ว |
| Database | **D1** | relational, transaction, revision update แบบ atomic |
| Cache/rate limit | KV | เหมาะกับ counter/short-lived cache |
| Real-time | Polling ก่อน; SSE/WebSocket ผ่าน Durable Object ภายหลัง | ลด complexity |
| Cron | Cloudflare Cron Triggers | expire commands, health |
| Framework | plain `fetch` หรือ Hono | Hono ช่วย routing/validation; เริ่ม plain ได้ |
| Validation | Zod (ถ้าใช้ Hono) หรือ manual | กัน payload เพี้ยน |

**ทำไมไม่ใช้ KV เป็น source of truth:** KV ไม่มี atomic compare-and-set/transaction ที่เหมาะกับ revision; การอ่าน-แก้-เขียนอาจ race กันได้ D1 เหมาะกว่า

### 4.3 Auth (v1: invite code + household password)

**รูปแบบที่เลือก:** household มี invite code + password; ผู้ใช้กรอกครั้งแรกแล้วได้ session token

- `POST /v1/auth/join` — `{inviteCode, password, displayName}` → สร้าง member + session
- `POST /v1/auth/login` — `{inviteCode, password, memberId?}` → session ใหม่ (ถ้า `memberId` ตรงกับ household ใช้ต่อ)
- Password hash: **PBKDF2-SHA256** ผ่าน WebCrypto (Workers ไม่มี Argon2 native) — salt ต่อ household, iterations 100,000 (ตรงกับ `worker/v2/index.js`)
- Session: random 32 bytes, เก็บ SHA-256 hash ใน D1 (`sessions` table), อายุ 30 วัน, revoke ได้
- ทุก request แนบ `Authorization: Bearer <session_token>`
- Device token แยก: random 32 bytes, แสดงครั้งเดียวตอน provision, เก็บ SHA-256 hash
- v1 ยังไม่มี role รายคน; audit ใช้ `member_id` + displayName
- อนาคต: เพิ่ม users/roles/OAuth ได้โดยไม่เปลี่ยน API หลัก

**ข้อดี:** ง่ายสำหรับครอบครัว, ไม่ต้องทำระบบ email/สมัครสมาชิก
**ข้อจำกัด:** ใครรู้ invite code + password ก็เข้าบ้านได้; มี endpoint เปลี่ยนรหัสบ้านและ rate limit 5 failed attempts/min/IP แล้ว แต่ยังไม่มี role รายคนหรือ 2FA

### 4.4 Real-time และ data budget (ESP32 บนซิม 399 MB/เดือน)

**ข้อจำกัดที่ยืนยันแล้ว:** ESP32 ใช้ซิม 4G/5G งบ 399 MB/เดือน ไม่ใช้ WiFi → **ห้าม poll HTTPS ถี่ ๆ** เพราะ TLS handshake กิน data มากกว่าตัว JSON หลายเท่า

**หลักการ:**

1. **MQTT persistent (TLS 8883)** เป็นช่องทาง real-time หลักของ ESP32 — command/status/ack
2. **HTTPS config poll ห่าง ๆ** (ทุก 15 นาที) พร้อม ETag/304 — หรือ fetch ทันทีเมื่อได้รับ MQTT notify ว่า config เปลี่ยน
3. **App** คุย Worker สำหรับ config; คุย MQTT (scoped credential) สำหรับ manual command/status ใน v1
4. **Status** ส่งทุก 5 นาทีเมื่อ idle + ส่งทันทีเมื่อ state เปลี่ยน (ไม่ต้องทุก 60 วิ)

**ประมาณการ data (ต่อเดือน):**

| รายการ | ความถี่ | ประมาณ |
|---|---|---|
| MQTT keepalive | 60 วิ | 3–8 MB |
| Status | 5 นาที + เมื่อเปลี่ยน | 2–4 MB |
| Device sync (HTTPS status+config, ETag) | 15 นาที | 5–20 MB |
| Command/ack | ตามการใช้งาน | < 1 MB |
| **รวม** | | **~10–30 MB/เดือน** |

> 399 MB/เดือน = ~13.3 MB/วัน; 10–30 MB/เดือนคือต่ำกว่า 8% ของงบ จึงเหลือ headroom สำหรับ OTA/debug

**ทำไมไม่ polling 10–15 วิ:**

- 720–2,880 requests/วัน + TLS handshake ต่อครั้ง → ราว 100–300 MB/เดือน หรือมากกว่า
- ถ้าใช้ `http.setReuse(true)` และ ETag/304 จะลดได้ แต่ยังแพงกว่า MQTT มาก
- MQTT connection เดียวที่เปิดค้างไว้ประหยัดกว่าและได้ latency ต่ำกว่าสำหรับ command

**ทางเลือกในอนาคต (ถ้าต้องการตัด MQTT credential ออกจากแอป):**

- Worker -> MQTT relay ผ่าน Cloudflare TCP 8883 (ต้องทดสอบว่า Worker ต่อพอร์ตนี้ได้)
- ESP32 <-> Worker WebSocket ผ่าน Durable Object (ตัด MQTT ออกทั้งหมด)
- ทั้งสองทางต้อง spike/test ก่อนตัดสินใจ; v1 ใช้ MQTT แบบ scoped credential ไปก่อน

**App polling:** foreground `GET /state` ทุก 15 วิ (เฉพาะตอนเปิดแอป), background หยุด; ใช้ ETag/304 — data ฝั่งมือถือไม่เกี่ยวกับซิม ESP32

---

## 5. Data model (D1)

### 5.1 Schema

```sql
-- บ้าน/กลุ่มผู้ใช้ (v1: invite code + household password)
CREATE TABLE households (
  id            TEXT PRIMARY KEY,
  name          TEXT NOT NULL,
  timezone      TEXT NOT NULL DEFAULT 'Asia/Bangkok',
  invite_code   TEXT UNIQUE NOT NULL,
  password_hash TEXT NOT NULL,                 -- pbkdf2$iterations$salt$hash
  created_at    TEXT NOT NULL DEFAULT (datetime('now'))
);

-- สมาชิกที่ join ผ่าน invite code (1 แถวต่อการ join/อุปกรณ์)
CREATE TABLE members (
  id           TEXT PRIMARY KEY,
  household_id TEXT NOT NULL REFERENCES households(id) ON DELETE CASCADE,
  display_name TEXT NOT NULL,
  created_at   TEXT NOT NULL DEFAULT (datetime('now'))
);

-- session token (opaque token; เก็บเฉพาะ hash)
CREATE TABLE sessions (
  id         TEXT PRIMARY KEY,
  member_id  TEXT NOT NULL REFERENCES members(id) ON DELETE CASCADE,
  token_hash TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL,
  expires_at TEXT NOT NULL,
  revoked_at TEXT
);

-- อุปกรณ์ (ESP32)
CREATE TABLE devices (
  id               TEXT PRIMARY KEY,
  household_id     TEXT NOT NULL REFERENCES households(id) ON DELETE CASCADE,
  name             TEXT NOT NULL,
  token_hash       TEXT NOT NULL,              -- SHA-256 ของ device token
  firmware_version TEXT,
  last_seen_at     TEXT,
  last_status_json TEXT,                       -- {valve, mode, rssi, uptime, ...}
  created_at       TEXT NOT NULL DEFAULT (datetime('now'))
);

-- config ต่อ household (v1: 1 เครื่องต่อ household)
CREATE TABLE configs (
  household_id   TEXT PRIMARY KEY REFERENCES households(id) ON DELETE CASCADE,
  mode           TEXT NOT NULL CHECK (mode IN ('auto','manual')),
  schedules_json TEXT NOT NULL,                -- JSON array; validate ก่อนเขียน
  revision       INTEGER NOT NULL DEFAULT 1,
  updated_at     TEXT NOT NULL,
  updated_by     TEXT REFERENCES members(id)
);

-- คำสั่งเปิด/ปิด (event ที่มีอายุ)
CREATE TABLE commands (
  id              TEXT PRIMARY KEY,
  household_id    TEXT NOT NULL REFERENCES households(id) ON DELETE CASCADE,
  device_id       TEXT NOT NULL REFERENCES devices(id) ON DELETE CASCADE,
  action          TEXT NOT NULL CHECK (action IN ('open','close')),
  status          TEXT NOT NULL DEFAULT 'pending'
                  CHECK (status IN ('pending','acked','expired','failed')),
  idempotency_key TEXT,
  created_by      TEXT REFERENCES members(id),
  created_at      TEXT NOT NULL,
  expires_at      TEXT NOT NULL,
  acked_at        TEXT,
  error           TEXT
);

-- audit
CREATE TABLE audit_log (
  id           TEXT PRIMARY KEY,
  household_id TEXT NOT NULL,
  actor_type   TEXT NOT NULL CHECK (actor_type IN ('user','device','system')),
  actor_id     TEXT,
  action       TEXT NOT NULL,
  detail_json  TEXT,
  created_at   TEXT NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX idx_devices_household ON devices(household_id);
CREATE INDEX idx_commands_pending ON commands(device_id, status, expires_at);
CREATE INDEX idx_audit_household_time ON audit_log(household_id, created_at DESC);
CREATE INDEX idx_sessions_member ON sessions(member_id, expires_at);
CREATE INDEX idx_members_household ON members(household_id);
CREATE UNIQUE INDEX idx_commands_idempotency
  ON commands(household_id, idempotency_key)
  WHERE idempotency_key IS NOT NULL;
```

### 5.2 ทำไมเก็บ schedules เป็น JSON ใน `configs`

- โครงสร้างเดิมเป็น JSON array อยู่แล้ว → migrate ง่าย
- การแก้ทั้งชุดเป็น atomic อยู่แล้วด้วย revision
- ลด join/transaction หลายตาราง
- ถ้าต้องการ query ราย schedule/ทำ per-schedule permission ค่อย normalize เป็นตาราง `schedules` ภายหลัง

**ข้อควรระวัง:** ต้อง validate JSON ฝั่ง server ทุกครั้ง (จำนวนรายการ, เวลา `HH:MM`, `days` length 7, `enabled` boolean, id ไม่ซ้ำ)

### 5.3 Revision

- เริ่มที่ `1`
- ทุกครั้งที่ `PUT /config` สำเร็จ → `revision = revision + 1`
- Client เก็บ `revision` ล่าสุดที่เห็น
- Client ส่ง `If-Match: <revision>` หรือ body `{baseRevision}` มา
- Server update แบบ atomic:

```sql
UPDATE configs
SET mode = ?, schedules_json = ?, revision = revision + 1,
    updated_at = ?, updated_by = ?
WHERE household_id = ? AND revision = ?;
```

ถ้า `meta.changes == 0` → `409 CONFLICT` พร้อมส่ง state ปัจจุบันกลับไป

---

## 6. API spec (v2)

Base: `<legacy-worker-url>/v1`

### 6.1 รูปแบบ response

สำเร็จ:

```json
{ "ok": true, "data": { ... } }
```

ผิดพลาด:

```json
{
  "ok": false,
  "error": {
    "code": "CONFLICT",
    "message": "Config was modified by another user",
    "currentRevision": 43
  }
}
```

HTTP status: 400, 401, 403, 404, 409, 410, 422, 429, 500

### 6.2 Auth (invite code + household password)

#### `POST /v1/auth/join`

```json
{ "inviteCode": "ABCD-1234", "password": "...", "displayName": "มือถือป๊า" }
```

```json
{
  "ok": true,
  "data": {
    "token": "sess_...",
    "expiresAt": "2026-11-28T00:00:00Z",
    "member": { "id": "mem_...", "displayName": "มือถือป๊า" },
    "household": { "id": "hh_...", "name": "บ้าน", "timezone": "Asia/Bangkok" }
  }
}
```

- ใช้ครั้งแรกของการติดตั้ง/เครื่องใหม่
- สร้าง `members` 1 แถว + `sessions` 1 แถว

#### `POST /v1/auth/login`

```json
{ "inviteCode": "ABCD-1234", "password": "...", "memberId": "mem_..." }
```

- `memberId` ไม่บังคับ; ถ้าระบุและตรงกับ household → ใช้ member เดิม
- ถ้าไม่ระบุ → สร้าง member ใหม่ (เช่น login จากเครื่องใหม่)
- Rate limit: 5 ครั้ง/นาที/IP (นับเฉพาะครั้งที่ล้มเหลว; join/login ใช้ bucket ร่วมกัน; สำเร็จแล้วล้างตัวนับ)

#### `POST /v1/auth/logout`

Revoke session

#### `GET /v1/me`

```json
{
  "ok": true,
  "data": {
    "member": { "id": "mem_...", "displayName": "มือถือป๊า" },
    "household": { "id": "hh_...", "name": "บ้าน", "timezone": "Asia/Bangkok" }
  }
}
```

#### `POST /v1/households/:hid/password`

เปลี่ยนรหัสบ้าน ต้องมี session ของ household นั้น

```json
{ "currentPassword": "...", "newPassword": "..." }
```

- `newPassword` อย่างน้อย 8 ตัวอักษร และต้องต่างจากรหัสเดิม
- รหัสปัจจุบันผิด → `401 INVALID_PASSWORD` (ไม่ใช่ `UNAUTHORIZED` เพื่อไม่ให้แอปเข้าใจว่า session หมดอายุ)
- สำเร็จ → revoke session อื่นทั้งหมดของ household (session ที่เรียกยังใช้ได้)
- audit: `household.password.change`

#### `POST /v1/households/:hid/devices/:did/rotate-token`

ออก device token ใหม่ (คืน plaintext ครั้งเดียว; D1 เก็บเฉพาะ hash)

- ต้องเป็นสมาชิกของ household และ device ต้องอยู่ใน household นั้น
- token เก่าใช้ไม่ได้ทันที → ต้อง provision firmware ใหม่ผ่าน serial
- audit: `device.token.rotate`

### 6.3 State (อ่านค่าที่ทุกเครื่องต้องเห็นเหมือนกัน)

#### `GET /v1/households/:hid/state`

Header: `Authorization: Bearer <token>`

```json
{
  "ok": true,
  "data": {
    "revision": 42,
    "mode": "auto",
    "schedules": [
      {
        "id": 1,
        "name": "รอบเช้า",
        "start": "06:00",
        "end": "06:30",
        "days": [true, true, true, true, true, false, false],
        "enabled": true
      }
    ],
    "updatedAt": "2026-10-29T10:00:00Z",
    "updatedBy": { "id": "mem_...", "displayName": "..." },
    "devices": [
      {
        "id": "dev_...",
        "name": "วาล์วหน้าบ้าน",
        "online": true,
        "lastSeenAt": "2026-10-29T09:59:50Z",
        "valve": false,
        "mode": "auto",
        "rssi": -62,
        "firmwareVersion": "2.0.0"
      }
    ]
  }
}
```

**ETag:** opaque เช่น `"rev-42-dev-<device>:<lastSeenAt>-t<minute>"` → client ส่ง `If-None-Match`; ถ้าไม่เปลี่ยน → `304`
- ETag ครอบทั้ง config revision, `lastSeenAt` ของอุปกรณ์ และ time bucket 1 นาที (เพื่อให้ `online` เปลี่ยนได้โดยไม่มี sync ใหม่)
- `If-Match` สำหรับเขียน config ยังเป็น `"rev-N"` แยกจาก ETag นี้

**Online window:** `online = true` เมื่อ `lastSeenAt` อยู่ภายใน 20 นาที (sync ทุก 15 นาที + margin) — ห้ามใช้ 5 นาที เพราะอุปกรณ์ปกติจะดูเหมือนออฟไลน์ 10 จาก 15 นาที

### 6.4 Config (เขียน)

#### `PUT /v1/households/:hid/config`

Header: `Authorization`, `If-Match: "rev-42"` (หรือ body `baseRevision`)

```json
{
  "mode": "auto",
  "schedules": [ ... ]
}
```

สำเร็จ:

```json
{ "ok": true, "data": { "revision": 43, "updatedAt": "..." } }
```

Conflict:

```json
{
  "ok": false,
  "error": {
    "code": "CONFLICT",
    "message": "Config was modified by another user",
    "current": { "revision": 44, "mode": "manual", "schedules": [ ... ] }
  }
}
```

**Validation:**
- `mode` ต้องเป็น `auto`/`manual`
- `schedules` array ≤ **8** รายการ (ตรงกับ `MAX_SCHEDULES` ของ firmware; เกินกว่าจะถูกปฏิเสธ)
- `id` เป็นจำนวนเต็มบวกและไม่ซ้ำ, `name` ≤ 100 ตัวอักษร
- `start`/`end` ตรง `^([01]\d|2[0-3]):[0-5]\d$`; ถ้า `end <= start` ถือเป็นรอบข้ามคืน
- `days` length 7, boolean (จันทร์=0)
- `enabled` boolean

### 6.5 Commands

#### `POST /v1/households/:hid/commands`

```json
{
  "deviceId": "dev_...",
  "action": "open",
  "idempotencyKey": "uuid-v4"
}
```

- Server ตั้ง `expires_at = now + 20 นาที` (ต้องมากกว่า sync interval 15 นาที)
- ถ้ามี pending command ของ device เดียวกัน → mark `expired` + `error='superseded'` ก่อน (latest wins; กันคำสั่ง open เก่ามาทำงานหลัง close)
- ถ้า `idempotencyKey` ซ้ำ → คืน command เดิม (กัน double-tap/retry)

```json
{
  "ok": true,
  "data": {
    "commandId": "cmd_...",
    "action": "open",
    "expiresAt": "2026-10-29T10:20:00Z",
    "status": "pending"
  }
}
```

#### `GET /v1/households/:hid/commands?limit=20`

ประวัติล่าสุด (optional)

- คำสั่ง `pending` ที่เลย `expires_at` จะถูก mark เป็น `expired` + `error='ttl'` ก่อนคืนผล (ยังไม่มี cron trigger)

### 6.6 Device endpoints (ใช้ device token)

#### `POST /v1/devices/sync` (แนะนำสำหรับ ESP32)

รวม status + desired ไว้ในคำขอเดียว เพื่อลด TLS/request overhead

Request:

```json
{
  "valve": false,
  "mode": "auto",
  "rssi": -62,
  "uptimeSeconds": 3600,
  "firmwareVersion": "2.0.0",
  "lastCommandId": "cmd_...",
  "fault": null
}
```

Response:

```json
{
  "ok": true,
  "data": {
    "deviceId": "dev_...",
    "revision": 43,
    "mode": "auto",
    "schedules": [ ... ],
    "command": null,
    "serverTime": "2026-10-29T10:00:05Z"
  }
}
```

- ใช้ทุก 15 นาที (หรือเมื่อได้รับ MQTT notify ว่า config เปลี่ยน)
- Server อัปเดต `devices.last_seen_at`, `last_status_json`
- ถ้าไม่มีอะไรเปลี่ยนและไม่มี command → สามารถคืน `304` ได้ (แต่ต้องอ่าน status ที่ส่งมาด้วย)

#### `GET /v1/devices/desired`

> ใช้สำหรับ debug/แยก endpoint; ปกติใช้ `POST /devices/sync`

Header: `Authorization: Bearer <device_token>`, `If-None-Match: "rev-42"`

```json
{
  "ok": true,
  "data": {
    "deviceId": "dev_...",
    "revision": 43,
    "mode": "auto",
    "schedules": [ ... ],
    "command": {
      "id": "cmd_...",
      "action": "open",
      "expiresAt": "2026-10-29T10:01:00Z"
    },
    "serverTime": "2026-10-29T10:00:05Z"
  }
}
```

- ถ้า revision ไม่เปลี่ยนและไม่มี command ใหม่ → `304`
- `command` เป็น `null` ถ้าไม่มี
- Server ไม่ส่ง command ที่หมดอายุแล้ว

#### `POST /v1/devices/status`

```json
{
  "valve": false,
  "mode": "auto",
  "rssi": -62,
  "uptimeSeconds": 3600,
  "firmwareVersion": "2.0.0",
  "lastCommandId": "cmd_...",
  "fault": null
}
```

#### `POST /v1/devices/commands/:id/ack`

```json
{ "status": "acked", "error": null }
```

- `acked` / `failed` / `expired`
- Server อัปเดต `commands.status`, `acked_at`

### 6.7 SSE (optional phase 5)

#### `GET /v1/households/:hid/events`

```text
event: config
data: {"revision":43,"mode":"auto",...}

event: command
data: {"commandId":"cmd_...","action":"open","status":"acked"}
```

- ใช้ Durable Object ต่อ household เพื่อ fan-out
- Android ต่อ SSE ขณะ foreground; background ใช้ polling/WorkManager

### 6.8 Health

#### `GET /health`

```json
{ "ok": true, "version": "2.0.0", "time": "..." }
```

---

## 7. Sync protocol

### 7.1 แอปเปิด

```
1. โหลด session จาก DataStore
2. GET /v1/households/:hid/state (พร้อม ETag/revision ล่าสุด)
3. ถ้า 200 → บันทึกลง Room → render
4. ถ้า 304 → ใช้ cache
5. เริ่ม poll ทุก 15 วิ (foreground)
```

### 7.2 ผู้ใช้แก้ mode/schedule

```
1. ผู้ใช้แก้ใน UI (optimistic update + แสดง "กำลังบันทึก")
2. PUT /config พร้อม baseRevision
3. 200 → อัปเดต revision ใหม่, แสดง "บันทึกแล้ว"
4. 409 → แสดง dialog "มีคนแก้ไขล่าสุด" + โหลด state ใหม่
   - ให้ผู้ใช้เลือก "ใช้ค่าล่าสุด" หรือ "เขียนทับ" (ส่ง revision ใหม่)
5. network error → เก็บ pending change ใน Room/WorkManager (optional)
   - อย่า auto-retry แบบไม่จำกัด; แสดงสถานะให้ผู้ใช้
```

### 7.3 ผู้ใช้กดเปิด/ปิดวาล์ว (v1: MQTT เร็ว + Worker audit)

```
1. App สร้าง commandId + expiresAt (60s สำหรับ MQTT)
2. App publish MQTT {cmd:"open|close", commandId, expiresAt}
   ไป myhouse/valve/control (scoped credential)
3. App POST /commands (D1 queue, TTL 20 นาที) เป็น fallback/audit
4. ESP32 ตรวจ expiresAt + commandId ใหม่ → execute → publish status/ack
5. App เห็น status ทาง MQTT ทันที
6. ESP32 POST /devices/commands/:id/ack (หรือรวมใน sync รอบถัดไป)
```

**Fallback:** ถ้า MQTT ส่งไม่ได้ → command ที่ POST ไว้ใน Worker จะถูกส่งให้ ESP32 ใน `desired`/`sync` รอบถัดไป (ช้ากว่า แต่ไม่หาย; TTL 20 นาทีครอบ sync 15 นาที)

**ห้าม** ให้แอปสมมติว่าคำสั่งสำเร็จทันที — ต้องรอ status/ack จาก ESP32

### 7.4 ESP32 (ประหยัด data)

```
loop:
  MQTT (TLS 8883) เชื่อมต่อค้าง:
    on control:
      ถ้า expiresAt > now และ commandId ใหม่:
        execute (open/close)
        publish status + ack
    publish status ทุก 5 นาที + เมื่อ state เปลี่ยน

  ทุก 15 นาที (หรือเมื่อได้รับ MQTT notify ว่า config เปลี่ยน):
    POST /v1/devices/sync
      body: {status, lastCommandId, firmwareVersion}
      response: {revision, mode, schedules, command, serverTime}
    ถ้า revision ใหม่:
      อัปเดต mode/schedules ลง NVS
    ถ้ามี command ค้างและยังไม่หมดอายุ:
      execute → POST /ack

  รัน schedule engine ในเครื่องทุก 10 วิ
  ตรวจ max runtime
```

**หมายเหตุ:** `POST /devices/sync` รวม status + desired ไว้ในคำขอเดียว ลด TLS handshake/request overhead เมื่อเทียบกับ GET desired + POST status แยกกัน

### 7.5 Conflict ตัวอย่าง

- เครื่อง A เปิดหน้า (revision 42)
- เครื่อง B เปลี่ยน mode เป็น auto → revision 43
- เครื่อง A เปลี่ยน schedule แล้วส่ง baseRevision 42 → 409
- แอป A โหลด state 43 → แสดง "มีคนเปลี่ยนโหมดเป็น auto" → ผู้ใช้ตัดสินใจ

---

## 8. Safety design (สำคัญที่สุด)

### 8.1 Valve max runtime

- ตั้ง `MAX_OPEN_MINUTES` (default 30, ปรับได้ใน settings)
- ถ้าเปิดต่อเนื่องเกิน → ปิดทันที + รายงาน `fault: "max_runtime"`
- ต้องมีที่ firmware เป็นหลัก ไม่พึ่ง cloud

### 8.2 Command TTL

- คำสั่งใน D1 queue มีอายุ **20 นาที** (ครอบ sync 15 นาที + retry)
- MQTT real-time command ใช้ TTL สั้น (60 วิ) ได้ เพราะส่งถึงอุปกรณ์ทันที
- ESP32 ignore command ที่ `expiresAt < now`
- `GET /commands` mark คำสั่ง pending ที่เลย TTL เป็น `expired` + `error='ttl'` (ยังไม่มี cron trigger)
- สร้าง command ใหม่จะ expire pending เดิมก่อน → ไม่มีคำสั่งเก่าค้างมาทำงานย้อนหลัง
- ป้องกันคำสั่งเก่าจาก KV ที่ค้างเป็นเดือน (ตามที่พบใน live)

### 8.3 Relay fail-safe

- กำหนด `RELAY_ACTIVE_LEVEL` เป็น config/diagram
- ตอน boot: ปิด relay (valve closed) เสมอ เว้นแต่มีเหตุผลเฉพาะ
- ตรวจด้วย multimeter ว่าขา HIGH/LOW ตรงกับ relay module
- ถ้าเป็น active-low: `digitalWrite(pin, on ? LOW : HIGH)` — **ห้ามเดา**

### 8.4 Watchdog

- `esp_task_wdt` หรือ watchdog ภายนอก
- ถ้า loop ค้าง → reset → valve ปิดตอน boot

### 8.5 Offline behavior

- ESP32 รัน schedule จาก NVS ได้แม้ไม่มีเน็ต
- ถ้าเน็ตหลุดนาน: ยังรัน schedule ต่อไป แต่ max runtime ยังบังคับ
- แอปแสดง "ข้อมูลล่าสุดเมื่อ..." ไม่หลอกว่า real-time

### 8.6 Manual override

- ปุ่ม physical (optional) สำหรับปิดวาล์วฉุกเฉิน
- ปุ่มนี้ต้องทำงานได้แม้ firmware/cloud ค้าง

### 8.7 Audit

- บันทึกทุกการเปลี่ยน mode/schedule/command: ใคร, เมื่อไร, ค่าเก่า/ใหม่
- ใช้ debug และ accountability

---

## 9. Security

### 9.1 Backend

- **Auth ทุก endpoint** ยกเว้น `/health`, `/auth/join`, `/auth/login`
- **CORS allowlist** เฉพาะ Pages domain/localhost; native app ไม่ต้องใช้ CORS
- **Rate limit** login/join: 5 failed attempts/minute/IP (shared bucket), password change: separate 5/minute/IP bucket; `429 RATE_LIMITED`
- **Command rate limit**: 10 commands/minute/member (per member, not per IP); `429 RATE_LIMITED` + `retryAfter`
- **Input validation** ทุก field
- **Secrets** ผ่าน `wrangler secret put`:
  - `SETUP_TOKEN` (ใช้ครั้งเดียวตอนสร้าง household)
  - `MQTT_USER` / `MQTT_PASS` (เฉพาะ Worker เก่าที่ยังใช้ MQTT)
- **Token hashing**: session/device token เป็น random 256-bit; D1 เก็บ SHA-256 hash (ไม่ต้องใช้ pepper เพราะ entropy สูงอยู่แล้ว)
- **ไม่ log** token/password
- **Device token** แยกจาก user token; revoke ได้
- **HTTPS only**; HSTS

### 9.2 ESP32

- เปลี่ยนจาก `setInsecure()` เป็น `setCACert()` (Cloudflare root CA หรือ pin)
- เก็บ APN/MQTT/device token ใน NVS หรือ provisioning
- อย่า hardcode APN password/MQTT credential ใน source
- ถ้าใช้ MQTT: buffer ≥ 1024, TLS, unique client id, ACL
- OTA update (optional) พร้อม signature

### 9.3 Android

- **ไม่ hardcode MQTT credential** — ใช้ user แยกที่จำกัดสิทธิ์เฉพาะ topic `myhouse/valve/control` (publish) และ `myhouse/valve/status` (subscribe) ถ้า HiveMQ ทำ ACL ได้
- ถ้า HiveMQ ไม่รองรับ ACL: ยอมรับความเสี่ยงชั่วคราวหรือทำ Worker->MQTT relay ก่อนใช้งานจริง
- Session token ใน **EncryptedSharedPreferences** หรือ DataStore + Keystore
- `android:usesCleartextTraffic="false"`
- จำกัด `allowBackup` ไม่ให้ backup token
- Certificate pinning (optional)
- ProGuard/R8 สำหรับ release
- ไม่ log token/password

### 9.4 Web UI (ถ้าเก็บไว้)

- ใช้ API v2 + auth
- ลบ CORS `*`
- เก็บเป็น admin console หรือทิ้ง

---

## 10. แผน migration

### Phase 0 — Backup & baseline (0.5 วัน)

- [ ] Backup KV `config`/`state`/`command` (export JSON)
- [ ] Backup `index.html`, firmware, Android project
- [ ] `git init` ใน Android project
- [ ] Tag/commit สถานะปัจจุบัน
- [ ] จด hardware config: relay pin, active level, APN/4G, MQTT

### Phase 1 — Backend v2 (4–6 วัน)

- [ ] สร้าง D1 database + schema
- [ ] Migrate config จาก KV → D1 (script)
- [ ] Auth: households/members/sessions + join/login/logout
- [ ] Households/devices + device token
- [ ] `GET /state`, `PUT /config` + revision
- [ ] `POST /commands` + TTL + idempotency
- [ ] Device endpoints: desired/status/ack
- [ ] Validation + error format
- [ ] CORS allowlist + rate limit
- [ ] Unit/integration tests (schedule validation, revision conflict, command expiry)
- [ ] Deploy เป็น staging URL

**Acceptance:** สอง client เรียก GET/PUT พร้อมกันแล้วได้ 409 ถูกต้อง; command หมดอายุแล้วไม่ถูกส่ง

### Phase 2 — Android native read-only (3–4 วัน)

- [ ] เพิ่ม Compose/deps
- [ ] Login screen (หรือ token ใส่ใน settings ชั่วคราว)
- [ ] Dashboard แสดง mode/schedules/valve status จาก API
- [ ] Room cache + loading/error/empty states
- [ ] Polling 15 วิ + pull-to-refresh
- [ ] Feature flag: native UI vs WebView เดิม
- [ ] Build/run บนเครื่องจริง

**Acceptance:** เปิดแอปเห็นค่าล่าสุดจาก Worker; เปลี่ยนค่าจาก web/เครื่องอื่นแล้วอีกเครื่องเห็นภายใน 15 วิ

### Phase 3 — Android native write (3–5 วัน)

- [ ] เปลี่ยน mode
- [ ] เพิ่ม/แก้/ลบ schedule + day toggles
- [ ] เปิด/ปิดวาล์ว + countdown
- [ ] Conflict dialog (409)
- [ ] Command status (pending/acked/expired)
- [ ] Settings: server URL, account, device
- [ ] ลบ WebView/`WebAppInterface`/MQTT ออกจาก flow หลัก
- [ ] Accessibility pass

**Acceptance:** ใช้งานได้ครบโดยไม่ต้องโหลด WebView; ไม่มี MQTT credential ในแอป

### Phase 4 — Firmware v2 (3–5 วัน)

- [ ] ArduinoJson แทน string parsing
- [ ] Device token + desired/ack
- [ ] Overnight schedule fix (ทั้ง Worker และ firmware)
- [ ] Command TTL + idempotency
- [ ] Max runtime + watchdog
- [ ] Relay active level config
- [ ] `setCACert()` แทน `setInsecure()`
- [ ] `mqtt.setBufferSize(1024)` + MQTT เป็นช่องทาง real-time หลัก
- [ ] Status report ครบ (valve, mode, rssi, uptime, fault)
- [ ] OTA (optional)

**Acceptance:** ถอดเน็ตแล้ว schedule ยังทำงาน; คำสั่งเก่าไม่ execute; เปิดนานเกิน max runtime แล้วปิดเอง

### Phase 5 — Real-time / multi-user / polish (3–5 วัน)

- [ ] SSE/WebSocket (ถ้าต้องการ)
- [ ] Push notification (FCM)
- [ ] Audit log UI
- [ ] Multi-device UI
- [x] Rate limit login/join/password change (5 failures/min/IP) + audit log
- [ ] Observability (Workers logs/metrics, alerting)
- [ ] Accessibility 90+

### Phase 6 — Cleanup (1–2 วัน)

- [ ] ลบ KV config/command เก่า (หลัง migrate)
- [ ] ตัดสินใจเรื่อง MQTT ระยะยาว (คงไว้ / Worker relay / WSS+DO)
- [ ] ตัดสินใจเก็บ/ทิ้ง Web UI
- [ ] อัปเดต README/runbook
- [ ] Backup/restore test

---

## 11. โครงสร้าง Android ที่แนะนำ

```
com.example.esp32/
├── App.kt
├── MainActivity.kt
├── data/
│   ├── api/
│   │   ├── ApiClient.kt
│   │   ├── AuthInterceptor.kt
│   │   ├── dto/
│   │   │   ├── AuthDto.kt
│   │   │   ├── StateDto.kt
│   │   │   ├── ConfigDto.kt
│   │   │   └── CommandDto.kt
│   │   └── WaterValveApi.kt
│   ├── local/
│   │   ├── AppDatabase.kt
│   │   ├── ConfigDao.kt
│   │   ├── SessionStore.kt
│   │   └── entity/
│   └── repository/
│       ├── AuthRepository.kt
│       ├── ConfigRepository.kt
│       └── DeviceRepository.kt
├── domain/
│   ├── model/
│   │   ├── Mode.kt
│   │   ├── Schedule.kt
│   │   └── DeviceStatus.kt
│   └── usecase/
│       ├── ObserveStateUseCase.kt
│       ├── SaveConfigUseCase.kt
│       └── SendCommandUseCase.kt
├── ui/
│   ├── login/LoginScreen.kt
│   ├── dashboard/DashboardScreen.kt
│   ├── schedules/SchedulesScreen.kt
│   ├── settings/SettingsScreen.kt
│   ├── components/
│   └── theme/
└── util/
    ├── ScheduleValidator.kt
    └── Time.kt
```

**หลัก:** UI ไม่เรียก Retrofit ตรง ๆ; ทุกอย่างผ่าน Repository → ง่ายต่อ test และ cache

---

## 12. แผนทดสอบ

### 12.1 Unit

- Schedule active: ปกติ, ข้ามคืน, ข้ามวัน, disabled, หลายช่วงซ้อน
- Validation: เวลาผิด, days length, id ซ้ำ
- Revision: update สำเร็จ/409
- Command: TTL, idempotency, replacement

### 12.2 Integration (Worker + D1)

- Login → GET state → PUT config → GET เห็นค่าใหม่
- สอง client PUT พร้อมกัน → คนหลังได้ 409
- Command หมดอายุ → desired ไม่ส่ง
- Device token ผิด → 401
- CORS: origin ที่ไม่อนุญาต → ไม่มี header

### 12.3 Firmware

- NTP sync → schedule เปิด/ปิดถูก
- Overnight 22:00–06:00
- MQTT payload ใหญ่ 1 KB
- 4G หลุด/กลับ
- Command เก่า/หมดอายุ
- Max runtime
- Relay active level (bench test)

### 12.4 Android

- Login/logout
- Rotate/process death
- Offline → cache + error message
- Conflict dialog
- Polling stop ตอน background
- Accessibility (TalkBack)

### 12.5 E2E

- เครื่อง A เปลี่ยน mode → เครื่อง B เห็นภายใน 15 วิ
- เครื่อง A เปิดวาล์ว → ESP32 เปิด → status กลับมาทั้งสองเครื่อง
- ปิด 4G ESP32 → schedule ยังทำงาน → กลับมาออนไลน์ status sync
- Command ค้าง → หมดอายุ → ไม่ execute

---

## 13. ประมาณการงาน

| Phase | งาน | ประมาณ |
|---|---|---|
| 0 | Backup/baseline | 0.5 วัน |
| 1 | Backend v2 | 4–6 วัน |
| 2 | Android read-only | 3–4 วัน |
| 3 | Android write + ลบ WebView | 3–5 วัน |
| 4 | Firmware v2 | 3–5 วัน |
| 5 | Real-time/push/polish | 3–5 วัน |
| 6 | Cleanup/QA | 1–2 วัน |
| **รวม MVP (Phase 0–4)** | | **14–21 วัน** |
| **รวม full** | | **18–28 วัน** |

> เป็นการประมาณสำหรับนักพัฒนาคนเดียวที่คุ้นโปรเจกต์; ขึ้นกับคุณภาพ UI, จำนวนอุปกรณ์, และเวลา debug ฮาร์ดแวร์

---

## 14. Decision log

| # | การตัดสินใจ | ทางเลือก | เหตุผล |
|---|---|---|---|
| D1 | ใช้ D1 เป็น source of truth | KV vs D1 | transaction/revision/query |
| D2 | App polling 15 วิ (foreground); ESP32 MQTT + sync 15 นาที | polling ถี่ vs MQTT | ประหยัด data บนซิม 399 MB/เดือน |
| D3 | Compose | Compose vs XML | native, state-driven, อนาคตดี |
| D4 | Invite code + household password | email/OAuth/Access | ง่ายสำหรับครอบครัว, ตรงกับที่เลือก |
| D5 | Device token แยก | shared user token | ความปลอดภัย/revoke |
| D6 | Single device per household (v1) | multi-device | ลด scope; schema รองรับภายหลัง |
| D7 | เก็บ Web UI เป็น admin (optional) | ทิ้ง | ใช้ debug/ตั้งค่าจากคอมได้ |
| D8 | MQTT เป็นช่องทาง real-time หลักของ ESP32 | ลบทั้งหมด / polling | ประหยัด data ที่สุด; app ใช้ scoped credential |
| D9 | 1 วาล์วใน v1 | multi-valve | ตรงกับ hardware/ความต้องการ |
| D10 | Backend + native UI คู่ขนาน | ทำทีละส่วน | ตรงกับที่เลือก; ใช้ API contract ร่วม |

---

## 15. คำถามที่ยังต้องยืนยัน

1. **ฮาร์ดแวร์** — relay เป็น active-high หรือ active-low? มีปุ่มฉุกเฉิน/flow sensor ไหม?
2. **Cloudflare** — ใช้ account/แผนเดิม (Free/Paid) หรือมีโดเมนของตัวเอง? D1/DO ใช้ได้ตามแผนไหม?
3. **MQTT credential** — HiveMQ Cloud สร้าง user แยกและจำกัดสิทธิ์ topic ได้ไหม? ถ้าไม่ได้ จะยอมรับความเสี่ยงหรือทำ Worker relay?
4. **Web UI เดิม** — เก็บเป็น admin console หรือทิ้ง?
5. **ประวัติ/แจ้งเตือน** — ต้องมี audit log/push notification ตั้งแต่ phase แรกไหม?
6. **เกณฑ์สำเร็จ** — "เห็นค่าภายใน 15 วิ" พอไหม และรองรับกี่เครื่อง?
7. **OTA firmware** — ต้องมีตั้งแต่เวอร์ชันนี้ไหม?
8. **ข้อมูลตั้งต้น** — ใครเป็นคนสร้าง household/invite code ครั้งแรก และเก็บรหัสอย่างไร?

---

## 16. สรุป

- สถาปัตยกรรมนี้ตอบโจทย์ **central config + native Android** ได้ครบ
- จุดเปลี่ยนสำคัญคือ **read path + revision + auth + command TTL/ack**
- **ข้อจำกัดใหม่:** ESP32 ใช้ซิม 399 MB/เดือน → ใช้ MQTT เป็นช่องทาง real-time หลัก + HTTPS sync ทุก 15 นาที (ประเมิน ~10–30 MB/เดือน)
- เริ่ม Phase 0–1 (backend) คู่ขนานกับ Phase 2 (Android read-only) ตามที่เลือก; Phase 3–4 ตามด้วย
- อย่าใช้กับวาล์วน้ำจริงจนกว่า safety (C5) และ auth (C1) จะเสร็จ
- รายละเอียดการย้าย Android อยู่ใน `docs/MIGRATION_PLAN.md` ของ repository แอปพลิเคชัน (แยกจาก repository นี้)

---

## 17. Firmware v2 implementation notes (2026-10-30)

These notes supersede the optimistic data estimate above.

### 17.1 One TLS session on A7672X

TinyGSM 0.12.0's A7672X driver hardcodes SSL session/context `0` and routes
URCs through `sockets[mux]`. Two `TinyGsmClientSecure` instances (MQTT on
mux 0 + HTTPS on mux 1) collide: `+CCHOPEN=0` is reused and `+CCHRECV=1`
targets a session that is not open. The firmware therefore uses **one**
`TinyGsmClientSecure` shared by PubSubClient and the HTTPS client.

Sync sequence:

1. `mqtt.disconnect()` (DISCONNECT + `+CCHCLOSE=0`)
2. HTTPS `POST /v1/devices/sync`
3. HTTPS ack for a command delivered by sync (if any)
4. `mqttConnect()` on the next loop iteration

MQTT is offline for a few seconds every 15 minutes. Commands sent only over
MQTT during that window are lost; the Android app must retry or use
`POST /v1/commands` so D1 queues the command for the next sync.

### 17.2 Ack path

The HTTP ack endpoint cannot be called while MQTT owns the TLS session.
Therefore:

- MQTT command success is reported in `status.lastCommandId`.
- MQTT command rejection is reported in `status.lastRejectedId` /
  `lastRejectedError`.
- The next `/v1/devices/sync` includes `lastCommandId`, and
  `updateDeviceStatus()` marks that pending command as `acked` in D1.
- HTTP `POST /v1/devices/commands/:id/ack` is only used for commands that
  arrive inside the sync response.

### 17.3 Data budget correction

A fresh TLS handshake per sync (MQTT reconnect + HTTPS) costs more than the
earlier 10–30 MB/month estimate. Realistic range: **~55–105 MB/month**
(see `esp32_water_valve/README.md`). Still well under 399 MB, but verify on
the carrier dashboard after the first week.

### 17.4 HTTP client

ESP32 core 3.3.11's `HTTPClient::begin()` requires `NetworkClient&`, which
TinyGSM's `GsmClient` does not implement. The firmware uses a small
HTTP/1.1 client over the shared TLS socket, with `Content-Length` and
chunked-body support, instead of `HTTPClient`.

### 17.5 Hardware verification still required

- Relay active level (`RELAY_ACTIVE_HIGH`, default 1 = HIGH opens).
- A7672X CA upload (`AT+CCERTDOWN` / `AT+CSSLCFG="cacert"`) and TLS
  handshake on the carrier network.
- HiveMQ ACL for the scoped user/topics.
- Power stability under cellular transmit bursts (2 A peaks).

---

## 18. Production deployment (2026-10-31)

Worker v2 and D1 are deployed and verified end-to-end.

| Item | Value |
|---|---|
| Worker URL | `https://water-valve-api-v2.czqwertyyy.workers.dev` |
| Worker name | `water-valve-api-v2` |
| D1 database | `water-valve-v2` (APAC; `database_id` อยู่ใน `wrangler.v2.toml`) |
| Schema | `migrations/0001_init.sql` + `0002_auth_attempts.sql` + `0003_rate_limits.sql` applied remotely |
| Health | `GET /health` → `200 {"version":"2.0.0"}` |
| Setup token | stored as Worker secret `SETUP_TOKEN`; the local copy is kept outside the repository |
| API smoke test | 95/95 checks pass against production (auth, rate limit, state, config revision/If-Match, 8-schedule limit, devices-aware ETag, commands, command rate limit, sync/ack, cross-household isolation, device-token scope, logout revocation, password/device-token rotation); test households and rate-limit rows deleted afterwards |

Deploy/update commands:

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0003_rate_limits.sql
$env:SETUP_TOKEN = '<setup-token>'
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
npx wrangler deploy --config wrangler.v2.toml
```

Run the API tests against production:

```powershell
$env:API_BASE = 'https://water-valve-api-v2.czqwertyyy.workers.dev'
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

The old `water-valve-scheduler` Worker and Pages site are untouched; they
remain as the legacy system until the native app/firmware are accepted.

Android debug builds now default to the production URL. For local testing:

```powershell
.\gradlew.bat assembleDebug -PapiBaseUrl=http://10.0.2.2:8787/
```

Firmware defaults to the production URL; `set url ...` can override it in NVS.


