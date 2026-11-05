# Water Valve Scheduler v2

> ภาษา: **ไทย** | [English](README.en.md)

ระบบควบคุมวาล์วน้ำหนึ่งตัว ประกอบด้วย API กลางบน Cloudflare Workers, แอปพลิเคชัน Android แบบ native และเฟิร์มแวร์ ESP32 ที่เชื่อมต่อผ่านเครือข่ายมือถือ

```
Android (Compose) ──HTTPS──> Cloudflare Worker v2 ──D1 (config, commands, audit)
                                   ▲
                                   │ HTTPS sync ทุก 15 นาที
                                   │
ESP32 (A7672X) ──MQTT/TLS 8883──> HiveMQ Cloud
```

## องค์ประกอบ

| องค์ประกอบ | รายละเอียด |
|---|---|
| Worker API | Cloudflare Workers + D1 เก็บ config, command queue และ audit log |
| แอปพลิเคชัน | Android native ด้วย Jetpack Compose ไม่ใช้ WebView |
| เฟิร์มแวร์ | ESP32 + A7672X ใช้ MQTT สำหรับ command/status และ HTTPS sync ทุก 15 นาที |
| MQTT | HiveMQ Cloud over TLS 8883 |

Worker เป็นเจ้าของสถานะทั้งหมด ได้แก่ mode, ตารางเวลา, คำสั่ง และประวัติการใช้งาน แอปพลิเคชันอ่านและเขียนผ่าน HTTPS ส่วนอุปกรณ์ดึงค่าที่ต้องการทุก 15 นาทีและยืนยันผลกลับมา

## คุณสมบัติ

- Login ด้วย invite code และรหัสบ้าน แยก session ของสมาชิกออกจาก device token
- แก้ mode และตารางเวลาได้พร้อมกันหลายเครื่อง โดยใช้ `revision` และ `If-Match` เพื่อป้องกันการเขียนทับ
- คำสั่งเปิด/ปิดมี `commandId`, TTL 20 นาที, idempotency และการแทนที่คำสั่งเก่าที่ค้างอยู่
- รองรับ schedule ข้ามคืน และบังคับปิดวาล์วเมื่อเปิดนานเกิน `maxopen`
- แอปพลิเคชันเก็บ session ใน Android Keystore และมี cache สำหรับใช้งานแบบออฟไลน์
- ไม่มี MQTT credential ใน APK คำสั่งทั้งหมดผ่าน Worker

## โครงสร้าง repository

| Path | เนื้อหา |
|---|---|
| `worker/v2/index.js` | Worker API: auth, state, config revision, commands, device sync/ack |
| `migrations/0001_init.sql` | schema หลัก: households, members, sessions, devices, configs, commands, audit_log |
| `migrations/0002_auth_attempts.sql` | ตารางนับความพยายามเข้าสู่ระบบ |
| `migrations/0003_rate_limits.sql` | ตารางจำกัดจำนวนคำสั่งต่อสมาชิก |
| `wrangler.v2.toml.example` | เทมเพลต Worker/D1; copy เป็น `wrangler.v2.toml` แล้วใส่ `database_id` |
| `scripts/test-api.ps1` | ชุดทดสอบ API สำหรับ local และ production |
| `scripts/create-household.ps1` | สร้าง household และ device token พร้อมจำกัดสิทธิ์ไฟล์ |
| `esp32_water_valve/` | เฟิร์มแวร์และเอกสาร wiring/provisioning |
| `releases/` | APK ที่ลงนามแล้วและ checksum |

แอปพลิเคชัน Android อยู่ใน repository แยกต่างหาก (Jetpack Compose, ไม่มี WebView)

## เริ่มต้นในเครื่อง

สร้าง config จากเทมเพลตและกำหนด `database_id` ของ D1:

```powershell
Copy-Item wrangler.v2.toml.example wrangler.v2.toml
# แก้ database_id ใน wrangler.v2.toml (ไฟล์นี้ถูก gitignore)
```

```powershell
# Terminal 1
npx wrangler dev --config wrangler.v2.toml --local --port 8787

# Terminal 2 (ครั้งแรก)
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0003_rate_limits.sql

powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

`.dev.vars` ต้องมี `SETUP_TOKEN` และถูกยกเว้นจาก git สคริปต์ทดสอบจะอ่านจาก environment หรือไฟล์นี้

## Deploy

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0003_rate_limits.sql

$env:SETUP_TOKEN = '<setup-token>'
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
npx wrangler deploy --config wrangler.v2.toml
```

Migration ใช้ `CREATE TABLE IF NOT EXISTS` จึงรันซ้ำได้ แต่การเปลี่ยนโครงสร้างต้องเพิ่ม migration ไฟล์ใหม่

ทดสอบกับ Worker ที่ deploy แล้ว:

