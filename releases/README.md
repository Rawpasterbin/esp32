# Water Valve APK releases

> ภาษา: **ไทย** | [English](README.en.md)

## ล่าสุด: v2.1-native (5 พ.ย. 2026)

- **GitHub Release:** <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>
- **ดาวน์โหลดตรง:**
  <https://github.com/Rawpasterbin/esp32/releases/download/v2.1-native/water-valve-2.1-native-release.apk>
- **ไฟล์:** `water-valve-2.1-native-release.apk`
- **Build:** release variant, `versionCode 2`, `versionName 2.1-native`
  (ปิด R8/minify บนเครื่อง build 8 GB; ดู migration plan)
- **Package:** `com.example.esp32`
- **Min SDK:** 24, **target SDK:** 34
- **Signing key SHA-256:**
  `6A:73:42:E7:13:B8:C0:41:9A:59:60:FA:E1:8B:43:0D:8E:AE:FA:68:2C:FB:3F:ED:8B:CD:74:83:D7:2F:2E:7E`
- **APK SHA-256:** `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`

การเปลี่ยนแปลงใน v2.1:

- ปุ่ม **เปิด** แบบ manual ถูกล็อกเมื่อวาล์วเปิดอยู่หรือมีคำสั่งค้าง;
  มี duplicate command guard ใน ViewModel
- ปุ่ม **ปิด** ยังเป็น safety action แต่จะไม่ queue ปิดซ้ำขณะมีคำสั่งปิดค้างอยู่
- Device card แสดง `ออนไลน์ · sync X นาทีที่แล้ว` / `ออฟไลน์ · เห็นล่าสุด
  X นาทีที่แล้ว` ทำให้เห็นชัดว่า online คือ window 20 นาที

### ติดตั้ง

ดาวน์โหลด APK จากโฟลเดอร์นี้ (GitHub: เปิดไฟล์ใน repo แล้วกด **Download**)
หรือ copy จากเครื่องที่ build

```powershell
adb install -r water-valve-2.1-native-release.apk
```

v2.1 ใช้ **signing key เดียวกัน** กับ v2.0 จึงติดตั้งทับ v2.0 ได้โดยไม่ต้องถอน
ถ้ามี debug-signed build ติดตั้งอยู่ ให้ถอนก่อน:

```powershell
adb uninstall com.example.esp32
adb install water-valve-2.1-native-release.apk
```

การถอนจะล้าง session/cache ในเครื่อง; ให้ login ใหม่ด้วย invite code และ
รหัสบ้านปัจจุบัน ส่วน config ของ household/device ยังอยู่ใน Worker/D1

### Signing key

Release keystore และรหัสผ่านเก็บไว้ **นอก repository**:

- `<secure-path>\water-valve-release.keystore`
- `<secure-path>\water-valve-keystore-password.txt`

ควร backup ไว้ APK ในอนาคตต้องเซ็นด้วย key เดิมจึงจะอัปเดตทับได้
ห้าม commit keystore หรือรหัสผ่าน

## ก่อนหน้า: v2.0-native (5 พ.ย. 2026)

- **ไฟล์:** `water-valve-2.0-native-release.apk`
- **Build:** release variant, `versionCode 1`, `versionName 2.0-native`
- **APK SHA-256:**
  `D8F82D055F0A0AB1EE63334133BF2D8154D1C430730B888399AF419EC89AEEAC`

## หมายเหตุ

- MQTT credential และ device token **ไม่** อยู่ใน APK
- แอปคุยกับ `https://water-valve-api-v2.czqwertyyy.workers.dev`
- คำสั่ง countdown/manual จะถูก queue ใน D1 และส่งเมื่อ ESP32 sync
  (ไม่เกิน ~15 นาที) เว้นแต่บอร์ดถูกควบคุมผ่าน MQTT ด้วย
