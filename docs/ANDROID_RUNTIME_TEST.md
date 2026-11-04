# Android runtime test report

- **วันที่:** 31 ตุลาคม 2026
- **เครื่องที่ทดสอบ:** Infinix X6815B, Android 12 (API 31), 1080×2460
- **APK:** `water-valve-2.0-native-debug.apk` (debug signed)
  - SHA-256: `5FD52B5F42CB575AFC8A2C0D0BC9F985C6287E16C99910BEF5AB0C0FA5C30B24`
- **Backend:** production Worker `https://water-valve-api-v2.czqwertyyy.workers.dev`
- **Household:** `Home`
- **วิธีทดสอบ:** ติดตั้ง APK ผ่าน `adb install -r`, ควบคุม UI ด้วย `adb shell input` + `uiautomator dump`, เก็บ screenshot ทุกขั้นตอน
- **ข้อจำกัด:** ยังไม่ได้ต่อ ESP32 จริง — การรับคำสั่ง/ack จำลองด้วย device token จริงผ่าน `POST /v1/devices/sync` และ `/v1/devices/commands/{id}/ack`

## 1. ผลทดสอบ

| # | กรณีทดสอบ | ผล | หมายเหตุ |
|---|---|---|---|
| 1 | Login ด้วย invite code + รหัสบ้าน | ผ่าน | เห็น dashboard, mode, schedule, valve |
| 2 | ชื่ออุปกรณ์ภาษาไทย | ผ่านหลังแก้ | เดิมเป็น mojibake จาก PowerShell encoding |
| 3 | เปลี่ยน mode manual → auto → manual | ผ่าน | มี banner revision ใหม่, ปุ่มเปิดถูก disable ใน auto |
| 4 | เพิ่ม schedule แล้วบันทึก | ผ่าน | revision 10 |
| 5 | ลบ schedule แล้วบันทึก | ผ่าน | revision 11, กลับเหลือ 3 รอบ |
| 6 | เปิดวาล์ว (confirm dialog) | ผ่าน | pending → acked, valve card เปลี่ยนเป็นเปิด |
| 7 | ปิดวาล์ว | ผ่าน | acked, valve card เปลี่ยนเป็นปิด |
| 8 | คำสั่งที่ ack หลัง 60 วิ | ผ่าน | การ์ดอัปเดตเป็น acked เองโดยไม่ต้อง refresh |
| 9 | Conflict ตอนมีคนอื่นแก้ config | ผ่าน | dialog แจ้ง revision 15 + โหลด state ใหม่ |
| 10 | ปิดเน็ต + force-stop + เปิดแอปใหม่ | ผ่าน | เห็น cache + "ออฟไลน์ — แสดงข้อมูลล่าสุดเมื่อ..." |
| 11 | กลับมาออนไลน์ + refresh | ผ่าน | banner หาย, ข้อมูลอัปเดต |
| 12 | Settings: บัญชี/บ้าน/อุปกรณ์/แอป | ผ่าน | ข้อมูลตรงกับ D1 |
| 13 | เปลี่ยนรหัสบ้านด้วยรหัสปัจจุบันผิด | ผ่าน | แสดง error โดยไม่ logout |
| 14 | เปลี่ยนรหัสบ้านสำเร็จ | ผ่าน | dialog ยืนยัน, session ที่ใช้อยู่ยัง valid |
| 15 | Logout + confirm | ผ่าน | กลับหน้า login โดยไม่ขึ้น "เซสชันหมดอายุ" |
| 16 | Login ใหม่ด้วยรหัสที่เปลี่ยน | ผ่าน | เข้า dashboard ได้ |
| 17 | ปิด/เปิดแอปแล้ว session ยังอยู่ | ผ่าน | ทดสอบผ่าน `adb install -r` + relaunch |
| 18 | Rotate device token จาก Settings | ยังไม่ทดสอบ | จะทำให้ token เดิมใช้ไม่ได้; API suite ครอบ endpoint แล้ว |

