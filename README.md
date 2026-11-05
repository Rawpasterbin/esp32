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

## ภาพรวมระบบ

| องค์ประกอบ | รายละเอียด |
|---|---|
| Worker API | `https://water-valve-api-v2.czqwertyyy.workers.dev` |
| ฐานข้อมูล | Cloudflare D1 `water-valve-v2` (ภูมิภาค APAC) |
| แอปพลิเคชัน | Android native ด้วย Jetpack Compose ไม่ใช้ WebView |
| เฟิร์มแวร์ | ESP32 + A7672X รองรับ MQTT แบบ real-time และ HTTPS config sync |

Worker เป็นเจ้าของสถานะทั้งหมด ได้แก่ ค่า mode และตารางเวลา (config), คิวคำสั่ง (commands) และบันทึกการใช้งาน (audit) แอปพลิเคชันอ่านและเขียนผ่าน HTTPS ส่วนอุปกรณ์ซิงก์ค่าที่ต้องการทุก 15 นาทีและยืนยันผลการทำงานกลับมา

## การติดตั้งแอปพลิเคชัน Android

- GitHub Release: <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>
- ดาวน์โหลดโดยตรง (v2.1): <https://github.com/Rawpasterbin/esp32/releases/download/v2.1-native/water-valve-2.1-native-release.apk>
- ไฟล์ใน repository: [water-valve-2.1-native-release.apk](./releases/water-valve-2.1-native-release.apk)
  SHA-256 `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
- เวอร์ชันก่อนหน้า: [water-valve-2.0-native-release.apk](./releases/water-valve-2.0-native-release.apk)
  SHA-256 `D8F82D055F0A0AB1EE63334133BF2D8154D1C430730B888399AF419EC89AEEAC`

เวอร์ชัน 2.1 ลงนามด้วยคีย์เดียวกับ 2.0 จึงติดตั้งทับได้โดยตรง กรณีที่เครื่องมีแพ็กเกจ `com.example.esp32` ที่ลงนามด้วย debug key อยู่ จำเป็นต้องถอนออกก่อนเนื่องจากใช้คีย์คนละชุด จากนั้นเข้าสู่ระบบด้วย invite code และรหัสบ้าน ข้อมูลฝั่งเซิร์ฟเวอร์ไม่เปลี่ยนแปลง

## ลำดับการทำงานของคำสั่ง

1. ผู้ใช้สั่งเปิดหรือปิดจากแอปพลิเคชัน แล้ว Worker บันทึกคำสั่งลงตาราง `commands` ด้วยสถานะ `pending` และกำหนดอายุ 20 นาที
2. อุปกรณ์ซิงก์ตามรอบ 15 นาที รับคำสั่งไปดำเนินการ แล้วส่งผลกลับมาเป็นสถานะ `acked`
3. หากคำสั่งหมดอายุก่อนถึงอุปกรณ์ ระบบจะเปลี่ยนสถานะเป็น `expired` พร้อมค่า `error='ttl'` และต้องสั่งใหม่
4. หากมีคำสั่งใหม่ระหว่างที่คำสั่งเดิมยัง pending คำสั่งใหม่จะแทนที่คำสั่งเดิมด้วยค่า `error='superseded'` เพื่อป้องกันกรณีคำสั่งเปิดที่ค้างอยู่ถูกส่งภายหลังคำสั่งปิด

ข้อจำกัดที่ต้องทำความเข้าใจ: แอปพลิเคชันไม่ได้เผยแพร่ MQTT ด้วยตนเอง คำสั่งจึงไม่ถึงอุปกรณ์ทันทีแม้อุปกรณ์จะเชื่อมต่อ MQTT อยู่ก็ตาม ช่องทาง MQTT ใช้สำหรับรับคำสั่งจากแหล่งอื่นและรองรับการควบคุมแบบ real-time ในระยะถัดไป การควบคุมให้มีผลทันทีจำเป็นต้องเพิ่มกลไก relay ผ่าน Worker หรือออก scoped credential จาก Worker ซึ่งยังไม่ได้ดำเนินการ

## โครงสร้าง repository

| Path | เนื้อหา |
|---|---|
| `worker/v2/index.js` | Worker API: การยืนยันตัวตน, state, config revision, commands, device sync/ack |
| `migrations/0001_init.sql` | schema หลัก ได้แก่ households, members, sessions, devices, configs, commands, audit_log |
| `migrations/0002_auth_attempts.sql` | ตารางนับความพยายามเข้าสู่ระบบ |
| `migrations/0003_rate_limits.sql` | ตารางจำกัดจำนวนคำสั่งต่อสมาชิก |
| `wrangler.v2.toml.example` | เทมเพลตการตั้งค่า Worker และ D1; copy เป็น `wrangler.v2.toml` (local, git-ignored) แล้วใส่ `database_id` ของตัวเอง |
| `scripts/test-api.ps1` | ชุดทดสอบ API สำหรับ local และ production |
| `scripts/create-household.ps1` | สร้าง household และ device token พร้อมกำหนดสิทธิ์ไฟล์ |
| `esp32_water_valve/` | เฟิร์มแวร์และเอกสารการต่อวงจร/การตั้งค่า |
| `docs/` | เอกสารวิเคราะห์ สถาปัตยกรรม runbook และ security |
| `releases/` | ไฟล์ APK ที่ลงนามแล้วและ checksum |

แอปพลิเคชัน Android อยู่ใน repository แยกต่างหาก (Jetpack Compose, ไม่มี WebView)

## การพัฒนาและทดสอบในเครื่อง

ก่อนเริ่มครั้งแรก ให้สร้างไฟล์ config จากเทมเพลตแล้วใส่ `database_id` ของ D1:

```powershell
Copy-Item wrangler.v2.toml.example wrangler.v2.toml
# แก้ database_id ใน wrangler.v2.toml (ไฟล์นี้ถูก gitignore)
```

```powershell
# เทอร์มินัลที่ 1
npx wrangler dev --config wrangler.v2.toml --local --port 8787

