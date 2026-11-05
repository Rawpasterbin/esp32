# Water Valve Controller v2 (ESP32 + A7672X)

> ภาษา: **ไทย** | [English](README.en.md)

เฟิร์มแวร์สำหรับควบคุมวาล์วน้ำหนึ่งตัวด้วย ESP32 ผ่านเครือข่าย 4G/5G โดยมี Cloudflare Worker v2 และ D1 เป็นแหล่งความจริงของ mode และ schedule และใช้ MQTT เป็นช่องทาง command/status แบบ real-time

```
Android app ──HTTPS──> Worker v2 ──D1 (config, commands, audit)
                            ▲
                            │ HTTPS sync (15 นาที)
                            │
ESP32 ──MQTT/TLS 8883──> HiveMQ Cloud
```

> แอปพลิเคชัน Android ใช้ HTTPS และ command queue เป็นหลัก ส่วน MQTT เป็นช่องทางของ ESP32 และจะรองรับ real-time control จากแอปพลิเคชันในระยะถัดไป

## หมายเหตุการออกแบบ

- mode และ schedule มาจาก Worker v2 ไม่ได้เก็บไว้บนมือถือ
- ทุกคำสั่งมี `commandId` โดยหน่วยความจำเก็บ 8 ID ล่าสุดข้าม reboot ทำให้ QoS1 redelivery ไม่ทำให้คำสั่งถูกทำซ้ำ และมี TTL (`expiresAt`)
- schedule ข้ามคืน เช่น 22:00–06:00 ทำงานถูกต้อง โดยช่วงหลังเที่ยงคืนใช้ flag ของวันก่อนหน้า
- วาล์วถูกบังคับให้ปิดเมื่อเริ่มทำงาน และปิดอีกครั้งหากเปิดนานเกิน `maxopen` นาที แม้ไม่มีการเชื่อมต่อเครือข่าย
- Wi-Fi ใช้สำหรับการทดสอบเท่านั้น ส่วนการใช้งานจริงใช้โมดูล A7672X
- ไม่มี WebView และไม่พึ่งพา web UI

## ฮาร์ดแวร์

| Signal | ESP32 pin | หมายเหตุ |
|---|---|---|
| Modem UART RX | GPIO16 | ESP32 RX ← A7672X TX |
| Modem UART TX | GPIO17 | ESP32 TX → A7672X RX |
| Modem power | 5 V | ใช้แหล่งจ่ายที่รองรับกระแสสูงสุด 2 A |
| Relay IN | GPIO23 | ต้องตรวจสอบระดับสัญญาณกับบอร์ดจริง |
| Relay VCC/GND | 5 V / GND | ต่อ ground ร่วมกับ ESP32 |
| Valve | Relay COM/NO | ใช้ relay ที่รองรับกระแสของวาล์ว |

**ไฟเลี้ยง:** ห้ามจ่ายไฟให้ A7672X จากขา 3.3 V ของ ESP32 เพราะกระแสสูงขณะส่งข้อมูล cellular อาจทำให้บอร์ด brown out และโมดูล reset ให้ใช้แหล่งจ่าย 5 V แยกและต่อ ground ร่วมกัน พร้อมเพิ่ม bulk capacitance (470 µF ขึ้นไป) ใกล้โมดูล

**ความปลอดภัยของ relay:** ค่าเริ่มต้นคือ `RELAY_ACTIVE_HIGH 1` (HIGH = เปิดวาล์ว) ตามเฟิร์มแวร์เดิม แต่ relay module บางรุ่นเป็น active-low ก่อนต่อวาล์วจริงให้ flash เฟิร์มแวร์และตรวจสอบว่า:

- boot แล้ว relay ปิดและวาล์วปิด
- `set`, `sync` หรือ command ทำให้ relay เปลี่ยนสถานะเฉพาะเมื่อควร

หาก logic กลับด้าน ให้เปลี่ยน `RELAY_ACTIVE_HIGH` เป็น `0` แล้ว flash ใหม่

สำหรับ solenoid valve ให้เพิ่ม flyback diode คร่อม coil และ fuse ที่ไฟเลี้ยงวาล์ว ห้ามต่อไฟบ้านบน breadboard