## 2. บั๊กที่พบและแก้แล้ว

1. **ชื่ออุปกรณ์/ชื่อรอบภาษาไทยเพี้ยน (mojibake)**
   - สาเหตุ: Windows PowerShell 5 `Invoke-RestMethod -Body <string>` ส่งเป็น ISO-8859-1 ทั้งที่ `Content-Type` เป็น UTF-8
   - แก้: `scripts/create-household.ps1` ส่ง `[Text.Encoding]::UTF8.GetBytes($payload)`; แก้ข้อมูล device name ใน D1
   - หมายเหตุ: การทดสอบ conflict ด้วย PowerShell ก็ทำให้ชื่อรอบเพี้ยน จึง restore `schedules_json` ด้วย SQL ไฟล์ UTF-8
2. **ขึ้น "เซสชันหมดอายุ" หลังกด logout เอง**
   - สาเหตุ: คำขอ refresh ที่ค้างอยู่ได้ 401 หลัง token ถูก revoke แล้วเรียก `handleUnauthorized`
   - แก้: เพิ่ม `manualLogout` flag; logout เองล้างข้อความ login และไม่แสดง error
3. **ข้อความ Settings บอกว่า sync ทันทีเมื่อต่อ MQTT**
   - แก้เป็น "อุปกรณ์ดึงไปใช้ในรอบ sync (ไม่เกิน 15 นาที)" เพราะแอปยังไม่ publish MQTT
4. **ETag ของ `/state` ไม่ครอบสถานะอุปกรณ์**
   - แก้ให้ ETag มี `lastSeenAt` + time bucket 1 นาที; ทดสอบว่า state เปลี่ยนหลัง device sync
5. **คำสั่ง ack หลัง 60 วิไม่อัปเดตการ์ด**
   - เพิ่มการเช็ค command ที่ pending ทุกครั้งที่ refresh state
6. **Schedule เกิน 8 รอบถูก firmware ตัดเงียบ ๆ**
   - Worker ปฏิเสธ >8, แอปแสดง "(n/8)" และ disable ปุ่มเพิ่ม
7. **สั่ง close ใน auto แล้ว firmware เปิดซ้ำ**
   - firmware latch `schedSuppress` ข้าม reboot จนจบ window (ยังต้องทดสอบบนฮาร์ดแวร์)

## 3. สิ่งที่ยังต้องทดสอบบนฮาร์ดแวร์จริง

- Relay active level และการต่อวาล์ว
- A7672X TLS/CA upload บนเครือข่ายมือถือ
- MQTT ACL และการรับคำสั่ง real-time
- max runtime / watchdog / power stability
- Data budget จริงบนซิม 399 MB/เดือน
- Manual close ใน auto ระหว่าง schedule active (firmware latch)

## 4. ไฟล์หลักฐาน

Screenshots อยู่ใน `C:\Temp\opencode\android-*.png` (นอก repo) ตั้งแต่
`android-01-login.png` ถึง `android-41-final-dashboard.png`

## 5. Board follow-up (5 Nov 2026)

หลังทดสอบ app ได้ต่อบอร์ด ESP32 จริง (COM15, WiFi bench build v2.1.0):

- แฟลช firmware v2.1.0 (WiFi bench) สำเร็จ; NVS เดิมไม่ถูก erase
- WiFi `noonit_2.4G` เชื่อมได้; DNS/TLS ผ่านหลังแก้ CA เป็น GlobalSign Root CA (RSA)
- `Sync HTTP 200`; บอร์ดได้ revision 16, 3 schedules, โหมด manual
- ส่งคำสั่ง `close` ผ่าน API แล้วสั่ง `sync` ผ่าน serial → บอร์ด ack 200
- MQTT ยังไม่เชื่อม เพราะรอ HiveMQ device user ใหม่ (credential เก่ารั่ว)

