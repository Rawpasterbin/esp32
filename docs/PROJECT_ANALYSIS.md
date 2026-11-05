# รายงานวิเคราะห์โปรเจกต์ appesp32 (Water Valve Scheduler)

- **วันที่ตรวจ:** 29 ตุลาคม 2026
- **ขอบเขต:** Web UI (`index.html`), Cloudflare Worker (`worker/schedule-worker.js`), ESP32 firmware (`esp32_water_valve/esp32_water_valve.ino`), Android app (repository แยก)
- **วิธีตรวจ:** อ่าน source ทั้งหมด, ตรวจ syntax JS/Worker, compile firmware, เรียก Worker ที่ deploy จริง, ทดสอบ logic ของ schedule/countdown/preview, ตรวจ Lighthouse
- **เอกสารที่เกี่ยวข้อง:**
  - [`ARCHITECTURE_CENTRAL_CONFIG.md`](./ARCHITECTURE_CENTRAL_CONFIG.md) — สถาปัตยกรรมเป้าหมายสำหรับ central config + native Android
  - `docs/MIGRATION_PLAN.md` (repository แอปพลิเคชัน) — แผนย้าย Android จาก WebView เป็น native

---

## 1. บทสรุป

### 1.1 ระบบปัจจุบันทำอะไรได้

โปรเจกต์นี้เป็นระบบเปิด-ปิดวาล์วน้ำผ่าน ESP32 โดยมี 4 ชิ้นส่วน:

1. **Web UI** (`index.html`) — หน้าเว็บ Tailwind ที่แสดงสถานะวาล์ว, ตั้งโหมด auto/manual, ตั้งตารางเวลา, ตั้ง countdown, และแสดงอุณหภูมิ
2. **Cloudflare Worker + KV + Cron** — เก็บ `{mode, schedules}` และคำสั่ง `{open}` ไว้ใน KV, มี cron ตรวจตารางทุกนาที
3. **ESP32 firmware** — ต่อ WiFi, รับคำสั่งผ่าน MQTT, ดึง config จาก Worker ทุก 2 นาที, รันตารางเวลาเองในเครื่อง, รายงานสถานะผ่าน MQTT
4. **Android app** — เป็น WebView wrapper โหลด `<legacy-web-ui-url>` และมี MQTT client ในตัวเพื่อรับส่งคำสั่งกับ ESP32

ระบบ **ทำงานได้ในระดับ demo/ใช้งานจริงบางส่วน**: มีโค้ดรองรับการเปิด-ปิดวาล์วผ่าน MQTT, โหมด auto ทำงานใน ESP32 ได้แม้แอปปิด, มีการเก็บ config สำรองใน Cloudflare

### 1.2 ระบบปัจจุบันยังไม่ตอบโจทย์ "ทุกเครื่องเห็น setting เดียวกัน"

**ยังไม่จริง** ในเชิงสถาปัตยกรรม เพราะ:

- Web UI **ไม่เคย GET config จาก Worker** — มีแต่ POST ไปเขียนทับ
- แต่ละเครื่องอ่าน/เขียน `localStorage` ของตัวเอง (`smartWaterValve_v1`)
- เมื่อเปิดแอปบนเครื่องใหม่ `init()` จะเอา `defaultSchedules()` ไป POST ทับ config บน Cloudflare ทันที
- ไม่มี `revision`, ไม่มี conflict detection, ไม่มี auth, ไม่มีผู้ใช้อ้างอิง
- ESP32 เป็นฝ่ายดึง config จาก Worker แต่ **แอป Android/เว็บไม่ได้ดึง** ดังนั้นมือถือเครื่องอื่นจึงไม่เห็นค่าที่เครื่องหนึ่งเพิ่งตั้ง

พูดสั้น ๆ: **Worker เป็น "ที่ฝากค่า" ไม่ใช่ "แหล่งความจริงร่วมกัน"** ในฝั่งแอป

### 1.3 คำตอบเรื่องความเป็นไปได้

**ทำได้ และเหมาะสมกับโปรเจกต์นี้** แต่ต้องเพิ่ม 4 อย่างหลัก:

1. **Read path** — ให้ทุกเครื่อง GET state/config จาก Worker (พร้อม `revision`)
2. **Revision + conflict handling** — กันการเขียนทับกันแบบเงียบ ๆ
3. **Auth** — ผู้ใช้และอุปกรณ์ต้องมี identity ไม่ใช่เปิด API สาธารณะ
4. **Native Android UI** — แทน WebView ด้วย Compose/XML และตัด MQTT credential ออกจากแอป

รายละเอียดสถาปัตยกรรมและ API อยู่ใน [`ARCHITECTURE_CENTRAL_CONFIG.md`](./ARCHITECTURE_CENTRAL_CONFIG.md)

### 1.4 ความเสี่ยงวิกฤตที่ต้องแก้ก่อนใช้งานจริง