## ไลบรารีที่ต้องใช้

ติดตั้งผ่าน Library Manager หรือ `arduino-cli lib install`:

- `TinyGSM` **0.12.0** (รองรับ A7672X)
- `PubSubClient` **2.8**
- `ArduinoJson` **7.x** (ทดสอบกับ 7.4.3)
- ESP32 core **3.3.11** (`esp32:esp32`)

`Preferences`, `WiFi`, `WiFiClientSecure` และ `esp_task_wdt` มาพร้อมกับ ESP32 core

## Build

Cellular build สำหรับการใช้งานจริง (`USE_CELLULAR=1`):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" `
  esp32_water_valve
```

Wi-Fi test build (`USE_CELLULAR=0`):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" `
  --build-property "compiler.cpp.extra_flags=-DUSE_CELLULAR=0" `
  esp32_water_valve
```

Flash ไปยังบอร์ด (ระบุพอร์ตจริง):

```powershell
arduino-cli upload -p <serial-port> --fqbn esp32:esp32:esp32 esp32_water_valve
```

Wi-Fi build มีขนาดใหญ่กว่า (TLS และ Wi-Fi ประมาณ 1.05 MB หรือ 80% ของ app partition ค่าเริ่มต้น) แต่ยังอยู่ในขอบเขต ส่วน cellular build มีขนาดประมาณ 460 KB

## Provisioning ครั้งแรก

เปิด serial monitor ที่ **115200** baud, line ending **Newline** ค่าที่ตั้งไว้จะถูกเก็บใน NVS และคงอยู่ข้าม reboot

1. สร้าง household และ device ผ่าน Worker setup (ครั้งเดียวต่อ household):

```powershell
$body = @{
  householdName = '<household-name>'
  deviceName    = '<device-name>'
  password      = '<household-password>'
  timezone      = 'Asia/Bangkok'
} | ConvertTo-Json

Invoke-RestMethod -Method Post `
  -Uri 'https://<your-worker-url>/v1/setup' `
  -Headers @{ 'X-Setup-Token' = '<SETUP_TOKEN>' } `
  -ContentType 'application/json' `
  -Body $body
```

ผลลัพธ์ประกอบด้วย `inviteCode`, `deviceId` และ `deviceToken` (`dev_...`) ซึ่งแสดงเพียงครั้งเดียวและต้องเก็บเป็นความลับ

2. ตั้งค่าบน serial console ของ ESP32:

```text
set token dev_xxxxxxxxxxxxxxxx
set apn internet
set apnuser <apn-user-if-required>
set apnpass <apn-password-if-required>
set url https://<your-worker-url>
set mqtthost <hivemq-host>
set mqttuser <hivemq-user>
set mqttpass <hivemq-password>
set maxopen 30
sync
status
reboot
```

เฉพาะ Wi-Fi test build:

```text
set ssid <wifi-name>
set pass <wifi-password>
```

หมายเหตุ:

- `set url` จะตัด trailing slash ออก
- `set maxopen` รับค่า 1–240 นาที ค่าเริ่มต้น 30
- `status` แสดง secret ในรูปแบบ mask
- `sync` บังคับให้ทำ HTTPS sync ทันที
- Worker URL ไม่มีค่าเริ่มต้นในเฟิร์มแวร์ ต้องตั้งด้วย `set url` ก่อน sync
- MQTT ไม่มี host หรือ credential ฝังในเฟิร์มแวร์ ต้องตั้งค่าด้วย `set mqtthost`, `set mqttuser` และ `set mqttpass` ก่อนใช้งาน
- APN ค่าเริ่มต้นคือ `internet` ให้เปลี่ยนตามผู้ให้บริการหากจำเป็น

## คำสั่ง serial

| Command | ความหมาย |
|---|---|
| `help` | แสดงรายการคำสั่ง |
| `show` | พิมพ์ config โดย secret ถูก mask |
| `set token <dev_...>` | device token จาก `/v1/setup` |
| `set apn <name>` | APN ของ cellular |
| `set apnuser <u>` / `set apnpass <p>` | credential ของ APN |
| `set ssid <name>` / `set pass <p>` | Wi-Fi test build เท่านั้น |
| `set url <https://...>` | base URL ของ Worker |
| `set mqtthost <host>` | hostname ของ MQTT broker |
| `set mqttuser <u>` / `set mqttpass <p>` | MQTT credential |
| `set maxopen <1..240>` | เวลาเปิดสูงสุดฝั่งเครื่อง (นาที) |
| `sync` | บังคับ HTTPS config/command sync |
| `status` | publish MQTT status และพิมพ์ config |
| `reboot` | restart ESP32 |

