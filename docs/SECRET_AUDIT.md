# Secret audit (5 Nov 2026)

การตรวจทั้ง 2 repo: `appesp32` (backend/firmware) และ Android project
โดยสแกน **working tree** และ **git history** สำหรับ token, password,
private key, device token, session token, setup token, invite code,
อีเมล และ path ที่มีชื่อผู้ใช้ Windows

> เอกสารนี้ **ไม่บันทึกค่ารหัสจริง** — ระบุเฉพาะประเภท/ตำแหน่งที่พบ
>
> ไฟล์ local ที่ถูก gitignore (เช่น `.dev.vars`, `wrangler.v2.toml`) และไฟล์
> secret ที่เก็บไว้นอกรีโป **ยังมีค่าจริงตามปกติ** — การ scrub นี้เอาค่าเหล่านั้น
> ออกจากไฟล์ที่ commit ขึ้น GitHub เท่านั้น

## 1. สรุป

| รายการ | สถานะ |
|---|---|
| Working tree (tracked files) | ✅ ไม่พบ secret จริง (เหลือแค่ test fixture `definitely-wrong` ใน `scripts/test-api.ps1` และชื่อตัวแปรใน code) |
| Device token / household password / invite code | ✅ ไม่พบใน git history ของทั้ง 2 repo |
| Setup token | ✅ ไม่พบใน git history |
| GitHub PAT | ✅ ไม่พบใน repo (แต่ถูกส่งในแชต — ต้อง revoke) |
| MQTT credential เก่า | ❌ ยังอยู่ใน git history หลาย commit |
| WiFi SSID/password | ❌ อยู่ใน git history ของ firmware เก่า (`9e42949`) |
| MQTT password ใหม่ | ❌ **ตรงกับรหัส WiFi บ้าน** และรหัส WiFi อยู่ใน history → ถือว่ารั่ว |
| ข้อมูลระบุฮาร์ดแวร์/ตัวตนใน working tree | ✅ redact เป็น placeholder แล้ว (ดู §2.1) |

## 2. สิ่งที่พบใน git history (ไม่แสดงค่า)

### Backend / firmware repo

- `9e42949` — `esp32_water_valve/esp32_water_valve.ino` มี `const char* ssid`
  และ `const char* password` ของ WiFi แบบ hardcode (บรรทัด 7–8)
- `81f3251`, `379898f`, `31fb842` — `wrangler.toml` / docs มี `MQTT_USER`
  และ `MQTT_PASS` เก่า (`test` + รหัสผ่านเดิม)
- `a02bb9b` — firmware v2 ยังอ้างถึง credential เก่าในประวัติ
- `86c0564` — `docs/MQTT_SETUP.md` ในประวัติมีรหัสผ่าน MQTT เก่า
  (ปัจจุบัน redact ใน working tree แล้ว แต่ยังอยู่ใน commit)

### Android repo

- `23c0519` (baseline) และ `eecbd98` (ลบ MQTT client) —
  `MqttManager.kt` / `CommandProcessor.kt` เคยมี MQTT username/password
  hardcode (`test` + รหัสผ่านเดิม) ในประวัติ

### สิ่งที่ตรวจแล้วไม่พบ

- invite code จริง, household password จริง, device token จริง,
  setup token จริง: ไม่พบใน history ของทั้ง 2 repo
- GitHub PAT: ไม่พบใน repo (พบเฉพาะในแชต/ประวัติการสนทนา)

## 2.1 ข้อมูลที่ redact ออกจาก working tree แล้ว

ข้อมูลต่อไปนี้ไม่ใช่ credential แต่เป็นรายละเอียดเฉพาะเครื่อง/ deployment
จึงเปลี่ยนเป็น placeholder เพื่อไม่ให้ repo public เปิดเผยโครงสร้างจริง:

| ข้อมูล | ตำแหน่งเดิม | ตำแหน่งใหม่ |
|---|---|---|
| HiveMQ cluster host | firmware default, `wrangler.toml`, docs | `<YOUR_HIVEMQ_CLUSTER>` / `<hivemq-cluster-host>` |
| WiFi SSID ของ tester | `docs/ANDROID_RUNTIME_TEST.md` | "WiFi ของ tester" |
| COM port ของบอร์ด | README, docs | "พอร์ต serial" |
| ชื่อรุ่นมือถือที่ใช้ทดสอบ | `docs/MIGRATION_PLAN.md` | "Android phone" |
| Legacy web UI URL | `docs/PROJECT_ANALYSIS.md`, `docs/MIGRATION_PLAN.md`, `index.html` | `<legacy-web-ui-url>` |
| Legacy Worker URL | `index.html` | `<legacy-worker-url>` |
| KV namespace id | `wrangler.toml` | `<YOUR_KV_NAMESPACE_ID>` |
| D1 database id | `docs/ARCHITECTURE_CENTRAL_CONFIG.md`, `docs/RUNBOOK.md`, `wrangler.v2.toml` | เอกสารอ้างอิง `wrangler.v2.toml`; config จริงย้ายออกจาก git |

