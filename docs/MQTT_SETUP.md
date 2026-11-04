# MQTT setup (HiveMQ Cloud)

บอร์ด ESP32 เชื่อม Worker/D1 ผ่าน HTTPS ได้แล้ว แต่ MQTT ยังต้องมี credential
ของอุปกรณ์ที่ **ไม่ใช่ credential เก่าที่รั่วใน git history**

## 1. สร้าง device user ที่ HiveMQ Cloud

1. เข้า HiveMQ Cloud console ของ cluster:
   `74bd39d864644eebb6efba20dadd4e93.s1.eu.hivemq.cloud`
2. ไปที่ **Access Management → Users**
3. สร้าง user ใหม่สำหรับอุปกรณ์ เช่น `valve-home` พร้อมรหัสผ่านยาว ๆ
   (ไม่ใช้ user admin และไม่ใช้ `test`)
4. ตั้ง permission/ACL ให้เฉพาะ topic ของบ้าน:
   - publish + subscribe: `myhouse/valve/control`
   - subscribe: `myhouse/valve/mode`, `myhouse/valve/schedule`
   - publish: `myhouse/valve/status`
   - หรือใช้ wildcard `myhouse/valve/#` ทั้ง publish/subscribe สำหรับ v1
5. บันทึก username/password

## 2. ใส่ credential ลงไฟล์ (ไม่ commit)

แก้ไฟล์ `C:\Temp\opencode\hivemq-device.txt`:

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
- ห้ามใช้ credential เก่า `test`/`test1234` ที่อยู่ใน git history