```powershell
$env:API_BASE = 'https://<your-worker-url>'
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

ชุดทดสอบนี้สร้าง household จริงเมื่อรันกับ production และไม่ลบข้อมูลให้อัตโนมัติ ควรใช้กับ local D1 เป็นหลัก หรือลบข้อมูลทดสอบออกจาก D1 ด้วยตนเองหลังทดสอบ

## สร้าง household

```powershell
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/create-household.ps1 `
  -ApiBase 'https://<your-worker-url>' `
  -HouseholdName '<household-name>'
```

- สคริปต์จะขอรหัสบ้านผ่าน prompt (อย่างน้อย 8 ตัวอักษร) และไม่เขียนรหัสลงดิสก์
- ผลลัพธ์มี `deviceToken` ที่แสดงครั้งเดียว ระบบเขียนไฟล์ JSON นอก repository ตาม `-OutFile` พร้อมจำกัดสิทธิ์ให้ผู้ใช้ปัจจุบัน
- สคริปต์พิมพ์คำสั่ง `set token ...` และ `set url ...` สำหรับ provisioning
- ไฟล์ผลลัพธ์เป็นความลับ ห้าม commit

## Android

APK ล่าสุด:

- [water-valve-2.1-native-release.apk](./releases/water-valve-2.1-native-release.apk)
- SHA-256: `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
- GitHub Release: <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>

```powershell
adb install -r water-valve-2.1-native-release.apk
```

ถ้ามี build ที่ลงนามด้วย debug key อยู่ ต้องถอนก่อน:

```powershell
adb uninstall com.example.esp32
adb install water-valve-2.1-native-release.apk
```

Build จาก source (repository แยก):

```powershell
cd <android-project>
.\gradlew.bat assembleDebug :app:testDebugUnitTest --no-daemon
```

- debug/release ใช้ API ที่กำหนดใน build config
- override เป็น local สำหรับ emulator: `.\gradlew.bat assembleDebug -PapiBaseUrl=http://10.0.2.2:8787/`
- session token เข้ารหัสด้วย Android Keystore; server เก็บเฉพาะ hash
- APK ไม่มี MQTT credential

