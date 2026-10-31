# Production runbook

คู่มือปฏิบัติการสำหรับ Worker v2 + D1 (`water-valve-api-v2`) และอุปกรณ์ ESP32
ใช้เมื่อ deploy, ตรวจสอบ, สำรองข้อมูล, หมุน secret หรือแก้เหตุขัดข้อง

> ค่าที่เป็น secret (setup token, device token, household password, MQTT password)
> ห้าม commit ลง repo และห้ามวางใน ticket/แชตสาธารณะ

## 1. ทรัพยากร production

| รายการ | ค่า |
|---|---|
| Worker | `water-valve-api-v2` |
| URL | `https://water-valve-api-v2.czqwertyyy.workers.dev` |
| D1 | `water-valve-v2` (`7ed1f52f-759e-4edd-a936-847c7277b824`, APAC) |
| Config | `wrangler.v2.toml` |
| Setup token | Worker secret `SETUP_TOKEN`; สำเนา local `C:\Temp\opencode\water-valve-setup-token.txt` |
| สถานะปัจจุบัน | ยังไม่มี household/device จริง (สร้างด้วย `scripts/create-household.ps1`) |

## 2. Health check

```powershell
Invoke-RestMethod https://water-valve-api-v2.czqwertyyy.workers.dev/health
# { ok: true, data: { version: "2.0.0", time: "..." } }
```

ถ้า health ไม่ผ่าน ให้ดู `npx wrangler tail --config wrangler.v2.toml` ก่อนแก้โค้ด

## 3. Deploy / อัปเดต

```powershell
# 1) migration ใหม่ (ถ้ามี) — ต้องรันก่อน deploy
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql

# 2) secret (ครั้งแรกหรือเมื่อ rotate)
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml

# 3) deploy
npx wrangler deploy --config wrangler.v2.toml
```

หลัง deploy ให้รัน API suite (ต้องตั้ง `API_BASE` + `SETUP_TOKEN`):

```powershell
$env:API_BASE = 'https://water-valve-api-v2.czqwertyyy.workers.dev'
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

> suite จะสร้าง `Test House` / `Other House` และจบด้วยการทำ rate limit ของ IP ตัวเองติด
> ต้องลบข้อมูลทดสอบและ `auth_attempts` หลังรัน (ดู §9) ก่อนรันซ้ำภายใน 1 นาที

## 4. Backup

### 4.1 Export รายวัน/ก่อนเปลี่ยนแปลงสำคัญ

```powershell
$stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$out = "C:\Temp\opencode\water-valve-v2-$stamp.sql"
npx wrangler d1 export water-valve-v2 --config wrangler.v2.toml --remote --output $out
Get-Item $out | Select-Object Name, Length
```

- ไฟล์มีทั้ง schema และข้อมูล (รวม password hash/token hash — ไม่ใช่ plaintext แต่ยังเป็นข้อมูลอ่อนไหว)
- เก็บนอก repo, จำกัด ACL, ลบเมื่อไม่จำเป็น
- ตรวจว่าไฟล์ไม่ว่างและมี `CREATE TABLE auth_attempts` (หลัง migration 0002)

### 4.2 D1 Time Travel (แนะนำสำหรับกู้ทั้งฐาน)

Cloudflare D1 มี Time Travel สำหรับย้อนฐานข้อมูลในหน้าต่างที่กำหนด ใช้เมื่อข้อมูลเสียหายทั้งชุด
รายละเอียดคำสั่งดู `npx wrangler d1 time-travel --help` เวอร์ชันที่ใช้

### 4.3 ทดสอบ restore บน local

```powershell
# ล้าง local state เฉพาะเครื่อง dev (ไม่กระทบ production)
Remove-Item -Recurse -Force .wrangler\state -ErrorAction SilentlyContinue
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=C:\Temp\opencode\water-valve-v2-YYYYMMDD-HHmm.sql
```

ยืนยันว่าตารางและจำนวนแถวตรงกับที่ export มา ก่อนเชื่อถือ backup

> **ห้าม** restore ทับ production โดยไม่ตรวจ backup และไม่มี backup ซ้ำ —
> ใช้เฉพาะเมื่อฐานเสียหายและยอมรับการย้อนข้อมูลได้

## 5. หมุน device token

ทำได้จากแอป (Settings → อุปกรณ์ → ออก device token ใหม่) หรือ API:

```powershell
$token = '<member session token>'
$hid = '<household id>'
$did = '<device id>'
Invoke-RestMethod -Method Post -Headers @{ Authorization = "Bearer $token" } `
  -Uri "https://water-valve-api-v2.czqwertyyy.workers.dev/v1/households/$hid/devices/$did/rotate-token"
```

- plaintext token ใหม่จะแสดงครั้งเดียว — คัดลอกทันที
- token เก่าใช้ไม่ได้ทันที; firmware จะ sync ไม่ผ่าน (401) จนกว่าจะ provision ใหม่
- provision ผ่าน serial 115200: `set token dev_...` แล้ว `reboot`
- ตรวจ `sync` ว่าได้ HTTP 200

## 6. เปลี่ยนรหัสบ้าน

ทำได้จากแอป (Settings → ความปลอดภัย → เปลี่ยนรหัสบ้าน) หรือ API:

```powershell
Invoke-RestMethod -Method Post -Headers @{ Authorization = "Bearer $token" } `
  -ContentType 'application/json' `
  -Uri "https://water-valve-api-v2.czqwertyyy.workers.dev/v1/households/$hid/password" `
  -Body (@{ currentPassword = '...'; newPassword = '...' } | ConvertTo-Json)
