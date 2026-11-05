# Water Valve Scheduler v2

> ภาษา: **ไทย** | [English](README.en.md)

ระบบ config กลาง + แอป Android native + ตัวควบคุม ESP32 แบบ cellular สำหรับวาล์วน้ำ 1 ตัว

```
Android (Compose) ──HTTPS──> Cloudflare Worker v2 ──D1 (config, commands, audit)
                                   ▲
                                   │ HTTPS sync ทุก 15 นาที
                                   │
ESP32 (A7672X) ──MQTT/TLS 8883──> HiveMQ Cloud
```

- **Worker v2:** `https://water-valve-api-v2.czqwertyyy.workers.dev`
- **D1:** `water-valve-v2` (APAC)
- **Android:** native Compose ไม่มี WebView
- **Firmware:** ESP32 + A7672X, MQTT real-time + HTTPS config sync

## ดาวน์โหลด Android APK

- **GitHub Release:** <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>
- **ดาวน์โหลดตรง (v2.1):**
  <https://github.com/Rawpasterbin/esp32/releases/download/v2.1-native/water-valve-2.1-native-release.apk>
- **ไฟล์ล่าสุดใน repo:** [water-valve-2.1-native-release.apk](./releases/water-valve-2.1-native-release.apk)
  - SHA-256: `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
- **เวอร์ชันก่อนหน้า:** [water-valve-2.0-native-release.apk](./releases/water-valve-2.0-native-release.apk)
  - SHA-256: `D8F82D055F0A0AB1EE63334133BF2D8154D1C430730B888399AF419EC89AEEAC`
- v2.1 ใช้ signing key เดียวกับ v2.0 จึงติดตั้งทับ v2.0 ได้เลย
  ถ้ามี debug-signed `com.example.esp32` อยู่ ต้องถอนก่อน (คนละ signing key)
  แล้วติดตั้งใหม่; login ด้วย invite code และรหัสบ้านปัจจุบัน ข้อมูลฝั่ง server ไม่เปลี่ยน

## โครงสร้าง repo

| Path | เนื้อหา |
|---|---|
| `worker/v2/index.js` | Worker API: auth, state, config revision, commands, device sync/ack |
| `migrations/0001_init.sql` | D1 schema |
| `migrations/0002_auth_attempts.sql` | ตาราง login/rate-limit |
| `migrations/0003_rate_limits.sql` | ตาราง rate-limit คำสั่งต่อ member |
| `wrangler.v2.toml` | config ของ Worker/D1 |
| `scripts/test-api.ps1` | API smoke tests (local หรือ production) |
| `esp32_water_valve/` | firmware + README เรื่อง wiring/provisioning |
| `docs/` | วิเคราะห์, architecture, migration notes |
| Android project | `C:\Users\ssde\AndroidStudioProjects\ESP32` (git repo แยก) |

## เริ่มต้นแบบ local (API)

```powershell
# Terminal 1
npx wrangler dev --config wrangler.v2.toml --local --port 8787

# Terminal 2 (ครั้งแรกครั้งเดียว)
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0003_rate_limits.sql
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

`.dev.vars` ต้องมี `SETUP_TOKEN=...` (git-ignored)

## Deploy production

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0003_rate_limits.sql
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
npx wrangler deploy --config wrangler.v2.toml
```

รัน API suite ทั้งชุดกับ production:

```powershell
$env:API_BASE = 'https://water-valve-api-v2.czqwertyyy.workers.dev'
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

## สร้าง household จริง

```powershell
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
powershell -ExecutionPolicy Bypass -File scripts/create-household.ps1 `
  -HouseholdName 'บ้านของเรา'
```

- จะ prompt ให้กรอกรหัสบ้าน (อย่างน้อย 8 ตัวอักษร); รหัสจะไม่ถูกเขียนลงดิสก์
- บันทึก `deviceToken` (ใช้ได้ครั้งเดียว) และ IDs ลง
  `C:\Temp\opencode\water-valve-household.json` พร้อม ACL แบบ user-only
- พิมพ์คำสั่ง provisioning `set token ...` / `set url ...`
- ใช้ `-Force` เฉพาะเมื่อต้องการแทนไฟล์ที่บันทึกไว้จริง ๆ
- ถือว่าไฟล์ token เป็น secret; ห้าม commit

ก่อน provisioning firmware ให้อ่าน `docs/SECURITY.md`: ต้อง rotate HiveMQ credential เก่าก่อน

## Android

```powershell
cd C:\Users\ssde\AndroidStudioProjects\ESP32
.\gradlew.bat assembleDebug :app:testDebugUnitTest --no-daemon
```

- debug/release ใช้ production API เป็นค่าเริ่มต้น
- override เป็น local: `.\gradlew.bat assembleDebug -PapiBaseUrl=http://10.0.2.2:8787/`
- session token เข้ารหัสด้วย Android Keystore (AES-GCM)
- การสั่งวาล์ว manual จะ queue command ไว้ใน D1; อุปกรณ์จะได้รับในรอบ HTTPS sync
  ถัดไป (ไม่เกิน ~15 นาที, TTL 20 นาที) — แอปยังไม่ publish MQTT โดยตรง
  ดังนั้นคำสั่งจากแอปจะไม่ถึงอุปกรณ์ทันทีแม้บอร์ดต่อ MQTT อยู่