| # | ความเสี่ยง | ผลกระทบ |
|---|---|---|
| C1 | Worker ไม่มี auth + CORS `*` | ใครก็อ่าน/เขียน config และสั่งเปิดวาล์วได้ |
| C2 | Secret WiFi/MQTT อยู่ใน source | รั่ว WiFi บ้านและ MQTT broker |
| C3 | ไม่มี central read/sync จริง | หลายเครื่องเห็นค่าไม่ตรงกัน |
| C4 | คำสั่งไม่มี TTL/ack/idempotency | คำสั่งเก่าอาจถูกใช้ตอนอุปกรณ์กลับมาออนไลน์ |
| C5 | ไม่มี max runtime / fail-safe | วาล์วอาจเปิดค้างนานเป็นชั่วโมง |
| C6 | ตารางข้ามคืนผิดทั้ง Worker และ firmware | รดน้ำผิดเวลา โดยเฉพาะช่วงหลังเที่ยงคืน |

---

## 2. โครงสร้างโปรเจกต์

```
appesp32\
├── index.html                     # Web UI ทั้งหมด (Tailwind + JS inline)
├── worker\
│   └── schedule-worker.js         # Cloudflare Worker API + cron
├── esp32_water_valve\
│   └── esp32_water_valve.ino      # Firmware ESP32 (Arduino)
├── wrangler.toml                  # Worker config, KV binding, cron, vars
├── package.json / package-lock.json
├── docs\                          # เอกสารวิเคราะห์ (โฟลเดอร์นี้)
└── .git\                          # git repo (local main ahead origin 5 commits)

android-project\
├── app\src\main\java\com\example\esp32\
│   ├── MainActivity.kt            # WebView wrapper
│   ├── MqttManager.kt             # MQTT client + credential hardcode
│   ├── CommandProcessor.kt        # map คำสั่ง -> MQTT topic
│   └── WebAppInterface.kt         # JavascriptInterface bridge
├── app\src\main\res\layout\activity_main.xml  # มีแค่ WebView
├── app\src\main\AndroidManifest.xml
├── app\build.gradle.kts
└── gradle\libs.versions.toml
```

**หมายเหตุ:** Android project ยังไม่เป็น git repo และยังไม่ได้แก้ไขไฟล์ใด ๆ ในรอบนี้

**พบสำเนาซ้ำ:** ในเครื่องที่ใช้วิเคราะห์มีโปรเจกต์ Android สองชุดที่มีไฟล์หลักเหมือนกัน (hash ของ `MainActivity.kt`, `MqttManager.kt`, `build.gradle.kts`, `settings.gradle.kts` ตรงกัน) จึงถือว่า `<android-project>` เป็นโปรเจกต์หลัก ควรเหลือชุดเดียวและเก็บอีกชุดไว้เป็น backup

---

## 3. สถาปัตยกรรมปัจจุบัน

### 3.1 ภาพรวม

```
┌──────────────────────────────┐
│ Android app (WebView)        │
│  MainActivity.kt             │
│  └─ WebView -> Pages         │
│  └─ WebAppInterface (JS)     │
│  └─ MqttManager (Paho)       │
└──────────┬───────────────────┘
           │ 1) โหลดหน้าเว็บ
           ▼
┌──────────────────────────────┐       2) fetch POST /config, /command
│ Cloudflare Pages             │ ────────────────────────────────────┐
│ <legacy-web-ui-url>          │                                     │
│  index.html + localStorage   │                                     ▼
└──────────────────────────────┘                     ┌──────────────────────────────┐
           │ 3) AndroidBridge (JS -> Kotlin)         │ Cloudflare Worker            │
           ▼                                         │ water-valve-scheduler...     │
┌──────────────────────────────┐                     │  KV: config/state/command    │
│ HiveMQ Cloud (MQTT/TLS)      │                     │  Cron: ทุก 1 นาที            │
│  myhouse/valve/#             │                     └──────────────┬───────────────┘
└──────────┬───────────────────┘                                    │
           │ 4) subscribe/publish                                  │ 5) GET / ทุก 2 นาที
           ▼                                                       ▼
┌───────────────────────────────────────────────────────────────────────────────┐
│ ESP32 Water Valve                                                             │
│  - ต่อ WiFi, NTP UTC+7                                                        │
│  - MQTT: control / mode / schedule / status                                   │
│  - HTTPS: ดึง config + command จาก Worker                                     │
│  - รัน schedule engine เองในเครื่อง (ทุก 10 วิ)                               │
│  - relay ขา 23                                                               │
└───────────────────────────────────────────────────────────────────────────────┘
```

### 3.2 เส้นทางการไหลของข้อมูล