## Timezone

Schedule และ NTP ใช้ offset คงที่ **UTC+7 (Asia/Bangkok)** (`GMT_OFFSET_SEC` ใน sketch) Worker household มี field `timezone` แต่เฟิร์มแวร์ยังไม่อ่านค่านี้ หากไม่ได้ใช้งานในเขต UTC+7 ให้เปลี่ยน `GMT_OFFSET_SEC` แล้ว deploy ใหม่ แอปพลิเคชัน Android แสดงเวลา schedule ตาม timezone ของอุปกรณ์ ดังนั้นอุปกรณ์ต้องตั้ง timezone เดียวกับวาล์ว

## MQTT topics

> **ข้อจำกัดปัจจุบัน:** topic เป็นระดับ global (`myhouse/valve/...`) และใช้ HiveMQ user ร่วมกัน จึงรองรับหนึ่ง household ต่อหนึ่ง MQTT broker จนกว่าจะทำ per-device topic หรือ scoped credential

MQTT เป็น optional: หาก `mqttuser` และ `mqttpass` ว่าง อุปกรณ์ยังเชื่อมต่อและ sync config ทุก 15 นาที รวมถึงรัน schedule และ queued command ได้ ต่างกันเพียงไม่มี real-time MQTT control และ status

Prefix ค่าเริ่มต้น: `myhouse/valve/`

| Topic | ทิศทาง | Payload |
|---|---|---|
| `control` | app → ESP32 | `ON` / `OFF` หรือ JSON (ด้านล่าง) |
| `status` | ESP32 → app | state JSON (ด้านล่าง) |
| `mode` | legacy | `{"mode":"auto"}` ซึ่งถูก override ด้วย sync ถัดไป |
| `schedule` | legacy | `{"schedules":[...]}` ซึ่งถูก override ด้วย sync ถัดไป |

Control JSON:

```json
{
  "cmd": "open",
  "commandId": "cmd_...",
  "expiresAt": "2026-10-30T12:00:00.000Z"
}
```

`cmd` ที่รองรับ: `open`, `close`, `toggle`, `getStatus`, `sync`, `reboot` ส่วน `ON`/`OFF` ยังคงอยู่เพื่อความเข้ากันได้และไม่มี TTL คำสั่งที่มี `expiresAt` จะถูกปฏิเสธหาก clock ของอุปกรณ์เลย TTL ไปแล้ว (บวก grace 5 วินาที) ระบบจำ command ID 8 ตัวล่าสุดใน NVS ทำให้ `commandId` เดิมถูกทำมากที่สุดหนึ่งครั้งแม้ reboot หรือ MQTT redelivery การเปิดถูกปฏิเสธใน auto mode (`auto_mode`) แต่การปิดทำได้เสมอ เพื่อให้ manual close หยุดวาล์วได้หลัง household เปลี่ยนเป็น auto

Status JSON (ย่อ):

```json
{
  "valve": false,
  "online": true,
  "mode": "auto",
  "scheduleCount": 2,
  "ntp": true,
  "time": 1790000000,
  "rssi": -67,
  "uptime": 3600,
  "fw": "2.1.0",
  "lastCommandId": "cmd_...",
  "lastRejectedId": "cmd_...",
  "lastRejectedError": "auto_mode"
}
```

`lastCommandId` คือ real-time ack ของคำสั่ง MQTT ที่สำเร็จ ส่วน `lastRejectedId` และ `lastRejectedError` รายงานคำสั่งที่ถูกปฏิเสธ HTTPS sync ครั้งถัดไปจะ mark `lastCommandId` ว่า acked ใน D1 ทำให้ ack ไม่สูญหายหากแอปพลิเคชันพลาด status message

## HTTPS sync

