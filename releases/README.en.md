# Water Valve APK releases

> Language: [ไทย](README.md) | **English**

## Latest: v2.1-native (5 Nov 2026)

- **GitHub Release:** <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>
- **Direct download:**
  <https://github.com/Rawpasterbin/esp32/releases/download/v2.1-native/water-valve-2.1-native-release.apk>
- **File:** `water-valve-2.1-native-release.apk`
- **Build:** release variant, `versionCode 2`, `versionName 2.1-native`
  (R8/minify disabled on this 8 GB build host; see the migration plan)
- **Package:** `com.example.esp32`
- **Min SDK:** 24, **target SDK:** 34
- **Signing key SHA-256:**
  `6A:73:42:E7:13:B8:C0:41:9A:59:60:FA:E1:8B:43:0D:8E:AE:FA:68:2C:FB:3F:ED:8B:CD:74:83:D7:2F:2E:7E`
- **APK SHA-256:** `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`

Changes in v2.1:

- Manual **open** is locked while the valve is already open or a command is
  pending; duplicate command guard in the ViewModel.
- **Close** remains a safety action, but a duplicate close is not queued while
  one is already pending.
- Device card now shows `ออนไลน์ · sync X นาทีที่แล้ว` / `ออฟไลน์ · เห็นล่าสุด
  X นาทีที่แล้ว`, making the 20-minute online window explicit.

### Install

Download the APK from this folder (GitHub: open the file in the repository and
click **Download**), or copy it from the build machine.

```powershell
adb install -r water-valve-2.1-native-release.apk
```

v2.1 uses the **same signing key** as v2.0, so it installs over v2.0 without
uninstalling. If a debug-signed build is installed, uninstall it first:

```powershell
adb uninstall com.example.esp32
adb install water-valve-2.1-native-release.apk
```

Uninstalling clears the local session/cache; sign in again with the invite code
and current household password. The household/device configuration stays in
Worker/D1.

### Signing key

The release keystore and its password are stored **outside the repository**:

- `<secure-path>\water-valve-release.keystore`
- `<secure-path>\water-valve-keystore-password.txt`

Keep a backup. Future APKs must be signed with the same key to update this
installation. Never commit the keystore or password.

## Previous: v2.0-native (5 Nov 2026)

- **File:** `water-valve-2.0-native-release.apk`
- **Build:** release variant, `versionCode 1`, `versionName 2.0-native`
- **APK SHA-256:**
  `D8F82D055F0A0AB1EE63334133BF2D8154D1C430730B888399AF419EC89AEEAC`

## Notes

- MQTT credentials and device token are **not** in the APK.
- The app talks to `https://water-valve-api-v2.czqwertyyy.workers.dev`.
- Countdown/manual commands are queued in D1 and delivered on the ESP32 sync
  (up to ~15 minutes) unless the board is also controlled over MQTT.