| # | เส้นทาง | ผู้ริเริ่ม | ปลายทาง | ความถี่ |
|---|---|---|---|---|
| 1 | Web UI -> AndroidBridge -> MQTT -> ESP32 | ผู้ใช้กดปุ่ม | ESP32 | ทันที (ถ้า MQTT ต่อ) |
| 2 | Web UI -> Worker `POST /config` | เปลี่ยน mode/schedule | KV | ทุกครั้งที่แก้ |
| 3 | Web UI -> Worker `POST /command` | เปิด/ปิดวาล์ว manual | KV | ทุกครั้งที่กด |
| 4 | ESP32 -> Worker `GET /` | firmware | ESP32 | ทุก 2 นาที |
| 5 | ESP32 -> MQTT `status` | firmware | Android -> WebView | ทุก 60 วิ |
| 6 | Web UI -> `localStorage` | ทุกการเปลี่ยนแปลง | เครื่องนั้น | ทันที |
| 7 | Worker cron -> KV `state` | cron | KV | ทุก 1 นาที (เฉพาะ auto) |

**จุดสำคัญ:** ไม่มีเส้นทางที่ Web UI/Android ดึงค่าจาก Worker กลับมาแสดงเลย มีแต่ ESP32 ที่ดึง

---

## 4. Data model ปัจจุบัน

### 4.1 Worker KV

| Key | รูปแบบ | ตัวอย่าง |
|---|---|---|
| `config` | `{mode, schedules[], updatedAt}` | `{"mode":"manual","schedules":[...],"updatedAt":"2026-10-01T22:22:13.091Z"}` |
| `state` | `{shouldOpen, at}` | `{"shouldOpen":false,"at":"2026-09-29T07:31:40.555Z"}` |
| `command` | `{open, at}` | `{"open":false,"at":"2026-08-02T14:50:19.639Z"}` |

### 4.2 Schedule

```json
{
  "id": 1,
  "name": "รอบเช้า",
  "start": "06:00",
  "end": "06:30",
  "days": [true, true, true, true, true, false, false],
  "enabled": true
}
```

- `days` = 7 ค่า, จันทร์ = index 0 ... อาทิตย์ = index 6
- `start`/`end` = `HH:MM` เวลาไทย (UTC+7)
- `end` เป็นแบบ exclusive (นาทีที่ `end` ไม่ถือว่าอยู่ในช่วง)

### 4.3 localStorage (Web UI)

Key: `smartWaterValve_v1`

```json
{
  "mode": "manual",
  "valveOn": false,
  "schedules": [ ... ],
  "countdown": {
    "active": false,
    "totalSeconds": 0,
    "endTime": null,
    "savedAt": 0
  },
  "savedAt": 0
}
```

### 4.4 ESP32 NVS (Preferences namespace `valve`)

| Key | ค่า |
|---|---|
| `mode` | `auto` / `manual` |
| `schedules` | JSON array string |
| `cmdAt` | timestamp ของ command ล่าสุดที่ใช้แล้ว |

### 4.5 MQTT topics

| Topic | ทิศทาง | Payload |
|---|---|---|
| `myhouse/valve/control` | App -> ESP32 | `{"cmd":"open"}`, `{"cmd":"close"}`, `{"cmd":"toggle"}`, `{"cmd":"getStatus"}`, `{"cmd":"reboot"}` หรือ `{"type":"valve","action":"on"}` |
| `myhouse/valve/mode` | App -> ESP32 | `{"type":"mode","mode":"auto"}` |
| `myhouse/valve/schedule` | App -> ESP32 | `{"type":"schedule","schedules":[...]}` |
| `myhouse/valve/status` | ESP32 -> App | `{"valve":false,"online":true,"mode":"manual","scheduleCount":3,"ntp":true,"time":...}` |
| `myhouse/notification` | App -> broker | `{"title":"...","message":"..."}` (ยังไม่มีผู้รับ) |

---

## 5. API ปัจจุบัน

Base URL: `<legacy-worker-url>/`

| Method | Path | Auth | Body | Response | ผู้ใช้ |
|---|---|---|---|---|---|
| `GET` | `/` | ไม่มี | — | `{ok, nowMin, dayOfWeek, config, state, command}` | ESP32, debug |
| `POST` | `/config` | ไม่มี | `{mode, schedules}` | `{ok, stored}` | Web UI |
| `POST` | `/command` | ไม่มี | `{open: boolean}` | `{ok, stored}` | Web UI |
| `OPTIONS` | `*` | — | — | 204 + CORS | browser preflight |

**ข้อสังเกต:**

- ไม่มี endpoint สำหรับอ่าน config แบบแยก (อ่านได้จาก `GET /` ทั้งก้อน)
- ไม่มี endpoint สำหรับสถานะอุปกรณ์จริง (status วิ่งทาง MQTT อย่างเดียว)
- ไม่มี pagination/versioning/health check
- CORS อนุญาตทุก origin

### 5.1 ผลการเรียก Worker ที่ deploy จริง (29 ต.ค. 2026)

