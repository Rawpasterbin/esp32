# เอกสารโปรเจกต์ appesp32

โฟลเดอร์นี้เก็บผลวิเคราะห์และสถาปัตยกรรมสำหรับการพัฒนาโปรเจกต์ Water Valve Scheduler

| เอกสาร | เนื้อหา |
|---|---|
| [`PROJECT_ANALYSIS.md`](./PROJECT_ANALYSIS.md) | วิเคราะห์ระบบปัจจุบันทั้งหมด: Web UI, Worker, ESP32, Android, data model, API, ปัญหา/ความเสี่ยง, ผลทดสอบ |
| [`ARCHITECTURE_CENTRAL_CONFIG.md`](./ARCHITECTURE_CENTRAL_CONFIG.md) | สถาปัตยกรรมเป้าหมาย: central config ด้วย D1 + revision + auth, API spec, sync protocol, safety, แผน migration, ประมาณการงาน |
| [`HARDWARE_TEST_CHECKLIST.md`](./HARDWARE_TEST_CHECKLIST.md) | ขั้นตอนทดสอบฮาร์ดแวร์: relay polarity, cellular/TLS, MQTT ACL, sync/ack, safety, data budget, Android runtime |

## เอกสารในโปรเจกต์ Android

| เอกสาร | เนื้อหา |
|---|---|
| `C:\Users\ssde\AndroidStudioProjects\ESP32\docs\MIGRATION_PLAN.md` | แผนย้าย Android จาก WebView เป็น native Compose UI + เชื่อม Worker API |

## สรุปสั้น

- **ทำได้** ที่จะให้ Worker เป็น backend กลางและทุกเครื่องเห็น setting เดียวกัน
- Worker v2 + D1 ถูก deploy จริงแล้วที่ `https://water-valve-api-v2.czqwertyyy.workers.dev` (ดู §18 ใน `ARCHITECTURE_CENTRAL_CONFIG.md`)
- Android เป็น native Compose UI (ไม่มี WebView) อ่าน/เขียน mode + ตารางเวลา และ queue คำสั่งเปิด/ปิดผ่าน Worker; ยังรอ smoke test บนเครื่องจริง
- Firmware v2 ใช้ MQTT/TLS เป็นช่องทาง real-time + HTTPS sync ทุก 15 นาที; ยังรอตรวจ relay/TLS บนฮาร์ดแวร์
- ต้องตรวจ safety (relay active level, maxopen) และ HiveMQ ACL ก่อนใช้งานกับวาล์วจริง

## ข้อตัดสินใจล่าสุด (31 ต.ค. 2026)

- 1 วาล์ว
- ESP32 ใช้ซิม 4G/5G 399 MB/เดือน ไม่ใช้ WiFi → MQTT + HTTPS sync ทุก 15 นาที (ประเมิน ~10–30 MB/เดือน)
- ทำ backend + native UI คู่ขนาน
- Login ด้วย invite code + household password