ทุก 15 นาที (retry หลัง 60 วินาทีเมื่อล้มเหลว) เฟิร์มแวร์จะเรียก:

```
POST /v1/devices/sync
Authorization: Bearer dev_...
Content-Type: application/json

{
  "valve": false,
  "mode": "auto",
  "rssi": -67,
  "uptimeSeconds": 3600,
  "firmwareVersion": "2.1.0",
  "lastCommandId": "cmd_...",
  "fault": null
}
```

> MQTT reconnect ไม่ได้บังคับให้ทำ HTTPS sync ทันที sync จะทำงานตามรอบ หลัง network reconnect หรือเมื่อใช้คำสั่ง serial `sync` เพื่อให้ปริมาณข้อมูลบนซิมคาดเดาได้เมื่อ MQTT ขาดการเชื่อมต่อเป็นระยะ

response ประกอบด้วย `revision`, `mode`, `schedules`, `command` ที่ค้างอยู่ (ถ้ามี) และ `serverTime` หาก clock ของอุปกรณ์ยังไม่ถูกตั้ง จะใช้ `serverTime` ในการตั้งค่าก่อนประเมิน schedule

### ข้อจำกัดของ A7672X: TLS session เดียว

TinyGSM driver ของ A7672X ใช้ SSL session 0 สำหรับทุก secure client ดังนั้น MQTT และ HTTPS จึงเปิดพร้อมกันไม่ได้ เฟิร์มแวร์จะ:

1. disconnect MQTT (ส่ง DISCONNECT และปิด TLS session)
2. ทำ HTTPS sync/ack
3. reconnect MQTT ใน loop ถัดไป

ในช่วงเวลาสั้น ๆ นี้ (ไม่กี่วินาทีทุก 15 นาที) คำสั่ง MQTT จะไม่ถูกส่ง สำหรับงานที่สำคัญ แอปพลิเคชันควรเรียก `POST /v1/households/:hid/commands` เพื่อให้คำสั่งเข้าคิวใน D1 และถูกส่งใน sync ถัดไป การเปิด TinyGSM secure client สองตัวพร้อมกันจะทำให้ modem session และ routing ของ `sockets[0]` ชนกัน

## Data budget (ซิม 399 MB/เดือน)

การออกแบบ single-session ทำให้มี TLS handshake ใหม่ทุก sync และ MQTT reconnect ประมาณการจริง:

| Traffic | ประมาณ |
|---|---|
| HTTPS sync ทุก 15 นาที (handshake + request/response) | 30–60 MB/เดือน |
| MQTT reconnect หลังแต่ละ sync | 20–40 MB/เดือน |
| Status ทุก 5 นาที | 3–5 MB/เดือน |
| Commands/acks | น้อยกว่า 1 MB/เดือน |
| **รวม** | **55–105 MB/เดือน** |

ปริมาณดังกล่าวยังอยู่ในงบ 399 MB หากผู้ให้บริการคิดค่า signaling หรือ modem retry มาก ให้ตรวจ dashboard ของซิมหลังใช้งานสัปดาห์แรกและเพิ่ม `SYNC_INTERVAL_MS` หากจำเป็น

## การทำงานด้านความปลอดภัย

- Relay ถูกสั่งปิดก่อนการทำงานอื่นทั้งหมดใน `setup()`
- `maxopen` (ค่าเริ่มต้น 30 นาที) ถูกบังคับใช้ฝั่งเครื่อง วาล์วปิดแม้เครือข่ายหลุด และรายงาน fault เป็น `max_runtime`
- คำสั่ง `close` ทำได้ทั้ง manual และ auto ส่วนคำสั่ง `open` ทำได้เฉพาะ manual
- manual `close` ระหว่าง auto schedule ที่ทำงานอยู่จะถูก latch ใน NVS และมีผลจนจบ window หากไม่มี latch นี้ `autoCheck` ทุก 10 วินาทีจะเปิดวาล์วซ้ำ การเปลี่ยนเป็น manual จะล้าง latch
- คำสั่ง `open` ใหม่จะล้าง latch และ schedule ที่จบลงก็ล้าง latch เช่นกัน
- คำสั่งที่มี TTL จะถูกปฏิเสธหาก clock ยังไม่ sync
- task watchdog 120 วินาทีจะ reset ESP32 หาก loop ค้าง
- สถานะวาล์วไม่ถูกเก็บข้าม reboot อุปกรณ์จะเริ่มทำงานในสถานะปิดเสมอ