## Firmware

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" esp32_water_valve
```

รายละเอียด wiring, provisioning, MQTT topics, ข้อจำกัด TLS session เดียวของ A7672X, data budget และ safety อยู่ใน `esp32_water_valve/README.md`

## API

ทุก endpoint ใต้ `/v1/households/...` ต้องส่ง `Authorization: Bearer <session token>` และ session ต้องเป็นสมาชิกของ household นั้น ไม่เช่นนั้นได้ 403

| Method | Path | Auth | หน้าที่ |
|---|---|---|---|
| GET | `/health` | – | ตรวจสอบเวอร์ชันและเวลาเซิร์ฟเวอร์ |
| POST | `/v1/setup` | `X-Setup-Token` | สร้าง household และ device token (plaintext คืนครั้งเดียว) |
| POST | `/v1/auth/join` | – | เข้าร่วม household ด้วย invite code, รหัสบ้าน และ displayName |
| POST | `/v1/auth/login` | – | ขอ session token (อายุ 30 วัน) |
| POST | `/v1/auth/logout` | session | ยกเลิก session ปัจจุบัน |
| GET | `/v1/me` | session | ข้อมูลสมาชิกและ household |
| GET | `/v1/households/:hid/state` | session | config ล่าสุดและสถานะอุปกรณ์ |
| PUT | `/v1/households/:hid/config` | session | เขียน mode/schedules; ต้องส่ง `If-Match` หรือ `baseRevision` |
| POST | `/v1/households/:hid/password` | session | เปลี่ยนรหัสบ้านและยกเลิก session อื่น |
| POST | `/v1/households/:hid/devices/:did/rotate-token` | session | ออก device token ใหม่ |
| POST | `/v1/households/:hid/commands` | session | เพิ่มคำสั่งเปิด/ปิด (รองรับ `idempotencyKey`) |
| GET | `/v1/households/:hid/commands` | session | ดูคำสั่งล่าสุดและปรับสถานะที่หมดอายุ |
| POST | `/v1/devices/sync` | device token | sync config, รับคำสั่ง และส่งสถานะ |
| POST | `/v1/devices/status` | device token | อัปเดตสถานะอย่างเดียว |
| GET | `/v1/devices/desired` | device token | ดึง config ที่ต้องการ |
| POST | `/v1/devices/commands/:id/ack` | device token | ยืนยันผลคำสั่ง |

Error codes: `BAD_REQUEST` 400, `UNAUTHORIZED` 401, `FORBIDDEN` 403, `NOT_FOUND` 404, `CONFLICT` 409, `VALIDATION` 422, `PRECONDITION_REQUIRED` 428, `RATE_LIMITED` 429, `INTERNAL` 500

## ค่าคงที่และขีดจำกัด

| รายการ | ค่า | ตำแหน่ง |
|---|---|---|
| รอบ sync ของอุปกรณ์ | 15 นาที (retry 60 วินาทีเมื่อ fail) | `SYNC_INTERVAL_MS` |
| เกณฑ์ online | last seen ภายใน 20 นาที | `DEVICE_ONLINE_WINDOW_MS` |
| อายุคำสั่ง | 20 นาที | `COMMAND_TTL_SECONDS` |
| จำนวน schedule สูงสุด | 8 | `validateConfig` |
| อายุ session | 30 วัน | `SESSION_DAYS` |
| Login ผิด | 5 ครั้ง/นาที/IP | `auth_attempts` |
| คำสั่ง | 10 ครั้ง/นาที/สมาชิก | `rate_limits` |
| เปิดวาล์วสูงสุด | 30 นาที (ปรับ 1–240) | `maxopen` |
| Data โดยประมาณ | 55–105 MB/เดือน | firmware |

## การวินิจฉัยปัญหา

| อาการ | สาเหตุ | แนวทาง |
|---|---|---|
| อุปกรณ์ offline | sync ล้มเหลวหรือ last seen เกิน 20 นาที | ดู log `Sync HTTP ...` |
| `Sync HTTP 401` | device token ผิดหรือถูก rotate | ออก token ใหม่แล้ว `set token` |
| `Sync HTTP 0` | DNS/TLS/CA/APN | ตรวจ `AT+CCERTLIST`, APN และ CA |
| คำสั่งค้าง pending | อุปกรณ์ offline นานกว่า TTL | สั่งใหม่เมื่อกลับมาออนไลน์ |
| 429 ตอน login | ผิด 5 ครั้งใน 1 นาที | รอ 1 นาที |
| 429 ตอนสั่งงาน | เกิน 10 คำสั่ง/นาที/สมาชิก | รอ 1 นาที |
| 409 ตอนบันทึก config | revision เปลี่ยนเพราะมีคนอื่นแก้ | โหลดใหม่แล้วบันทึกอีกครั้ง |
| 428 ตอนบันทึก config | ไม่ได้ส่ง `If-Match` | ส่ง revision ล่าสุด |
| Worker 500 | D1 binding/migration/body ผิด | `npx wrangler tail --config wrangler.v2.toml` |
| วาล์วปิดเอง | ถึง `maxopen`, schedule จบ หรือมี `close` | ตรวจ `audit_log` |

## ก่อนใช้งานกับวาล์วจริง

- ตรวจระดับ active ของ relay (`RELAY_ACTIVE_HIGH`) กับบอร์ดจริงก่อนต่อวาล์ว
- ใช้ MQTT credential เฉพาะอุปกรณ์นี้ ไม่ใช้ร่วมกับระบบอื่น
- MQTT topic ยังเป็น global (`myhouse/valve/...`) จึงรองรับหนึ่ง household ต่อหนึ่ง MQTT deployment
- วาล์วถูกบังคับปิดตอน boot และปิดอัตโนมัติเมื่อครบ `maxopen`
- คำสั่งที่มี TTL จะถูกปฏิเสธถ้านาฬิกาอุปกรณ์ยังไม่ sync
- ควรมี breaker หรือ cutoff แยกสำหรับวาล์ว

## ความปลอดภัย

- Worker/D1 เก็บเฉพาะ hash ของ session และ device token; plaintext แสดงครั้งเดียวตอนสร้างหรือ rotate
- APK ไม่มี MQTT credential; คำสั่งทั้งหมดผ่าน Worker
- `.dev.vars`, `wrangler.v2.toml`, keystore และไฟล์ token ถูก gitignore; ห้าม commit ค่าเหล่านี้
- MQTT user ของอุปกรณ์ควรจำกัด ACL เฉพาะ `myhouse/valve/control`, `status`, `mode` และ `schedule`
- เมื่อ rotate device token หรือ MQTT credential ต้องตั้งค่าอุปกรณ์ใหม่ผ่าน `set token` / `set mqttuser` / `set mqttpass`
- รองรับหนึ่ง household ต่อหนึ่ง MQTT broker เพราะ topic ยังเป็น global

## หมายเหตุด้านภาษา

คำเทคนิค เช่น Worker, D1, API, MQTT, TLS, HTTPS, ESP32, A7672X, HiveMQ, Cloudflare, Compose, APK, token, firmware, provisioning, sync, deploy, build, release, repository, revision, ETag, If-Match, JSON, Keystore, NVS, GPIO, relay, watchdog, maxopen, schedule, TTL, idempotency, ACL, CA และ NTP คงเป็นภาษาอังกฤษเพราะเป็นชื่อเฉพาะหรือแปลแล้วความหมายคลาดเคลื่อน