```

- รหัสใหม่ต้อง ≥ 8 ตัวอักษรและต่างจากรหัสเดิม
- session อื่นทั้งหมดของบ้านถูก revoke; session ที่เรียกยังใช้ได้
- รหัสปัจจุบันผิดได้ `401 INVALID_PASSWORD` (ไม่ทำให้ session หมดอายุ)

## 7. หมุน setup token

ทำเมื่อสงสัยว่า token รั่ว หรือหลัง provisioning เสร็จ:

```powershell
# สร้างค่าใหม่ (อย่างน้อย 32 hex)
$new = -join ((1..64) | ForEach-Object { '{0:x}' -f (Get-Random -Max 16) })
$new | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
$new | Set-Content C:\Temp\opencode\water-valve-setup-token.txt -Encoding ascii
```

- `/v1/setup` ยังใช้ token นี้สร้าง household เพิ่มได้ — หมุนหลัง Provisioning
- ถ้าไม่ต้องการให้สร้าง household เพิ่มอีก ให้ลบ secret หรือเปลี่ยน Worker ให้ปิด setup หลังมี household แรก

## 8. ลบ household (ถอนข้อมูล)

สำรองก่อนทุกครั้ง แล้วลบตามลำดับ FK:

```sql
DELETE FROM audit_log WHERE household_id = '<hid>';
DELETE FROM commands WHERE household_id = '<hid>';
DELETE FROM sessions WHERE member_id IN (SELECT id FROM members WHERE household_id = '<hid>');
DELETE FROM configs WHERE household_id = '<hid>';
DELETE FROM devices WHERE household_id = '<hid>';
DELETE FROM members WHERE household_id = '<hid>';
DELETE FROM households WHERE id = '<hid>';
```

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --command "<SQL>"
```

หลังลบ ให้ตรวจ `SELECT COUNT(*) FROM households;`

## 9. ล้างข้อมูลทดสอบ / rate limit

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --command `
  "DELETE FROM auth_attempts;"
```

- `auth_attempts` เก็บเฉพาะ failed login ล่าสุด; ถ้าไม่มีใคร login ผิดก็ปล่อยไว้ได้
- ข้อมูลทดสอบ (`Test House`, `Other House`) ให้ลบด้วย §8

## 10. วินิจฉัยปัญหา

### อุปกรณ์ขึ้น offline

1. ตรวจ `devices.last_seen_at` — Worker ถือว่า online ภายใน **20 นาที** (sync ทุก 15 นาที)
2. ดู log firmware: `modem.init`, GPRS, `Sync HTTP ...`
3. `Sync HTTP 401` → device token ผิด/ถูก rotate → §5
4. `Sync HTTP 0` → DNS/TLS/CA/APN; ตรวจ `AT+CCERTLIST`
5. ถ้า sync ผ่านแต่แอปยังเก่า → ตรวจ ETag/cache ฝั่งแอป, กด refresh

### คำสั่งค้าง pending

- TTL 20 นาที; หลังหมดเวลา `GET /commands` จะ mark เป็น `expired` + `error='ttl'`
- คำสั่งใหม่จะ supersede คำสั่งเก่า (`error='superseded'`)
- ถ้าอุปกรณ์ออฟไลน์นานกว่า TTL คำสั่งจะหมดอายุก่อนถึงอุปกรณ์ — ต้องส่งใหม่

### Login ขึ้น 429

- เกิดจาก failed attempts 5 ครั้งใน 1 นาทีจาก IP เดียวกัน
- รอ 1 นาที หรือลบ `auth_attempts` ตาม §9 (ระวัง: เป็นการปลดล็อก IP ทั้งหมด)

### Worker 500

```powershell
npx wrangler tail water-valve-api-v2 --config wrangler.v2.toml
```

ตรวจ D1 binding, migration ที่ยังไม่รัน, หรือ JSON body ผิดรูปแบบ

## 11. Incident: secret รั่ว

1. **Device token**: rotate ทันที (§5) แล้ว provision firmware ใหม่
2. **Household password**: เปลี่ยนทันที (§6) — session อื่นถูก revoke
3. **Setup token**: rotate (§7)
4. **MQTT credential**: rotate ที่ HiveMQ console แล้ว `set mqttuser`/`set mqttpass` (ดู `docs/SECURITY.md`)
5. ตรวจ `audit_log` และ `sessions` เพื่อดูการใช้งานผิดปกติ
6. บันทึกเหตุการณ์และสาเหตุในเอกสารภายใน

## 12. Data retention (ทำเมื่อมีข้อมูลจริง)

ยังไม่มี cron trigger; ลบข้อมูลเก่าด้วยมือเป็นระยะ:

```sql
-- commands เก่ากว่า 90 วัน
DELETE FROM commands WHERE created_at < datetime('now', '-90 days');
-- audit เก่ากว่า 180 วัน (เก็บเฉพาะที่จำเป็น)
DELETE FROM audit_log WHERE created_at < datetime('now', '-180 days');
-- sessions ที่หมดอายุ/ถูก revoke เกิน 30 วัน
DELETE FROM sessions WHERE expires_at < datetime('now', '-30 days');
```

ก่อนลบ ควร export backup และตรวจว่ามี audit ที่ต้องเก็บตามข้อกำหนดหรือไม่