หมายเหตุ: `database_id` ของ D1 **ไม่ถูก commit** แล้ว — เก็บใน `wrangler.v2.toml`
ที่เป็นไฟล์ local (git-ignored) โดย commit เฉพาะ `wrangler.v2.toml.example`
ที่มี placeholder (เข้าถึง D1 ไม่ได้หากไม่มี Cloudflare API token อยู่แล้ว
แต่การแยกไฟล์ช่วยไม่ให้เปิดเผย resource id ของบัญชี)

### ข้อมูล infra อื่นใน history (ไม่ใช่ credential)

- HiveMQ cluster host, D1 database id, legacy Pages URL และ COM port/SSID
  ปรากฏใน commit เก่าบาง commit
- ข้อมูลเหล่านี้ไม่ทำให้เข้าถึงระบบได้หากไม่มี credential/API token
  จึงไม่จำเป็นต้อง rotate; จะหายก็ต่อเมื่อ rewrite history (§4)

## 3. ต้องทำทันที

1. **เปลี่ยนรหัส WiFi ของบ้าน** — เพราะรหัส WiFi อยู่ใน public git history
   และถูกนำมาใช้เป็นรหัส MQTT ใหม่
2. **เปลี่ยนรหัส MQTT ของ user อุปกรณ์** ที่ HiveMQ console
   - ห้ามใช้รหัส WiFi, รหัสเดิมใน git history, หรือรหัสซ้ำกับบริการอื่น
   - ตั้งรหัสเฉพาะสำหรับ MQTT ความยาว ≥ 20 ตัวอักษร
3. อัปเดตบอร์ด:
   ```
   set pass <wifi-password-ใหม่>
   set mqttpass <mqtt-password-ใหม่>
   reboot
   ```
4. **ลบ/disable HiveMQ user เก่า** ที่เคยใช้ credential รั่ว
5. **Revoke GitHub PAT** ทั้งสองตัวที่เคยส่งในแชต
6. ถ้าเปลี่ยน WiFi แล้วบอร์ดออฟไลน์ ให้ provision ใหม่ผ่าน serial

## 4. การล้าง git history (optional แต่แนะนำถ้า repo public)

การลบ secret ออกจาก working tree **ไม่ลบออกจาก history** การ rewrite history
จะเปลี่ยน commit hash ทั้งหมดและต้อง force-push:

```powershell
# backup ก่อน
git bundle create <backup-dir>\appesp32-backup.bundle --all

# ติดตั้ง git-filter-repo แล้วสร้าง replacements.txt
# ใส่ค่าที่ต้องการ redact (ห้าม commit ไฟล์นี้)
# <old-secret>==>REDACTED

git filter-repo --replace-text replacements.txt
git push --force --all
git push --force --tags
```

**ข้อควรระวัง:**
- คนที่ clone ไว้ต้อง clone ใหม่
- GitHub Release/tag อาจต้องสร้างใหม่
- Secret ที่เคย public แล้วยังถือว่ารั่ว → ต้อง rotate เสมอ
- Android repo ไม่มี remote; rewrite ได้ในเครื่องแต่ควร backup

## 5. Scanner สำหรับอนาคต

ใช้ `scripts/scan-secrets.ps1` ก่อน commit/push:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/scan-secrets.ps1
```

Scanner ตรวจเฉพาะ working tree และจะ fail ถ้าพบ pattern ที่เสี่ยง
(ไม่ได้ตรวจ history; ใช้ `git log -S` หรือ `git filter-repo` สำหรับ history)

## 6. ไฟล์ secret ที่อยู่นอก repo

ไฟล์ต่อไปนี้เป็น secret และ **ห้าม commit**:

- `<secret-dir>\water-valve-household.json` — invite code/device token
- `<secret-dir>\water-valve-household-password.txt` — รหัสบ้าน
- `<secret-dir>\water-valve-setup-token.txt` — setup token
- `<secret-dir>\hivemq-device.txt` — MQTT credential
- `<secret-dir>\water-valve-release.keystore` — signing key
- `<secret-dir>\water-valve-keystore-password.txt` — keystore password
- `.dev.vars` — local Worker setup token (git-ignored)

ควรเก็บใน password manager และลบไฟล์ที่ไม่จำเป็นหลัง provisioning