# เทอร์มินัลที่ 2 (เฉพาะครั้งแรก)
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0003_rate_limits.sql

powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

ไฟล์ `.dev.vars` ต้องมีค่า `SETUP_TOKEN` และถูกยกเว้นจาก git แล้ว หากไม่ได้กำหนดตัวแปรสภาพแวดล้อม `SETUP_TOKEN` สคริปต์ทดสอบจะอ่านค่าจากไฟล์นี้

## การติดตั้งขึ้น production

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0003_rate_limits.sql

$env:SETUP_TOKEN = '<setup-token>'
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
npx wrangler deploy --config wrangler.v2.toml
```

ไฟล์ migration ทุกไฟล์ใช้ `CREATE TABLE IF NOT EXISTS` จึงรันซ้ำได้โดยไม่เกิดข้อผิดพลาด แต่ตารางและ index ที่มีอยู่แล้วจะไม่ถูกปรับให้ตรงกับ schema ใหม่ หากต้องการเปลี่ยนโครงสร้างต้องเพิ่ม migration ไฟล์ใหม่

การรันชุดทดสอบกับ production:

```powershell
$env:API_BASE = 'https://water-valve-api-v2.czqwertyyy.workers.dev'
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

ชุดทดสอบนี้สร้าง household จริงสองรายการเพื่อทดสอบการแยกข้อมูลระหว่าง household เมื่อรันกับ production จะมีข้อมูลทดสอบคงค้างใน D1 และต้องลบด้วยตนเองตามขั้นตอนใน `docs/RUNBOOK.md` หัวข้อ 9

## การสร้าง household

```powershell
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/create-household.ps1 `
  -HouseholdName 'บ้านของเรา'
```

- สคริปต์จะขอรหัสบ้านผ่าน prompt (อย่างน้อย 8 ตัวอักษร) และไม่เขียนรหัสลงดิสก์
- ผลลัพธ์ประกอบด้วย `deviceToken` ที่แสดงเพียงครั้งเดียวและรหัสต่าง ๆ ระบบเขียนไฟล์ JSON ออกนอก repository โดยกำหนดตำแหน่งได้ด้วยพารามิเตอร์ `-OutFile` พร้อมจำกัดสิทธิ์ให้เฉพาะผู้ใช้ปัจจุบัน
- สคริปต์พิมพ์คำสั่งสำหรับตั้งค่าอุปกรณ์ ได้แก่ `set token ...` และ `set url ...`
- ใช้ `-Force` เฉพาะกรณีที่ต้องการเขียนทับไฟล์เดิม
- ไฟล์ผลลัพธ์ถือเป็นความลับ ห้าม commit ขึ้น repository

ก่อนตั้งค่าเฟิร์มแวร์ ควรอ่าน `docs/SECURITY.md` เนื่องจาก credential ของ HiveMQ ชุดเดิมรั่วไหลและต้องหมุนใหม่ที่คอนโซลก่อน

## การ build แอปพลิเคชัน Android

```powershell
cd <android-project>
.\gradlew.bat assembleDebug :app:testDebugUnitTest --no-daemon
```

- debug และ release ใช้ production API เป็นค่าเริ่มต้น
- กำหนด API ปลายทางเป็นเครื่อง local สำหรับ emulator: `.\gradlew.bat assembleDebug -PapiBaseUrl=http://10.0.2.2:8787/`
- session token เข้ารหัสด้วย Android Keystore (AES-GCM) ฝั่งเซิร์ฟเวอร์เก็บเฉพาะค่า hash
- ภายใน APK ไม่มี MQTT credential