```json
{
  "ok": true,
  "nowMin": 1115,
  "dayOfWeek": 3,
  "config": {
    "mode": "manual",
    "schedules": [
      {"id":1,"name":"รอบเช้า","start":"06:00","end":"06:30","days":[true,true,true,true,true,false,false],"enabled":true},
      {"id":2,"name":"รอบเย็น","start":"17:30","end":"18:00","days":[true,true,true,true,true,true,true],"enabled":true},
      {"id":3,"name":"รอบบ่าย","start":"12:00","end":"12:15","days":[false,false,true,true,true,false,false],"enabled":false}
    ],
    "updatedAt": "2026-10-01T22:22:13.091Z"
  },
  "state": {"shouldOpen": false, "at": "2026-09-29T07:31:40.555Z"},
  "command": {"open": false, "at": "2026-08-02T14:50:19.639Z"}
}
```

- `state.at` เก่ากว่า 1 เดือน และ `command.at` เก่ากว่า 2 เดือน → ยืนยันว่า cron/command ไม่ได้ถูกใช้ต่อเนื่อง และไม่มีกลไกหมดอายุ
- โหมดเป็น `manual` จึงทำให้ cron `runCron()` return ทันทีและไม่เขียน `state` ใหม่

---

## 6. Android app ปัจจุบัน

### 6.1 สรุป

| หัวข้อ | สถานะ |
|---|---|
| รูปแบบ UI | WebView 100% (`activity_main.xml` มีแค่ `WebView`) |
| หน้าเว็บ | `<legacy-web-ui-url>` |
| การเชื่อมต่ออุปกรณ์ | MQTT ผ่าน Paho โดยตรงจากแอป |
| Credential | hardcode ใน `MqttManager.kt` |
| สถิติ build | minSdk 24, compile/target 34, Kotlin 1.9.24, AGP 8.7.3 |
| Dependency | AppCompat, Core KTX, Material, Paho MQTT 1.2.5, Gson 2.10.1 |
| Compose | ยังไม่มี |
| Retrofit/OkHttp | ยังไม่มี |
| Git | ยังไม่เป็น repo |

### 6.2 จุดเชื่อมต่อ WebView <-> Kotlin

`MainActivity.kt`:

```kotlin
findViewById<WebView>(R.id.webView).apply {
    settings.javaScriptEnabled = true
    settings.domStorageEnabled = true
    webViewClient = WebViewClient()
    addJavascriptInterface(
        WebAppInterface(this, commandProcessor, mqttManager),
        "AndroidBridge"
    )
    loadUrl("<legacy-web-ui-url>")
}
```

`WebAppInterface` เปิด method ให้ JS เรียก:

- `triggerAction(command: String)`
- `sendNotification(title, message)`
- `getMqttStatus()`

และเรียก JS กลับผ่าน `evaluateJavascript("window.<fn>(<json>)")`

### 6.3 MQTT

`MqttManager.kt`:

- `ssl://<hivemq-cluster-host>:8883`
- username/password แบบ hardcode (legacy — ลบออกจาก repo แล้ว; ต้อง rotate ที่ HiveMQ)
- `cleanSession = true`, `automaticReconnect = true`
- subscribe `myhouse/valve/status` และ `myhouse/+/status`
- QoS 1

`CommandProcessor.kt` เลือก topic ตาม `type`:

- `mode` -> `myhouse/valve/mode`
- `schedule` -> `myhouse/valve/schedule`
- อื่น ๆ -> `myhouse/valve/control`

### 6.4 ปัญหาเฉพาะของ Android

- WebView โหลดหน้าเว็บจากอินเทอร์เน็ตทุกครั้ง → เปิดช้า, ขึ้นอยู่กับ Pages, UI ไม่ใช่ native
- `usesCleartextTraffic="true"` ใน manifest (ไม่จำเป็นสำหรับ HTTPS/MQTT TLS)
- `allowBackup="true"` อาจสำรอง token/credential ในอนาคต
- MQTT credential อยู่ใน APK → ใครแตก APK ก็ได้
- `onResume()` เรียก `evaluateJavascript` ทุกครั้ง → ถ้า WebView ยังโหลดไม่เสร็จจะไม่มีผล
- ไม่มี offline cache/state ของตัวเอง
- ไม่มี auth ผู้ใช้
- ไม่มี notification จริง แม้ขอ permission ไว้แล้ว
- `MainActivity` ผูก MQTT lifecycle กับ Activity โดยตรง ไม่มี foreground service

---

## 7. ผลการตรวจสอบและทดสอบ

### 7.1 สิ่งที่ผ่าน

