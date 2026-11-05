# Water Valve Controller v2 (ESP32 + A7672X)

> ภาษา: **ไทย** | [English](README.en.md)

Firmware สำหรับวาล์วน้ำ 1 ตัว ควบคุมด้วย ESP32 ผ่านซิม 4G/5G
Cloudflare Worker v2 + D1 เป็น source of truth ของ mode และ schedule;
MQTT เป็นช่องทาง command/status แบบ real-time

```
Android app ──HTTPS──> Worker v2 ──D1 (config, commands, audit)
                            ▲
                            │ HTTPS sync (15 นาที)
                            │
ESP32 ──MQTT/TLS 8883──> HiveMQ Cloud
```

> แอป Android ปัจจุบันใช้ HTTPS + command queue; MQTT เป็นช่องทางของ ESP32
> (และอนาคตถ้าจะทำ real-time control ในแอป)

## สิ่งที่เปลี่ยนจาก v1

- mode และ schedule มาจาก Worker v2 ไม่ใช่จากมือถือ
- ทุกคำสั่งมี `commandId` (จำ 8 ID ล่าสุดข้าม reboot ทำให้ QoS1 redelivery
  ไม่ execute ซ้ำ) และมี TTL (`expiresAt`)
- schedule ข้ามคืน (เช่น 22:00–06:00) ทำงานถูกต้อง: ช่วงหลังเที่ยงคืนใช้
  flag ของวันก่อนหน้า
- วาล์วถูกบังคับให้ **ปิด** ตอน boot และปิดอีกครั้งถ้าเปิดนานเกิน `maxopen`
  นาที แม้ไม่มีเน็ต
- Wi-Fi ใช้เฉพาะ bench/debug; production ใช้ modem A7672X
- ไม่มี WebView และไม่พึ่ง web UI

## ฮาร์ดแวร์

| Signal | ESP32 pin | หมายเหตุ |
|---|---|---|
| Modem UART RX | GPIO16 | ESP32 RX ← A7672X TX |
| Modem UART TX | GPIO17 | ESP32 TX → A7672X RX |
| Modem power | 5 V | ใช้ supply ที่จ่าย peak ได้ 2 A |
| Relay IN | GPIO23 | ต้องตรวจ active level บน bench |
| Relay VCC/GND | 5 V / GND | ต่อ ground ร่วมกับ ESP32 |
| Valve | Relay COM/NO | ใช้ relay ที่รับกระแสวาล์วได้ |

**ไฟเลี้ยง:** ห้ามเลี้ยง A7672X จากขา 3.3 V ของ ESP32 เพราะ cellular
transmit burst อาจทำให้บอร์ด brown out และ modem reset ให้ใช้ supply 5 V
แยกและต่อ ground ร่วมกัน และเพิ่ม bulk capacitance (≥ 470 µF) ใกล้ modem
ถ้าจำเป็น

**ความปลอดภัยของ relay:** ค่าเริ่มต้นคือ `RELAY_ACTIVE_HIGH 1`
(HIGH = เปิดวาล์ว) ตาม firmware เดิม แต่ relay module บางรุ่นเป็น active-low
ก่อนต่อวาล์วจริง ให้ flash firmware แล้วดู LED ของ relay และยืนยันว่า:

- boot → relay off / วาล์วปิด
- `set`/`sync`/command → relay click เฉพาะตอนที่ควร

ถ้า logic กลับด้าน ให้เปลี่ยน `RELAY_ACTIVE_HIGH` เป็น `0` แล้ว flash ใหม่

สำหรับ solenoid valve ให้เพิ่ม flyback diode คร่อม coil และ fuse ที่ไฟเลี้ยง
วาล์ว ห้ามสลับไฟบ้านบน breadboard

## ไลบรารีที่ต้องใช้

ติดตั้งผ่าน Library Manager หรือ `arduino-cli lib install`:

- `TinyGSM` **0.12.0** (รองรับ A7672X)
- `PubSubClient` **2.8**
- `ArduinoJson` **7.x** (ทดสอบ 7.4.3)
- ESP32 core **3.3.11** (`esp32:esp32`)

`Preferences`, `WiFi`, `WiFiClientSecure` และ `esp_task_wdt` มาพร้อม
ESP32 core

## Build

Cellular production build (ค่าเริ่มต้น, `USE_CELLULAR=1`):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "$env:USERPROFILE\Documents\Arduino\libraries" `
  esp32_water_valve
```

Wi-Fi bench build (`USE_CELLULAR=0`):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "$env:USERPROFILE\Documents\Arduino\libraries" `
  --build-property "compiler.cpp.extra_flags=-DUSE_CELLULAR=0" `
  esp32_water_valve
