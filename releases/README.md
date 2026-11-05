# Water Valve APK releases

> ภาษา: **ไทย** | [English](README.en.md)

## v2.1-native

- **ไฟล์:** `water-valve-2.1-native-release.apk`
- **Build:** release variant, `versionCode 2`, `versionName 2.1-native`
- **Package:** `com.example.esp32`
- **Min SDK:** 24, **target SDK:** 34
- **APK SHA-256:** `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
- **Signing key SHA-256:** `6A:73:42:E7:13:B8:C0:41:9A:59:60:FA:E1:8B:43:0D:8E:AE:FA:68:2C:FB:3F:ED:8B:CD:74:83:D7:2F:2E:7E`
- **GitHub Release:** <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>

### ติดตั้ง

```powershell
adb install -r water-valve-2.1-native-release.apk
```

ถ้ามี build ที่ลงนามด้วย debug key อยู่ ต้องถอนก่อน:

```powershell
adb uninstall com.example.esp32
adb install water-valve-2.1-native-release.apk
```

การถอนจะล้าง session และ cache ในเครื่อง ต้อง login ใหม่ด้วย invite code และรหัสบ้านปัจจุบัน ข้อมูล household/device ยังอยู่ใน Worker/D1

### Signing key

Release keystore และรหัสผ่านเก็บไว้นอก repository ต้องสำรองไว้ เพราะ APK เวอร์ชันถัดไปต้องใช้ key เดิมจึงจะอัปเดตทับได้ ห้าม commit keystore หรือรหัสผ่าน

### หมายเหตุ

- APK ไม่มี MQTT credential หรือ device token
- คำสั่ง manual ถูก queue ใน D1 และส่งถึงอุปกรณ์ในรอบ sync ถัดไป (ไม่เกิน ~15 นาที)
- แอปพลิเคชันสื่อสารกับ Worker ที่กำหนดตอน build