| รายการ | ผล |
|---|---|
| JS syntax (`index.html`) | ผ่าน |
| Worker syntax (`schedule-worker.js`) | ผ่าน |
| ESP32 compile | ผ่าน 1,033,488 bytes (78% flash), RAM 48,904 bytes (14%) |
| Worker GET live | 200, คืน config/state/command ครบ |
| MQTT broker | พบ credential hardcode; ยังไม่ได้ทดสอบเชื่อมต่อในรอบนี้ |
| Android build | ยังไม่ได้ build ใหม่ในรอบนี้ (พบ `hs_err_pid2612.log`/`replay_pid2612.log` จาก build เดิม) |

### 7.2 Lighthouse

- Accessibility: **72/100**
- ปัญหาหลัก: ปุ่ม toggle mode ซ่อนปุ่มที่ไม่ได้เลือก, contrast ของข้อความบางจุด, label ของ input, ขนาด touch target

### 7.3 Behavioral tests ที่พบปัญหา

| Test | ผลที่คาด | ผลจริง |
|---|---|---|
| ตาราง 22:00–06:00 วันจันทร์ | เปิดต่อเนื่องถึง 06:00 อังคาร | หลังเที่ยงคืนตรวจ `days[อังคาร]` → ปิด (ถ้าอังคารไม่เปิด) |
| เปิดแอปใหม่หลังตั้งค่าไว้ | ค่า `valveOn` เดิมกลับมา | ไม่ restore `valveOn` ยกเว้นมี countdown active |
| `schedules: [null]` ใน localStorage | ถูกกรองออก | `normalizeSchedule` คืน `null` แล้ว `renderSchedules` crash |
| กด Reset | ปิดวาล์วด้วย | ตั้ง `State.valveOn=false` แต่ไม่ส่งคำสั่งปิดไป ESP32/Worker |
| Countdown หมดเวลา | ปิดวาล์วทั้ง MQTT และ Cloudflare | ส่งแค่ MQTT (`sendCommandToEsp`) ไม่ POST `/command` |
| เปิดแอปเครื่องใหม่ | เห็น config จาก cloud | `init()` เอา default ไป POST ทับ cloud |
| แก้ตารางบนเครื่อง A | เครื่อง B เห็นตาม | เครื่อง B ใช้ localStorage ตัวเอง ไม่ GET จาก cloud |

---

## 8. ปัญหาและความเสี่ยงโดยละเอียด

### 8.1 Critical

#### C1 — Worker เปิดสาธารณะ ไม่มี auth

**ตำแหน่ง:** `worker/schedule-worker.js` บรรทัด 19–23 (CORS), 103–137 (POST), 139–145 (GET)

**อาการ:**

- ใครรู้ URL ก็ `GET /` ดู config/command ได้
- ใครก็ `POST /config` เปลี่ยน mode/schedules ได้
- ใครก็ `POST /command` สั่งเปิดวาล์วได้
- CORS `Access-Control-Allow-Origin: *` เปิดให้ทุกเว็บเรียก

**ผลกระทบ:** ความปลอดภัยของบ้าน/ทรัพย์สิน และค่าน้ำ

**แนวทางแก้:**

- เพิ่ม auth (user token + device token)
- จำกัด CORS เฉพาะ origin ที่รู้จัก
- แยก endpoint อ่าน/เขียน และเพิ่ม rate limit
- เก็บ audit log

#### C2 — Secret อยู่ใน source

**ตำแหน่ง:**

- `esp32_water_valve.ino` บรรทัด 10–16: WiFi SSID/password, MQTT user/pass
- `wrangler.toml`: เดิมมี `MQTT_USER`, `MQTT_PASS` แบบ hardcode — ลบออกแล้ว; ใช้ `wrangler secret put` และ rotate credential
- `MqttManager.kt` บรรทัด 13–16: broker URL, user, pass

**ผลกระทบ:** รั่ว WiFi, broker, และสามารถ publish คำสั่งเปิดวาล์วได้

**แนวทางแก้:**

- ย้าย secrets ไป `wrangler secret put`
- ใช้ provisioning (WiFiManager/serial config) แทน hardcode
- เปลี่ยน MQTT credential
- Android ไม่ควรถือ MQTT credential อีกต่อไป (ให้คุยกับ Worker API แทน)

#### C3 — ไม่มี central sync จริง

**ตำแหน่ง:**

- `index.html` บรรทัด 580 (`apiWorkerUrl`), 814–887 (localStorage), 1515–1527 (`syncToCloudflare`)
- ไม่มีฟังก์ชัน `fetch` แบบ GET ไป Worker เลย

**อาการ:**

- แอปแต่ละเครื่องอ่าน `localStorage` ของตัวเอง
- `init()` (บรรทัด 1570–1588) เอา default schedules ไป POST ทับ cloud
- ไม่มี revision/conflict

**ผลกระทบ:** ผู้ใช้ตั้งค่าบนเครื่องหนึ่ง อีกเครื่องไม่เห็น; ถ้าเปิดเครื่องใหม่ทับค่าเดิม

