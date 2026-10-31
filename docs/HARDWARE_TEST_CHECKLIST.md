# Hardware test checklist (ESP32 + A7672X + relay)

ทำตามลำดับนี้ก่อนต่อวาล์วน้ำจริง **ห้ามต่อน้ำ/ไฟ mains ระหว่างทดสอบ relay**

## 0. เตรียมของ

- ESP32 dev board + สาย USB data
- A7672X/A7672G module หรือบอร์ดที่ต่อ UART2 (RX16/TX17) ได้
- SIM 4G/5G ที่เปิด data แล้ว (APN ถูกต้อง)
- Power supply 5 V ที่จ่าย 2 A peak ได้
- Relay module + มัลติมิเตอร์ (วัด continuity)
- หลอดไฟ 12 V หรือ buzzer แทนวาล์วสำหรับทดสอบแรก
- คอมพิวเตอร์ที่ flash firmware และเปิด serial monitor 115200 ได้

## 1. ประกอบและตรวจไฟ

- [ ] ต่อ GND ของ ESP32, modem, relay, power supply เข้าด้วยกัน
- [ ] A7672X ใช้ 5 V แยก ไม่ดึงจาก 3.3 V ของ ESP32
- [ ] ใส่ bulk capacitor ≥ 470 µF ใกล้ modem
- [ ] วัดไฟ 5 V ขณะ modem transmit (ดูด้วย scope หรือมัลติมิเตอร์) ไม่ตกต่ำกว่า ~4.6 V
- [ ] ยังไม่ต่อวาล์ว/โหลด mains

## 2. Relay polarity (สำคัญที่สุด)

- [ ] Flash firmware โหมด cellular (หรือ Wi-Fi bench) โดยยังไม่ต่อวาล์ว
- [ ] เปิด serial monitor 115200
- [ ] ตอน boot: วัด COM–NO ต้อง **ไม่ต่อวงจร** (valve closed)
- [ ] ส่งคำสั่ง `set`/`sync` หรือ MQTT `open` → relay ต้องเปลี่ยนสถานะ
- [ ] ถ้า inverted: เปลี่ยน `RELAY_ACTIVE_HIGH` เป็น `0`, flash ใหม่, ทดสอบซ้ำ
- [ ] ต่อหลอดไฟ 12 V แทนวาล์ว แล้วทดสอบ open/close ซ้ำ
- [ ] ตรวจว่า relay ปิดเมื่อกด `close`, schedule หมด, หรือ maxopen ครบ

## 3. Cellular + TLS

- [ ] `set apn internet` (หรือ APN ของ carrier), `set apnuser`/`set apnpass` ถ้าต้องใช้
- [ ] `reboot` แล้วดู log: `modem.init`, `GPRS connect`, IP
- [ ] ตรวจ `AT+CCERTLIST` ว่ามี `cfroot.pem`
- [ ] ดู log `TLS: CA upload failed` — ถ้ามี ให้ power-cycle modem แล้วลองใหม่
- [ ] `sync` ต้องได้ `Sync HTTP 200`
- [ ] ถ้า `Sync HTTP 0`: ตรวจ DNS/TLS/CA, ลอง `AT+CSSLCFG="sslversion",0,3`
- [ ] ถ้า `Sync HTTP 401`: device token ผิด/ถูก revoke → `set token dev_...` ใหม่

## 4. MQTT + ACL

- [ ] สร้าง HiveMQ Cloud user สำหรับอุปกรณ์ (ไม่ใช้ user admin)
- [ ] จำกัดสิทธิ์ publish/subscribe เฉพาะ `myhouse/valve/control` และ `myhouse/valve/status`
- [ ] `set mqttuser` / `set mqttpass`
- [ ] ดู log: MQTT connect rc=0
- [ ] rc=4 → username/password ผิด
- [ ] rc=5 → ACL ไม่Allow topic
- [ ] publish `{"cmd":"getStatus"}` ไป `myhouse/valve/control` → ได้ status กลับ
- [ ] publish `{"cmd":"open","commandId":"test-1","expiresAt":"<future>"}` → relay เปิด, status มี `lastCommandId`
- [ ] publish commandId เดิมซ้ำ → ต้องไม่ทำงานซ้ำ (idempotent)

## 5. Worker sync + command queue