- ไม่มี MQTT credential ใน APK; real-time control จากแอปต้องใช้ scoped credential
  ที่ Worker ออกให้ หรือใช้ relay ผ่าน Worker

## Firmware

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "$env:USERPROFILE\Documents\Arduino\libraries" esp32_water_valve
```

ดู `esp32_water_valve/README.md` สำหรับ wiring, provisioning, MQTT topics,
ข้อจำกัดเรื่อง TLS session เดียว, safety และ data budget

## สถานะ

ทำเสร็จและตรวจแล้ว:

- Worker v2 + D1 deploy แล้ว; API checks ผ่าน **95/95** บน production
  (cross-household isolation, device-token scope, logout revocation,
  password/device-token rotation, login/command rate limiting)
- สร้าง household `Home` + device จริงแล้ว (credentials อยู่นอก repo);
  device sync ผ่าน production
- **Android v2.1-native release APK** เซ็นแล้วและติดตั้งบนเครื่องจริง
  (Infinix X6815B, Android 12); runtime smoke test ผ่าน: login, แก้ mode/schedule,
  open/close (จำลอง device sync/ack), conflict dialog, offline cache ข้าม process,
  เปลี่ยนรหัสบ้าน, logout/relogin
- **v1 scope: 1 household/valve ต่อ MQTT deployment** — MQTT topics ยังเป็น global;
  ดู `docs/SECURITY.md`
- Config revision + `If-Match` conflict handling
- Command queue มี idempotency, TTL 20 นาที, latest-wins supersede
- Android มี login, dashboard, mode/schedule editor, command queue, Keystore token
- **ESP32 board connected** (COM15, WiFi bench build v2.1.0): WiFi + HTTPS sync
  `HTTP 200`, revision 17, device online ใน D1; คำสั่ง `close` จริงถูกส่งตอน sync
  และ ack `200`
- **MQTT connected** กับ HiveMQ Cloud ด้วย device user ใหม่: `CONNACK 0`;
  `getStatus` และ MQTT `close` ผ่านการทดสอบ — user เก่า `test` ที่รั่วยังต้อง
  disable/delete (ดู `docs/MQTT_SETUP.md`)
- Firmware compile ผ่านทั้ง cellular และ WiFi bench; MQTT + HTTPS sync ทำงาน

ยังต้องตรวจบนฮาร์ดแวร์จริง:

- Relay active level และ wiring ของวาล์ว (`RELAY_ACTIVE_HIGH`)
- A7672X CA upload/TLS handshake บนเครือข่าย carrier (ตอนนี้เป็น WiFi bench)
- Disable/delete HiveMQ user เก่า `test` ที่ credential รั่ว
- Power stability ตอน cellular transmit burst
- เปิด/ปิดวาล์วจริง (ทดสอบแล้วเฉพาะ close แบบ no-op)
- Real-time Android MQTT control (optional phase)

ดู `docs/ARCHITECTURE_CENTRAL_CONFIG.md` §17–18,
`docs/HARDWARE_TEST_CHECKLIST.md`, **`docs/SECURITY.md`**
(ต้อง rotate MQTT credential เก่าก่อน deploy ฮาร์ดแวร์) และ
`docs/RUNBOOK.md` สำหรับขั้นตอน deploy/backup/rotation

## หมายเหตุเรื่องคำศัพท์

คำเทคนิคต่อไปนี้คงเป็นภาษาอังกฤษ เพราะแปลไทยแล้วอ่านเพี้ยนหรือเป็นชื่อเฉพาะ:
Worker, D1, API, MQTT, TLS, HTTPS, ESP32, A7672X, HiveMQ, Cloudflare,
Android, Compose, WebView, APK, GitHub, token, firmware, provisioning,
sync, deploy, build, release, commit, repo, branch, tag, revision, ETag,
If-Match, JSON, DTO, Retrofit, DataStore, Keystore, NVS, UART, GPIO, relay,
watchdog, maxopen, schedule, mode, manual, auto, command queue, TTL,
idempotency, ACL, CA, NTP, RSSI