**แนวทางแก้:** ตาม [`ARCHITECTURE_CENTRAL_CONFIG.md`](./ARCHITECTURE_CENTRAL_CONFIG.md) — GET state + revision, PUT แบบ optimistic concurrency, polling/SSE

#### C4 — Command ไม่มี TTL/ack/idempotency

**ตำแหน่ง:** `worker/schedule-worker.js` บรรทัด 122–137; `esp32_water_valve.ino` บรรทัด 247–271

**อาการ:**

- `/command` เขียน KV ค่าเดียว `{open, at}` ไม่มีวันหมดอายุ
- ESP32 ใช้ `lastCmdAt` กันซ้ำ แต่ถ้าอุปกรณ์ออฟไลน์ตอนสั่ง แล้วกลับมาออนไลน์ภายหลัง จะใช้คำสั่งเก่านั้น
- ไม่มี ack ว่าคำสั่งถูก execute จริง
- คำสั่งใหม่ทับคำสั่งเก่าก่อนที่อุปกรณ์จะอ่าน (last write wins)

**ผลกระทบ:** วาล์วเปิด/ปิดผิดเวลา, คำสั่งค้างเป็นวัน

**แนวทางแก้:**

- `commands` table + `expires_at` (เช่น 60 วิ) + `status`
- `idempotency_key`
- device ack endpoint
- ESP32 ignore command ที่หมดอายุ

#### C5 — ไม่มี safety max runtime / fail-safe

**ตำแหน่ง:** `esp32_water_valve.ino` บรรทัด 79–85 (`setValve`), 196–200 (`autoCheck`)

**อาการ:**

- ไม่มีเพดานเวลาเปิดวาล์ว
- ถ้า config ค้าง `open` หรือ schedule ผิด วาล์วเปิดค้าง indefinitely
- `digitalWrite(RELAY_PIN, on ? HIGH : LOW)` — ต้องยืนยันว่า relay module เป็น active-high หรือ active-low
- ไม่มี watchdog
- ตอน boot ตั้ง `LOW` (ถ้า relay active-low = เปิดวาล์วทันที!)

**ผลกระทบ:** น้ำท่วม/ค่าน้ำ/วาล์วเสียหาย

**แนวทางแก้:**

- `MAX_OPEN_MINUTES` (เช่น 30–60 นาที) แล้วปิด + รายงาน fault
- กำหนด `RELAY_ACTIVE_LEVEL` ชัดเจนและทดสอบกับฮาร์ดแวร์จริง
- `esp_task_wdt`
- เปิดวาล์วแล้วต้องมี heartbeat/lease ต่ออายุ

#### C6 — ตารางข้ามคืนผิด

**ตำแหน่ง:**

- Worker: `scheduleActiveNow` บรรทัด 50–69 (รองรับ overnight แต่เช็ค `days[dayOfWeek]` ของวันปัจจุบัน)
- Firmware: `scheduleActiveNow` บรรทัด 154–193 (เหมือนกัน)
- Web UI: `scheduleActiveNow` บรรทัด 1042–1047 (ไม่รองรับ overnight เลย)

**ตัวอย่าง:** ตาราง 22:00–06:00 เฉพาะวันจันทร์

- 22:00–23:59 จันทร์ → เปิดถูก
- 00:00–05:59 อังคาร → `days[อังคาร]` เป็น false → ปิดผิด

**แนวทางแก้:** ตรวจทั้ง window ของ "เมื่อวาน" และ "วันนี้" หรือ normalize เป็น interval ข้ามวัน

### 8.2 High

#### H1 — MQTT buffer 256 bytes เล็กเกินไป

**ตำแหน่ง:** `esp32_water_valve.ino` — ไม่มีการเรียก `mqtt.setBufferSize()`; PubSubClient default `MQTT_MAX_PACKET_SIZE = 256`

**อาการ:** payload `{"type":"schedule","schedules":[...]}` สำหรับ 3 ตารางมีขนาดราว 400–450 bytes → publish ไม่ถึง ESP32 (เงียบ ๆ)

**แนวทางแก้:** `mqtt.setBufferSize(1024)` (หรือ 2048) และทดสอบ publish จริง

#### H2 — Countdown/reset ไม่ปิดวาล์วผ่าน Cloudflare

**ตำแหน่ง:**

- `tickCountdown` บรรทัด 976–1002: หมดเวลาแล้วเรียกแค่ `sendCommandToEsp({type:'valve',action:'off'})`
- `resetDemoData` บรรทัด 1539–1557: ไม่ส่งคำสั่งปิดเลย

**ผลกระทบ:** ถ้า MQTT หลุด แต่ Worker ยังมี command เก่าเป็น `open` → วาล์วเปิดต่อ

**แนวทางแก้:** ทุกครั้งที่ปิดวาล์วต้องยิงทั้ง MQTT และ `POST /command` (หรือเปลี่ยนไปใช้ API เดียว)