- [ ] สร้าง household/device ด้วย `scripts/create-household.ps1` เก็บ `deviceToken` (ไฟล์นอก repo)
- [ ] `set token dev_...`, `set url https://water-valve-api-v2.czqwertyyy.workers.dev`
- [ ] `sync` → D1 `devices.last_seen_at` อัปเดต
- [ ] แก้ mode/schedule ในแอป → `sync` → firmware ได้ revision ใหม่
- [ ] ส่ง command จากแอปตอน MQTT ปิด → รอ sync ถัดไป → firmware ทำงาน + ack
- [ ] ส่ง command ใหม่ก่อนตัวเก่า ack → ตัวเก่าต้อง `expired/superseded`, ตัวใหม่ทำงาน
- [ ] ตรวจ `GET /v1/households/{id}/commands` เห็นสถานะ acked/expired ถูกต้อง

## 6. Safety

- [ ] ตั้ง `set maxopen 1` (1 นาที) เพื่อทดสอบ → เปิดวาล์วแล้วรอ → ต้องปิดเอง + `fault=max_runtime`
- [ ] ตัดเน็ต (ถอดเสาอากาศ/ปิด data) แล้วเปิดวาล์ว → maxopen ต้องยังปิดได้
- [ ] ตัดไฟเลี้ยงแล้วเปิดใหม่ → วาล์วต้อง boot แบบ closed เสมอ
- [ ] ตั้ง schedule ข้ามคืน (เช่น 22:00–06:00) → ตรวจว่าหลังเที่ยงคืนยังเปิดตามวันก่อนหน้า
- [ ] ปล่อยให้ loop ค้าง (ถ้าจำลองได้) → task watchdog 120 วิ reset

## 7. Data budget

- [ ] จด volume ก่อนทดสอบจาก SIM dashboard
- [ ] เปิดระบบ 24 ชั่วโมง (sync 15 นาที + status 5 นาที)
- [ ] วัด volume ที่ใช้ → ประเมินต่อเดือน (เป้า 55–105 MB/เดือน)
- [ ] ถ้าเกิน 150 MB/เดือน ให้เพิ่ม `SYNC_INTERVAL_MS` หรือลด status frequency
- [ ] ตรวจว่า TLS handshake ไม่ retry รัว ๆ (ดู log)

## 8. Android runtime

- [ ] ใช้เครื่องจริงหรือ emulator ที่มี RAM พอ (≥ 4 GB free commit)
- [ ] Login ด้วย invite code + household password
- [ ] เห็น valve/mode/schedules/revision
- [ ] เปลี่ยน mode → เครื่องอื่นเห็นภายใน 15 วิ
- [ ] เพิ่ม/แก้/ลบ schedule → บันทึกสำเร็จ, conflict dialog ทำงานเมื่อแก้พร้อมกัน
- [ ] เปิด/ปิดวาล์ว → เห็น pending แล้ว acked/expired
- [ ] ปิดเน็ต → เห็น cache + ข้อความ "ออฟไลน์ — แสดงข้อมูลล่าสุดเมื่อ..."
- [ ] ปิดแอปแล้วเปิดใหม่ตอนเน็ตหลุด → ยังเห็น snapshot ล่าสุด (cache ข้าม process)
- [ ] เปิด Settings → เห็นบัญชี/บ้าน/อุปกรณ์/เวอร์ชัน/API และ logout มี confirm
- [ ] Settings → เปลี่ยนรหัสบ้าน: รหัสปัจจุบันผิดแสดง error โดยไม่ logout; รหัสถูกต้องสำเร็จ; เครื่องที่สองถูกบังคับ login ใหม่
- [ ] Settings → ออก device token ใหม่: token แสดงครั้งเดียว, คัดลอกได้; sync ด้วย token เก่าได้ 401; provision token ใหม่แล้ว sync ผ่าน
- [ ] ปิด/เปิดแอป → session ยังอยู่ (token ใน Keystore)
- [ ] Logout → token ถูก revoke และกลับหน้า login

## 9. Sign-off

| หัวข้อ | ผ่าน | หมายเหตุ |
|---|---|---|
| Relay polarity | ☐ | |
| Cellular/TLS | ☐ | |
| MQTT/ACL | ☐ | |
| Sync/command/ack | ☐ | |
| Safety/maxopen | ☐ | |
| Data budget | ☐ | |
| Android runtime | ☐ | |

เมื่อผ่านครบแล้วจึงต่อวาล์วน้ำจริง และควรมี cutoff/breaker แยกสำหรับวาล์ว