## เฟิร์มแวร์

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" esp32_water_valve
```

รายละเอียดการต่อวงจร การตั้งค่าครั้งแรก MQTT topics ข้อจำกัดเรื่อง TLS session เดียวของ A7672X การใช้ข้อมูล และข้อกำหนดด้านความปลอดภัย อยู่ใน `esp32_water_valve/README.md`

ค่าที่กำหนดผ่าน serial ได้แก่ `set maxopen <1..240>` (หน่วยนาที ค่าเริ่มต้น 30), `set mqttuser`, `set mqttpass`, `set token`, `set url` และสั่ง `reboot` เมื่อตั้งค่าเสร็จ

## API

ทุก endpoint ภายใต้ `/v1/households/...` ต้องส่ง `Authorization: Bearer <session token>` และ session ต้องเป็นสมาชิกของ household นั้น มิฉะนั้นจะได้รับสถานะ 403

| Method | Path | การยืนยันตัวตน | หน้าที่ |
|---|---|---|---|
| GET | `/health` | – | ตรวจสอบเวอร์ชันและเวลาของเซิร์ฟเวอร์ |
| POST | `/v1/setup` | `X-Setup-Token` | สร้าง household และ device token (คืนค่า plaintext ครั้งเดียว) |
| POST | `/v1/auth/join` | – | เข้าร่วม household ด้วย invite code รหัสบ้าน และ displayName |
| POST | `/v1/auth/login` | – | ขอ session token (อายุ 30 วัน) |
| POST | `/v1/auth/logout` | session | ยกเลิก session ปัจจุบัน |
| GET | `/v1/me` | session | ข้อมูลสมาชิกและ household |
| GET | `/v1/households/:hid/state` | session | config ล่าสุดและสถานะอุปกรณ์ |
| PUT | `/v1/households/:hid/config` | session | เขียน mode และ schedules ต้องส่ง `If-Match` หรือ `baseRevision` |
| POST | `/v1/households/:hid/password` | session | เปลี่ยนรหัสบ้านและยกเลิก session อื่นทั้งหมด |
| POST | `/v1/households/:hid/devices/:did/rotate-token` | session | ออก device token ใหม่ |
| POST | `/v1/households/:hid/commands` | session | เพิ่มคำสั่งเปิดหรือปิด (รองรับ `idempotencyKey`) |
| GET | `/v1/households/:hid/commands` | session | ดูคำสั่งล่าสุดและปรับสถานะที่หมดอายุ |
| POST | `/v1/devices/sync` | device token | ซิงก์ config รับคำสั่ง และส่งสถานะ |
| POST | `/v1/devices/status` | device token | อัปเดตสถานะอย่างเดียว |
| GET | `/v1/devices/desired` | device token | ดึง config ที่ต้องการ |
| POST | `/v1/devices/commands/:id/ack` | device token | ยืนยันผลการดำเนินการของคำสั่ง |

รหัสข้อผิดพลาดที่ Worker ส่งกลับ: `BAD_REQUEST` 400, `UNAUTHORIZED` 401, `FORBIDDEN` 403, `NOT_FOUND` 404, `CONFLICT` 409 (revision ไม่ตรง), `VALIDATION` 422, `PRECONDITION_REQUIRED` 428 (ไม่ได้ส่ง `If-Match`), `RATE_LIMITED` 429, `INTERNAL` 500

## ค่าคงที่และขีดจำกัดของระบบ

| รายการ | ค่า | ตำแหน่งที่กำหนด |
|---|---|---|
| รอบซิงก์ของอุปกรณ์ | 15 นาที (ลองใหม่หลัง 60 วินาทีเมื่อล้มเหลว) | เฟิร์มแวร์ `SYNC_INTERVAL_MS` |
| เกณฑ์ถือว่าอุปกรณ์ออนไลน์ | พบครั้งล่าสุดภายใน 20 นาที | Worker `DEVICE_ONLINE_WINDOW_MS` |
| อายุของคำสั่ง | 20 นาที | Worker `COMMAND_TTL_SECONDS` |
| จำนวน schedule สูงสุด | 8 รอบ | Worker `validateConfig` |
| อายุ session | 30 วัน | Worker `SESSION_DAYS` |
| การเข้าสู่ระบบผิด | 5 ครั้งต่อนาทีต่อ IP | ตาราง `auth_attempts` |
| การสั่งงาน | 10 คำสั่งต่อนาทีต่อสมาชิก | ตาราง `rate_limits` |
| ระยะเวลาเปิดวาล์วสูงสุด | 30 นาที (ปรับได้ 1–240) | เฟิร์มแวร์ `maxopen` |
| ปริมาณข้อมูลที่คาดไว้ | ประมาณ 10–30 MB ต่อเดือน | – |

## การวินิจฉัยปัญหา

| อาการ | สาเหตุที่เป็นไปได้ | แนวทางแก้ไข |
|---|---|---|
| อุปกรณ์แสดงสถานะ offline | การซิงก์ล้มเหลว หรือเวลาที่พบครั้งล่าสุดเกิน 20 นาที | ตรวจสอบบันทึกของเฟิร์มแวร์ที่บรรทัด `Sync HTTP ...` |
| `Sync HTTP 401` | device token ไม่ถูกต้องหรือถูกหมุนใหม่ | หมุน token ใหม่แล้วตั้งค่าด้วย `set token` ผ่าน serial |
| `Sync HTTP 0` | ปัญหา DNS, TLS, CA หรือ APN | ตรวจสอบ `AT+CCERTLIST` และค่า APN แล้วลองใหม่ |
| คำสั่งค้างสถานะ pending | อุปกรณ์ออฟไลน์นานกว่าอายุคำสั่ง | สั่งใหม่เมื่ออุปกรณ์กลับมาออนไลน์ |
| ได้รับ 429 ขณะเข้าสู่ระบบ | กรอกรหัสผิด 5 ครั้งภายใน 1 นาที | รอ 1 นาที หรือล้างตาราง `auth_attempts` ตาม `docs/RUNBOOK.md` หัวข้อ 9 |
| ได้รับ 429 ขณะสั่งงาน | เกิน 10 คำสั่งต่อนาทีต่อสมาชิก | รอ 1 นาทีแล้วลองใหม่ |
| ได้รับ 409 ขณะบันทึก config | มีผู้ใช้อื่นแก้ไขก่อน (revision เปลี่ยน) | โหลดข้อมูลใหม่แล้วบันทึกอีกครั้ง |
| ได้รับ 428 ขณะบันทึก config | ไม่ได้ส่ง `If-Match` | ส่ง revision ที่อ่านมาล่าสุด |
| Worker ส่ง 500 | D1 binding ไม่ถูกต้อง migration ยังไม่ครบ หรือ body ผิดรูปแบบ | ใช้ `npx wrangler tail water-valve-api-v2 --config wrangler.v2.toml` |
| วาล์วปิดเองโดยไม่คาดคิด | ถึงขีด `maxopen` ตารางเวลาสิ้นสุด หรือมีคำสั่ง `close` | ตรวจสอบบันทึกและตาราง `audit_log` |

## ข้อกำหนดก่อนใช้งานกับวาล์วจริง

- รองรับหนึ่ง household ต่อหนึ่ง MQTT deployment เนื่องจาก MQTT topic ยังเป็นระดับ global (`myhouse/valve/...`) และใช้ผู้ใช้ HiveMQ ร่วมกัน หากสร้าง household ที่สองบน broker เดียวกัน อุปกรณ์อาจรับคำสั่งข้าม household จำเป็นต้องทำ per-device topic ก่อน (ดู `docs/SECURITY.md`)
- ตรวจสอบระดับสัญญาณ active ของ relay ให้ตรงกับบอร์ดจริง (`RELAY_ACTIVE_HIGH`) มิฉะนั้นคำสั่งเปิดอาจกลายเป็นการปิด
- วาล์วถูกบังคับให้ปิดเมื่อเริ่มทำงาน และปิดอัตโนมัติเมื่อเปิดนานเกิน `maxopen` ซึ่งเป็นพฤติกรรมที่ออกแบบไว้
- คำสั่งที่มีอายุจะถูกปฏิเสธหากนาฬิกาของอุปกรณ์ยังไม่ซิงก์ (NTP) ควรตรวจสอบเวลาของอุปกรณ์ก่อนทดสอบ
- ต้องหมุน credential ของ HiveMQ ชุดเดิมก่อนติดตั้งใช้งานจริง

## สถานะการพัฒนา

รายการที่ดำเนินการและตรวจสอบแล้ว:

| ส่วน | สถานะ |
|---|---|
| Worker v2 และ D1 | ติดตั้งบน production แล้ว ชุดทดสอบ API ผ่าน 95/95 รวมถึงการแยกข้อมูลระหว่าง household, ขอบเขตของ device token, การยกเลิก session เมื่อ logout, การหมุนรหัสผ่านและ token และการจำกัดอัตราการเรียกใช้ |
| Household และอุปกรณ์ | สร้างบน production แล้ว ข้อมูลรับรองเก็บไว้นอก repository และการซิงก์ของอุปกรณ์ทำงานได้ |
| แอปพลิเคชัน Android v2.1 | ลงนามและติดตั้งบนอุปกรณ์จริง (Android 12) ทดสอบการเข้าสู่ระบบ การแก้ไข mode และตารางเวลา การเปิด/ปิดด้วยการจำลอง sync/ack หน้าต่างแจ้งข้อขัดแย้ง แคชแบบออฟไลน์ข้าม process การเปลี่ยนรหัสบ้าน และ logout/relogin |
| เฟิร์มแวร์ | คอมไพล์ผ่านทั้งโหมด cellular และ Wi-Fi และการทำงานของ MQTT ร่วมกับ HTTPS sync ผ่านการทดสอบ |
| บอร์ดจริง | ซิงก์ผ่าน HTTPS ด้วยสถานะ `HTTP 200` อุปกรณ์แสดงออนไลน์ใน D1 และคำสั่ง `close` ถูกส่งในรอบซิงก์พร้อม ack สำเร็จ |
| MQTT | เชื่อมต่อ HiveMQ Cloud ด้วยผู้ใช้อุปกรณ์ชุดใหม่ได้ `CONNACK 0` และทดสอบ `getStatus` กับคำสั่ง `close` ผ่าน |

รายการที่ยังไม่ได้ดำเนินการ:

| รายการ | หมายเหตุ |
|---|---|
| ระดับ active ของ relay และการต่อวงจรวาล์ว | ยังไม่ได้เชื่อมต่อวาล์วจริง |
| การอัปโหลด CA และ TLS handshake ของ A7672X บนเครือข่ายผู้ให้บริการ | ปัจจุบันทดสอบผ่าน Wi-Fi |
| ปิดหรือลบผู้ใช้ HiveMQ ชุดเดิมที่รั่วไหล | ต้องดำเนินการที่คอนโซล |
| เสถียรภาพของไฟเลี้ยงระหว่างการส่งข้อมูล cellular | ยังไม่ได้วัดค่า |
| การเปิดและปิดวาล์วจริง | ทดสอบเฉพาะคำสั่งปิดที่ไม่เกิดผลกับอุปกรณ์ |
| การควบคุมแบบ real-time จากแอปพลิเคชัน | อยู่ในระยะถัดไปและต้องมี scoped credential |

## เอกสารอ้างอิง

- `docs/ARCHITECTURE_CENTRAL_CONFIG.md` หัวข้อ 17–18 — สถาปัตยกรรมและข้อตัดสินใจ
- `docs/HARDWARE_TEST_CHECKLIST.md` — ขั้นตอนทดสอบบนฮาร์ดแวร์
- `docs/SECURITY.md` — การจัดการความลับและการหมุน credential
- `docs/RUNBOOK.md` — การติดตั้ง สำรองข้อมูล หมุน token และรับมือเหตุการณ์

## หมายเหตุด้านภาษา

เอกสารนี้คงคำศัพท์เทคนิคบางคำไว้ในรูปภาษาอังกฤษ เนื่องจากเป็นชื่อเฉพาะหรือแปลแล้วทำให้ความหมายคลาดเคลื่อน ได้แก่ Worker, D1, API, MQTT, TLS, ESP32, A7672X, HiveMQ, Cloudflare, Compose, APK, token, firmware, provisioning, sync, deploy, build, release, commit, repository, revision, ETag, If-Match, JSON, Keystore, NVS, GPIO, relay, watchdog, maxopen, schedule, TTL, idempotency, ACL, CA และ NTP