#### H3 — `init()` ทับค่าด้วย default

**ตำแหน่ง:** `index.html` บรรทัด 1570–1588

**อาการ:** `State.schedules = defaultSchedules()` ก่อน `loadState()`; ถ้าไม่มี saved state หรือ saved state มี schedules ว่าง → `syncSchedulesToEsp()` + `syncToCloudflare()` เขียน default ทับ

**ผลกระทบ:** เครื่องใหม่เปิดแอปครั้งแรกทำลายค่าที่ตั้งไว้จากเครื่องอื่น

**แนวทางแก้:** GET ก่อน, ถ้าไม่มีข้อมูลค่อยใช้ default แบบ explicit; ห้าม POST ตอน init

#### H4 — State restoration ไม่ครบ / crash

**ตำแหน่ง:** `index.html` บรรทัด 832–887, 1121–1125

**อาการ:**

- `loadState()` ไม่ restore `valveOn` (ยกเว้น countdown active)
- `normalizeSchedule()` คืน `null` สำหรับ entry ที่ไม่ถูกต้อง แต่ `loadState()` ยังเก็บไว้ → `renderSchedules()` อ่าน `sch.id` แล้ว crash
- ถ้าผู้ใช้ลบ schedule ทั้งหมด (`schedules: []`) แล้ว reload → กลับไปใช้ default

**แนวทางแก้:** validate/filter null, restore `valveOn`, แยก "ไม่มีข้อมูล" กับ "มีข้อมูลแต่ array ว่าง"

#### H5 — Cron เขียน KV 1,440 ครั้ง/วัน และ state ไม่ใช่สถานะจริง

**ตำแหน่ง:** Worker `runCron` บรรทัด 74–90

**อาการ:**

- โหมด auto: ทุกนาที `put(STATE_KEY)` แม้ค่าไม่เปลี่ยน → 1,440 writes/day
- โหมด manual: return ทันที → `state` ค้างเก่า (ตามที่เห็นใน live: 2026-09-29)
- `state.shouldOpen` เป็น "ค่าที่ควรเป็น" ไม่ใช่สถานะวาล์วจริง

**แนวทางแก้:** เขียนเมื่อค่าเปลี่ยน, เก็บ state ใน D1, ใช้ status จากอุปกรณ์เป็นหลัก

#### H6 — Manual control ช้าและพึ่ง MQTT

**ตำแหน่ง:** Firmware `CONFIG_FETCH_MS_MANUAL = 120000` (2 นาที), MQTT เป็นช่องทางหลัก

**ผลกระทบ:** ถ้า MQTT หลุด ผู้ใช้กดเปิดวาล์วแล้วต้องรอถึง 2 นาที; ถ้า MQTT ต่อได้แต่ payload ใหญ่ (schedule) ก็ drop

**แนวทางแก้:** ใช้ MQTT + ack เป็นช่องทางหลัก (อย่าเพิ่ม HTTPS poll ถี่เพราะซิม 399 MB/เดือน); ถ้า MQTT หลุดให้ fallback HTTPS poll 60–120 วิ และแสดงสถานะ "คำสั่งอาจล่าช้า"

#### H7 — Single global config ไม่มี device identity

**ตำแหน่ง:** Worker ทั้งไฟล์; firmware ใช้ topic/client id คงที่

**ผลกระทบ:** รองรับหลายวาล์ว/หลายบ้านไม่ได้; อุปกรณ์ทุกตัวแชร์ config เดียวกัน; status แยกไม่ได้

**แนวทางแก้:** `households`, `devices`, `device_id`, device token; config per device หรือ per household ตาม requirement

#### H8 — JSON parsing ด้วย string extraction เปราะบาง

**ตำแหน่ง:** Firmware `extractField`, `parseDays`, `scheduleActiveNow` บรรทัด 98–193

**อาการ:**

- `extractField` หา `"key":` ตัวแรก อาจไปโดน key ซ้อน
- `scheduleCount()` นับ `{` ใน string (ชื่อมี `{` ก็ผิด)
- ไม่ validate schema/ชนิดข้อมูล
- ชื่อภาษาไทย/escape sequence อาจทำให้ parse ผิด

**แนวทางแก้:** ใช้ ArduinoJson (เวอร์ชัน 6/7) + schema validation

### 8.3 Medium