## TLS certificate rotation

Worker host ที่ deploy ใช้ chain ที่มี root เป็น **GlobalSign Root CA (RSA)** ในกรณีของ Cloudflare:

leaf → Google Trust Services WE1 → GTS Root R4 (cross-signed) → GlobalSign Root CA

`ROOT_CA` ของเฟิร์มแวร์คือ RSA root นี้ ส่วน MQTT (HiveMQ Cloud) ใช้ chain ที่มี root เป็น **ISRG Root X1** (Let's Encrypt) ซึ่งเก็บใน `ROOT_CA_HIVEMQ` Wi-Fi build จะสร้าง bundle ถาวร (`ROOT_CA` + `ROOT_CA_HIVEMQ`) และเรียก `setCACert()` ครั้งเดียว bundle ต้องมีอายุตลอดอายุของ client หากใช้ `String` ชั่วคราว mbedTLS จะรายงาน "X509 - Format not recognized"

หาก Cloudflare หรือ HiveMQ เปลี่ยน chain:

1. ตรวจ chain จริงจาก client ที่เชื่อมต่อถึง endpoint:
   `openssl s_client -showcerts -connect <host>:443|8883 -servername <host>`
2. ดึง root CA PEM
3. เปลี่ยน `ROOT_CA` (HTTPS) หรือ `ROOT_CA_HIVEMQ` (MQTT) ใน `esp32_water_valve.ino`
4. Flash เฟิร์มแวร์ใหม่ ระบบจะ upload CA ไปเก็บในโมดูลเป็น `cfroot.pem` ตอน boot

ไฟล์ certificate ในโมดูลมีขนาดเล็ก (1–2 KB) และ upload ครั้งเดียวต่อ boot ดังนั้นโมดูลที่ factory-reset จะกลับมาทำงานได้เอง

## แก้ปัญหา

| อาการ | ตรวจสอบ |
|---|---|
| `modem.init failed` | ไฟเลี้ยง 5 V และกระแส, การต่อ TX/RX, SIM, antenna และขา `SerialAT.begin` |
| `GPRS connect failed` | APN, แพ็กเกจ data ของ SIM, signal และลอง `set apn` |
| `MQTT failed, rc=-2` | TLS หรือ network: CA, clock และพอร์ต 8883 |
| `MQTT failed, rc=4` | MQTT username หรือ password ไม่ถูกต้อง |
| `MQTT failed, rc=5` | HiveMQ ACL ไม่อนุญาต topic |
| `Sync HTTP 401` | device token ไม่ถูกต้องหรือถูก revoke ให้ตั้งค่า `set token` ใหม่ |
| `Sync HTTP 0` | DNS, TLS หรือ connect ล้มเหลว ตรวจ serial log และ CA |
| `TLS: CA upload failed` | file system ของโมดูล ให้ power-cycle และตรวจ `AT+CCERTLIST` |
| Relay กลับด้าน | ตั้ง `RELAY_ACTIVE_HIGH 0`, flash ใหม่ และทดสอบโดยยังไม่ต่อวาล์ว |
| วาล์วปิดเองโดยไม่คาดคิด | ถึง `maxopen`, schedule จบ หรือมี MQTT `close` |

## ไฟล์

- `esp32_water_valve.ino` — เฟิร์มแวร์
- `README.md` — เอกสารนี้
- Worker API: `../worker/v2/index.js`
- Schema: `../migrations/0001_init.sql`
- Main README: `../README.md`

## หมายเหตุเรื่องคำศัพท์

คำเทคนิค เช่น MQTT, TLS, HTTPS, ESP32, A7672X, HiveMQ, Cloudflare, Worker, D1, token, firmware, provisioning, sync, deploy, build, relay, watchdog, schedule, GPIO, NVS, CA และ NTP คงไว้ในรูปภาษาอังกฤษเนื่องจากเป็นชื่อเฉพาะหรือแปลแล้วทำให้ความหมายคลาดเคลื่อน
