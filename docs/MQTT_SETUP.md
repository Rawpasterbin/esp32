# MQTT setup (HiveMQ Cloud)

บอร์ด ESP32 เชื่อม Worker/D1 ผ่าน HTTPS ได้แล้ว แต่ MQTT ยังต้องมี credential
ของอุปกรณ์ที่ **ไม่ใช่ credential เก่าที่รั่วใน git history**

## 1. สร้าง device user ที่ HiveMQ Cloud

1. เข้า HiveMQ Cloud console ของ cluster:
   `<YOUR_HIVEMQ_CLUSTER>.s1.eu.hivemq.cloud`
2. ไปที่ **Access Management → Users**
3. สร้าง user ใหม่สำหรับอุปกรณ์ เช่น `valve-home` พร้อมรหัสผ่านยาว ๆ
   (ไม่ใช้ user admin และไม่ใช้ user เก่า)
4. ตั้ง permission/ACL ให้เฉพาะ topic ของบ้าน:
   - publish + subscribe: `myhouse/valve/control`
   - subscribe: `myhouse/valve/mode`, `myhouse/valve/schedule`
   - publish: `myhouse/valve/status`
   - หรือใช้ wildcard `myhouse/valve/#` ทั้ง publish/subscribe สำหรับ v1
5. บันทึก username/password

## 2. ใส่ credential ลงไฟล์ (ไม่ commit)

เก็บ credential ไว้ในไฟล์นอกรีโป (ห้าม commit) แล้วเขียนค่าลงไฟล์นั้น:

```
<username>
<password>
```

จากนั้นแจ้งผู้ช่วยเพื่อ provision ผ่าน serial:

```
set mqttuser <username>
set mqttpass <password>
reboot
```

## 3. ตรวจสอบ

- serial log ต้องเห็น `MQTT connected` (rc=0)
- ถ้า rc=4 → username/password ผิด
- ถ้า rc=5 → ACL ไม่อนุญาต topic
- ทดสอบ publish `{"cmd":"getStatus"}` ไป `myhouse/valve/control`
  แล้วต้องได้ status กลับมาที่ `myhouse/valve/status`
- ห้าม publish `open`/`close` จนกว่าจะตรวจ relay polarity และวาล์วจริง

## 4. หลังทดสอบ

- เก็บ credential ใหม่ไว้ใน password manager
- ลบไฟล์ `hivemq-device.txt` หลัง provision
- ห้ามใช้ credential เก่าที่อยู่ใน git history (user เก่าและรหัสผ่านเดิม)
- ปิด/ลบ user เก่าใน HiveMQ console ด้วย

## 5. ผลทดสอบจริง (5 พ.ย. 2026)

- `CONNACK 0` ทั้งจาก PC และ ESP32
- `MQTT connected` หลัง reboot
- subscribe `myhouse/valve/status` + publish `{"cmd":"getStatus"}`
  → ได้ status กลับ (`valve=false`, `mode=manual`, `rssi≈-37`, `fw=2.1.0`)
- publish `{"cmd":"close","commandId":"mqtt-close-...","expiresAt":"..."}`
  → status มี `lastCommandId` ตรงกัน, valve ยังปิด
- ยังไม่ทดสอบ `open` จนกว่าจะตรวจ relay polarity/วาล์วจริง
- **ข้อควรระวัง:** ถ้าคัดลอก username มาแบบมี `< >` เช่น `<your-user>`
  broker จะตอบ `CONNACK 5 / 0x87`; ต้องใช้ username ที่ไม่มี `< >` เท่านั้น
