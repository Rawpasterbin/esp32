# Security notes

## Incident: legacy MQTT credential in git history

`wrangler.toml` เคยมี `MQTT_USER` / `MQTT_PASS` ของ HiveMQ แบบ hardcode และไฟล์นี้
ถูก push ไปที่ GitHub สาธารณะแล้ว (`Rawpasterbin/esp32`) ก่อนถูกแก้ในรอบนี้

**ถือว่า credential นั้นรั่วและใช้งานไม่ได้อีก** การลบออกจาก commit ล่าสุดไม่เพียงพอ
เพราะยังอยู่ใน git history และอาจถูก fork/cache ไปแล้ว

### สิ่งที่ต้องทำ (เรียงลำดับ)

1. เข้า HiveMQ Cloud console → Access Management
2. ลบ/disable user เก่า (`test`) หรือเปลี่ยน password ทันที
3. สร้าง user ใหม่สำหรับอุปกรณ์โดยเฉพาะ (ไม่ใช้ user admin) และจำกัด ACL:
   - publish/subscribe `myhouse/valve/control`
   - publish `myhouse/valve/status`
   - subscribe `myhouse/valve/mode`, `myhouse/valve/schedule` (ถ้ายังใช้ legacy)
4. ตั้ง secret ฝั่ง Worker (ถ้ายังใช้ Worker เก่า):
   ```powershell
   npx wrangler secret put MQTT_USER --config wrangler.toml
   npx wrangler secret put MQTT_PASS --config wrangler.toml
   ```
5. อัปเดต firmware ผ่าน serial 115200:
   ```
   set mqttuser <new-user>
   set mqttpass <new-password>
   reboot
   ```
6. ตรวจว่า MQTT connect rc=0 และ publish/subscribe ทำงานตาม ACL
7. (optional) ตรวจ GitHub secret scanning / audit log ของ HiveMQ

### การล้าง git history (optional, หลัง rotate แล้วเท่านั้น)

ถ้าต้องการลบข้อความ secret ออกจาก history ทั้งหมด ต้องใช้ `git filter-repo` และ
force-push ซึ่งจะเปลี่ยน commit hash ทั้ง repo:

```powershell
# ติดตั้ง git-filter-repo ก่อน แล้วรันใน repo ที่ backup ไว้
@'
test1234==>REDACTED
'@ | Set-Content replacements.txt
git filter-repo --replace-text replacements.txt
git push --force --all
git push --force --tags
```

**สำคัญ:** rotation เป็นสิ่งจำเป็น การ rewrite history ไม่ได้ทำให้ secret ที่รั่วแล้ว
กลับมาปลอดภัย

## Secret handling rules

- ห้าม commit `.dev.vars`, `wrangler.toml` secret, device token, household password,
  APN password หรือ MQTT password
- `SETUP_TOKEN` เก็บเป็น Worker secret:
  ```powershell
  npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
  ```
- Device token เก็บเฉพาะ hash (SHA-256) ใน D1; token จริงอยู่ใน firmware NVS
- Member session token เก็บเฉพาะ hash ใน D1; ฝั่ง Android เข้ารหัสด้วย Android Keystore (AES-GCM)
- ห้าม log token/password; firmware `show` แสดงเฉพาะค่า mask
- ถ้า token รั่ว: ใช้ `POST /v1/households/:hid/devices/:did/rotate-token` ออก device token ใหม่
  (plaintext คืนครั้งเดียว) แล้ว provision firmware ผ่าน serial; session ที่สงสัยให้ logout/เปลี่ยนรหัสบ้าน

## Current status

- [x] ลบ MQTT credential ออกจาก `wrangler.toml` และเอกสารใน working tree
- [x] เพิ่ม endpoint เปลี่ยนรหัสบ้าน + rotate device token (revoke session อื่นอัตโนมัติ)
- [x] Rate limit login/join/password change (5 failed attempts/min/IP; ตาราง `auth_attempts`)
- [ ] Rotate HiveMQ credential (ต้องทำที่ console — ยังไม่ได้ทำ)
- [ ] อัปเดต firmware ที่ติดตั้งจริงด้วย credential ใหม่
- [ ] (optional) ล้าง git history และ force-push