| # | ปัญหา | ตำแหน่ง/ผลกระทบ | แนวทาง |
|---|---|---|---|
| M1 | Timezone hardcode UTC+7 | Worker/firmware; ประเทศไทยไม่มี DST จึงใช้ได้ แต่ควรเก็บเป็น config | เก็บ `timezone` ใน household |
| M2 | Schedule edge cases | end exclusive, zero-length, overlap, ไม่มี minimum duration | validate ตอนบันทึก |
| M3 | Accessibility 72/100 | ปุ่ม mode ซ่อนปุ่ม, contrast, label | ปรับ UI/เพิ่ม contentDescription |
| M4 | Fire-and-forget ไม่มี retry/queue | `fetch(...).catch(()=>{})` หลายจุด | ใช้ queue + แสดงสถานะ send/success/fail |
| M5 | ไม่มี test/CI | ทั้ง repo | เพิ่ม unit test สำหรับ schedule engine + Worker |
| M6 | `usesCleartextTraffic=true`, `allowBackup=true` | Android manifest | ปิด cleartext, จำกัด backup |
| M7 | Notification ไม่ทำงานจริง | ขอ permission แต่ไม่มี implementation | FCM/local notification |
| M8 | KV eventual consistency | Worker ใช้ KV เป็น source of truth | ใช้ D1 สำหรับ config/commands |
| M9 | ไม่มี audit log | ใครเปลี่ยน mode/schedule ไม่รู้ | `audit_log` table |
| M10 | ไม่มี health/observability | debug ยาก | `/health`, structured logs, Sentry/Logpush |

---

## 9. สิ่งที่นำกลับมาใช้ได้

- **Schedule semantics** — โครง `{id,name,start,end,days,enabled}` ใช้ต่อได้
- **UI/UX patterns** — dashboard, mode toggle, schedule list, countdown (แต่ต้องทำ accessibility ใหม่)
- **Worker + KV** — ใช้เป็นจุดเริ่มต้นได้ แต่ควรย้าย config/commands ไป D1
- **Firmware** — 4G/NTP/relay/NVS/schedule loop ใช้ต่อได้; เปลี่ยน parser เป็น ArduinoJson และเพิ่ม safety
- **MQTT** — เก็บเป็นช่องทาง real-time หลักของ ESP32 ได้ เพราะประหยัด data ที่สุดบนซิม; แต่ app ต้องใช้ scoped credential ไม่ใช่ credential admin
- **Default schedules** — ใช้เป็นค่าเริ่มต้นเมื่อสร้าง household ใหม่

---

## 10. ข้อจำกัดและคำถามที่ต้องตัดสินใจ

### 10.1 ข้อตัดสินใจที่ยืนยันแล้ว (29 ต.ค. 2026)

| หัวข้อ | ข้อตัดสินใจ |
|---|---|
| จำนวนวาล์ว | 1 วาล์ว |
| การเชื่อมต่อ ESP32 | ซิม 4G/5G 399 MB/เดือน ไม่ใช้ WiFi |
| ลำดับงาน | Backend + native UI คู่ขนาน |
| Auth | Invite code + household password |

### 10.2 คำถามที่ยังเหลือ

1. **ฮาร์ดแวร์** — relay เป็น active-high หรือ active-low? มี sensor/flow switch หรือปุ่มฉุกเฉินไหม?
2. **Cloudflare plan/โดเมน** — ใช้ Workers/D1 ฟรี tier หรือ paid? มีโดเมนของตัวเองไหม?
3. **MQTT credential** — HiveMQ Cloud จำกัดสิทธิ์ topic ได้ไหม? ถ้าไม่ได้จะยอมรับความเสี่ยงหรือทำ Worker relay?
4. **Offline behavior** — แอปต้องแก้ค่า offline ได้ไหม หรือต้องออนไลน์เท่านั้น?
5. **ประวัติ/แจ้งเตือน** — ต้องมี audit log/push notification ตั้งแต่ phase แรกไหม?
6. **Web UI เดิม** — เก็บเป็น admin console หรือทิ้งไปเลย?
7. **OTA firmware** — ต้องมีตั้งแต่เวอร์ชันนี้ไหม?
8. **เกณฑ์สำเร็จ** — "เห็นค่าภายใน 15 วิ" พอไหม และรองรับกี่เครื่อง?

---

## 11. ข้อสรุป

1. **Central backend + native Android UI ทำได้** และเป็นทิศทางที่ถูกต้องสำหรับโจทย์ "ทุกเครื่องเห็นเหมือนกัน"
2. งานหลักไม่ใช่การเก็บค่าใน Cloudflare (มีอยู่แล้ว) แต่คือ:
   - ทำ read path + revision ให้ทุกเครื่อง sync
   - ทำ auth/device identity
   - ทำ command ที่มี TTL/ack
   - แทน WebView ด้วย native UI
   - แก้ safety ของ firmware
3. ควรทำ backend v2 (D1 + auth + revision) **ก่อน** หรือ **ขนานกับ** native UI เพื่อไม่ต้องเขียน UI สองรอบ
4. ห้ามเปิดใช้ระบบนี้กับวาล์วน้ำจริงจนกว่า C1–C6 จะถูกแก้
5. ขั้นตอนถัดไปและ API spec อยู่ใน [`ARCHITECTURE_CENTRAL_CONFIG.md`](./ARCHITECTURE_CENTRAL_CONFIG.md)