```

Flash (เปลี่ยน `COM5` ตามพอร์ตจริง):

```powershell
arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32 esp32_water_valve
```

Wi-Fi build ใหญ่กว่า (TLS + Wi-Fi ≈ 1.05 MB / 80% ของ app partition
ค่าเริ่มต้น) แต่ยังพอดี ส่วน cellular build ≈ 460 KB

## Provisioning ครั้งแรก

เปิด serial monitor ที่ **115200** baud, line ending **Newline**
ค่าที่ provision จะเก็บใน NVS และอยู่ข้าม reboot

1. สร้าง household/device ด้วย Worker setup (ครั้งเดียวต่อ household):

```powershell
$body = @{
  householdName = 'Home'
  deviceName    = 'Valve 1'
  password      = '<household-password>'
  timezone      = 'Asia/Bangkok'
} | ConvertTo-Json

Invoke-RestMethod -Method Post `
  -Uri 'https://water-valve-api-v2.czqwertyyy.workers.dev/v1/setup' `
  -Headers @{ 'X-Setup-Token' = '<SETUP_TOKEN>' } `
  -ContentType 'application/json' `
  -Body $body
```

ผลลัพธ์จะมี `inviteCode`, `deviceId` และ `deviceToken` (`dev_...`)
เก็บ device token เป็นความลับ เพราะแสดงครั้งเดียว

2. บน serial console ของ ESP32:

```text
set token dev_xxxxxxxxxxxxxxxx
set apn internet
set apnuser <apn-user-if-required>
set apnpass <apn-password-if-required>
set url https://water-valve-api-v2.czqwertyyy.workers.dev
set mqtthost <hivemq-host>
set mqttuser <hivemq-user>
set mqttpass <hivemq-password>
set maxopen 30
sync
status
reboot
```

เฉพาะ Wi-Fi bench build:

```text
set ssid <wifi-name>
set pass <wifi-password>
```

หมายเหตุ:

- `set url` จะตัด trailing slash ออก
- `set maxopen` รับ 1–240 นาที; ค่าเริ่มต้น 30
- `status` แสดง secret แบบ mask
- `sync` บังคับ HTTPS sync ทันที
- Worker URL มีค่าเริ่มต้นใน firmware แต่ override ได้ด้วย `set url`
- MQTT **ไม่มี** host/credential ฝังใน firmware; ต้อง provision ด้วย
  `set mqtthost`, `set mqttuser`/`set mqttpass` ก่อนใช้ MQTT
- APN ค่าเริ่มต้นคือ `internet`; เปลี่ยนตาม carrier ถ้าจำเป็น

## คำสั่ง serial

| Command | ความหมาย |
|---|---|
| `help` | แสดงรายการคำสั่ง |
| `show` | พิมพ์ config (secret ถูก mask) |
| `set token <dev_...>` | device token จาก `/v1/setup` |
| `set apn <name>` | APN ของ cellular |
| `set apnuser <u>` / `set apnpass <p>` | credential ของ APN |
| `set ssid <name>` / `set pass <p>` | Wi-Fi bench เท่านั้น |
| `set url <https://...>` | base URL ของ Worker |
| `set mqtthost <host>` | hostname ของ MQTT broker (ต้อง provision เอง) |
| `set mqttuser <u>` / `set mqttpass <p>` | MQTT credential |
| `set maxopen <1..240>` | เวลาเปิดสูงสุดฝั่งเครื่อง (นาที) |
| `sync` | บังคับ HTTPS config/command sync |
| `status` | publish MQTT status และพิมพ์ config |
| `reboot` | restart ESP32 |

## Timezone

Schedule และ NTP ใช้ offset คงที่ **UTC+7 (Asia/Bangkok)**
(`GMT_OFFSET_SEC` ใน sketch) Worker household มี field `timezone`
แต่ firmware ยังไม่อ่านค่านี้ ถ้าบ้านไม่ได้อยู่ UTC+7 ให้เปลี่ยน
`GMT_OFFSET_SEC` แล้ว deploy ใหม่ แอป Android แสดงเวลา schedule ตาม
timezone ของมือถือ ดังนั้นมือถือต้องตั้ง timezone เดียวกับวาล์ว

## MQTT topics

> **คำเตือนเรื่อง isolation ใน v1:** topic เป็น global (`myhouse/valve/...`)
> และใช้ HiveMQ user ร่วมกัน ให้ deploy แค่ 1 household/valve ต่อ MQTT broker
> จนกว่าจะทำ per-device topic หรือ scoped credential (ดู `../docs/SECURITY.md`)

MQTT เป็น optional สำหรับ v1: ถ้า `mqttuser`/`mqttpass` ว่าง อุปกรณ์ยัง
เชื่อมต่อ, sync config ทุก 15 นาที และรัน schedule/queued command ได้
ต่างกันแค่ไม่มี real-time MQTT control/status

Prefix ค่าเริ่มต้น: `myhouse/valve/`

| Topic | ทิศทาง | Payload |
|---|---|---|
| `control` | app → ESP32 | `ON` / `OFF` หรือ JSON (ด้านล่าง) |
| `status` | ESP32 → app | state JSON (ด้านล่าง) |
| `mode` | legacy | `{"mode":"auto"}` — ถูก override ด้วย sync ถัดไป |
| `schedule` | legacy | `{"schedules":[...]}` — ถูก override ด้วย sync ถัดไป |

Control JSON:

```json
{
  "cmd": "open",
  "commandId": "cmd_...",
  "expiresAt": "2026-10-30T12:00:00.000Z"
}
```

`cmd` ที่รองรับ: `open`, `close`, `toggle`, `getStatus`, `sync`, `reboot`
`ON`/`OFF` ยังอยู่เพื่อ compatibility และไม่มี TTL คำสั่งที่มี `expiresAt`
จะถูกปฏิเสธถ้า clock ในเครื่องเลย TTL (บวก grace 5 วินาที) จำ command ID
8 ตัวล่าสุดใน NVS ทำให้ `commandId` เดิมถูก execute มากสุดครั้งเดียว
แม้ reboot หรือ MQTT redelivery การเปิดถูกปฏิเสธใน auto mode (`auto_mode`);
**การปิดทำได้เสมอ** เพื่อให้ manual close หยุดวาล์วได้หลังบ้านเปลี่ยนเป็น auto

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

`lastCommandId` คือ real-time ack ของคำสั่ง MQTT ที่สำเร็จ
`lastRejectedId`/`lastRejectedError` รายงานคำสั่งที่ถูกปฏิเสธ HTTPS sync
ครั้งถัดไปจะ mark `lastCommandId` ว่า acked ใน D1 ด้วย ทำให้ ack ไม่หาย
ถ้าแอปพลาด status message

## HTTPS sync

ทุก 15 นาที (retry 60 วินาทีหลัง fail) firmware จะเรียก:

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

> MQTT reconnect **ไม่** บังคับให้ HTTPS sync ทันที sync จะรันตามรอบ,
> หลัง network reconnect หรือเมื่อใช้คำสั่ง serial `sync`
> เพื่อให้ data budget บนซิมคาดเดาได้เมื่อ MQTT flap

response มี `revision`, `mode`, `schedules`, `command` ที่ค้างอยู่ (ถ้ามี)
และ `serverTime` ถ้า clock ยังไม่ถูกตั้ง จะใช้ `serverTime` ตั้งก่อน
ประเมิน schedule

### ข้อจำกัดสำคัญของ A7672X: TLS session เดียว

TinyGSM driver ของ A7672X ใช้ SSL session 0 สำหรับทุก secure client
ดังนั้น MQTT และ HTTPS จึงเปิดพร้อมกันไม่ได้ firmware จะ:

1. disconnect MQTT (ส่ง DISCONNECT + ปิด TLS session)
2. ทำ HTTPS sync/ack
3. reconnect MQTT ใน loop ถัดไป

ในช่วงสั้น ๆ นี้ (ไม่กี่วินาทีทุก 15 นาที) คำสั่ง MQTT จะไม่ถูกส่ง
สำหรับงานที่สำคัญ แอปควร POST `/v1/commands` เพื่อให้คำสั่งเข้า queue
ใน D1 และถูกส่งใน sync ถัดไป ห้าม "แก้" ด้วยการเปิด TinyGSM secure client
สองตัว เพราะ modem session และ `sockets[0]` routing จะชนกัน

## Data budget (ซิม 399 MB/เดือน)

การออกแบบ single-session ทำให้มี TLS handshake ใหม่ทุก sync และ MQTT
reconnect ประมาณการจริง:

| Traffic | ประมาณ |
|---|---|
| HTTPS sync ทุก 15 นาที (handshake + request/response) | 30–60 MB/เดือน |
| MQTT reconnect หลังแต่ละ sync | 20–40 MB/เดือน |
| Status ทุก 5 นาที | 3–5 MB/เดือน |
| Commands/acks | < 1 MB/เดือน |
| **รวม** | **~55–105 MB/เดือน** |

ยังเหลือ headroom จาก 399 MB ถ้า carrier คิดค่า signaling หรือ modem retry
หนัก ให้ดู SIM dashboard หลังสัปดาห์แรกและเพิ่ม `SYNC_INTERVAL_MS` ถ้าจำเป็น

## การทำงานด้านความปลอดภัย

- Relay ถูกสั่ง **ปิด** ก่อนอย่างอื่นทั้งหมดใน `setup()`
- `maxopen` (ค่าเริ่มต้น 30 นาที) ถูกบังคับใช้ฝั่งเครื่อง; วาล์วปิดแม้เน็ตหลุด
  และรายงาน fault เป็น `max_runtime`
- คำสั่ง `close` ทำได้ทั้ง manual และ auto; คำสั่ง `open` ทำได้เฉพาะ manual
- manual `close` ระหว่าง auto schedule ที่ active จะถูก latch ใน NVS และชนะ
  จนจบ window ถ้าไม่มี latch นี้ `autoCheck` ทุก 10 วินาทีจะเปิดวาล์วซ้ำ
  การเปลี่ยนเป็น manual จะล้าง latch
- คำสั่ง `open` ใหม่จะล้าง latch; schedule ที่จบลงก็ล้าง latch เช่นกัน
- คำสั่งที่มี TTL จะถูกปฏิเสธถ้า clock ยังไม่ sync
- task watchdog 120 วินาทีจะ reset ESP32 ถ้า loop ค้าง
- สถานะวาล์วไม่ถูกเก็บข้าม reboot; boot มาจะปิดเสมอ

## TLS certificate rotation

ณ **5 พ.ย. 2026** `water-valve-api-v2.czqwertyyy.workers.dev` ให้ chain
ที่ root เป็น **GlobalSign Root CA (RSA)**:
leaf → Google Trust Services WE1 → GTS Root R4 (cross-signed) →
GlobalSign Root CA และ `ROOT_CA` ของ firmware คือ RSA root นี้

MQTT (HiveMQ Cloud) ใช้ chain คนละตัว root เป็น **ISRG Root X1**
(Let's Encrypt); `ROOT_CA_HIVEMQ` เก็บ root นี้ WiFi build จะสร้าง
bundle ถาวร (`ROOT_CA` + `ROOT_CA_HIVEMQ`) และเรียก `setCACert()` ครั้งเดียว
bundle ต้องมีอายุตลอดอายุ client — ถ้าใช้ `String` ชั่วคราวจะทำให้ mbedTLS
ขึ้น "X509 - Format not recognized"

ถ้า Cloudflare หรือ HiveMQ เปลี่ยน chain:

1. ตรวจ chain จริงจาก client ที่ต่อถึง endpoint:
   `openssl s_client -showcerts -connect <host>:443|8883 -servername <host>`
2. ดึง root CA PEM
3. เปลี่ยน `ROOT_CA` (HTTPS) หรือ `ROOT_CA_HIVEMQ` (MQTT) ใน
   `esp32_water_valve.ino`
4. Flash ใหม่ firmware จะ upload CA ไปเก็บใน modem เป็น `cfroot.pem`
   ตอน boot

ไฟล์ certificate ใน modem เล็ก (~1–2 KB) และ upload ครั้งเดียวต่อ boot
ดังนั้น modem ที่ factory-reset จะกลับมาทำงานได้เอง

## แก้ปัญหา

| อาการ | ตรวจสอบ |
|---|---|
| `modem.init failed` | supply 5 V/current, TX/RX ไขว้กัน, SIM เสียบแน่น, antenna, ขา `SerialAT.begin` |
| `GPRS connect failed` | APN, แพ็กเกจ data ของ SIM, signal; ลอง `set apn` |
| `MQTT failed, rc=-2` | TLS/network; CA, clock, port 8883 |
| `MQTT failed, rc=4` | MQTT username/password ผิด |
| `MQTT failed, rc=5` | HiveMQ ACL ไม่อนุญาต topic |
| `Sync HTTP 401` | device token ผิด/ถูก revoke; รัน `set token` ใหม่ |
| `Sync HTTP 0` | DNS/TLS/connect fail; ดู serial log และ CA |
| `TLS: CA upload failed` | file system ของ modem; power-cycle modem, ตรวจ `AT+CCERTLIST` |
| Relay กลับด้าน | ตั้ง `RELAY_ACTIVE_HIGH 0`, flash ใหม่, ทดสอบโดยยังไม่ต่อวาล์ว |
| วาล์วปิดเองไม่คาดคิด | ถึง `maxopen`, schedule จบ, หรือมี MQTT `close` |

## ไฟล์

- `esp32_water_valve.ino` — firmware
- `README.md` — ไฟล์นี้
- Worker API: `../worker/v2/index.js`
- Schema: `../migrations/0001_init.sql`
- Architecture: `../docs/ARCHITECTURE_CENTRAL_CONFIG.md`

## หมายเหตุเรื่องคำศัพท์

คำเทคนิคเช่น MQTT, TLS, HTTPS, ESP32, A7672X, HiveMQ, Cloudflare, Worker,
D1, token, firmware, provisioning, sync, deploy, build, relay, watchdog,
maxopen, schedule, mode, manual, auto, command queue, TTL, idempotency,
ACL, CA, NTP, RSSI, payload, QoS, NVS, UART, GPIO คงเป็นภาษาอังกฤษเพราะ
แปลไทยแล้วอ่านเพี้ยนหรือเป็นชื่อเฉพาะ
