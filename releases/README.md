# Water Valve APK releases

## v2.0-native (5 Nov 2026)

- **File:** `water-valve-2.0-native-release.apk`
- **Build:** release variant, `versionCode 1`, `versionName 2.0-native`
  (R8/minify disabled on this 8 GB build host; see the migration plan)
- **Package:** `com.example.esp32`
- **Min SDK:** 24, **target SDK:** 34
- **Signing key SHA-256:**
  `6A:73:42:E7:13:B8:C0:41:9A:59:60:FA:E1:8B:43:0D:8E:AE:FA:68:2C:FB:3F:ED:8B:CD:74:83:D7:2F:2E:7E`
- **APK SHA-256:** `D8F82D055F0A0AB1EE63334133BF2D8154D1C430730B888399AF419EC89AEEAC`

### Install

```powershell
adb install -r water-valve-2.0-native-release.apk
```

If a debug-signed build of `com.example.esp32` is already installed, uninstall
it first (different signing key):

```powershell
adb uninstall com.example.esp32
adb install water-valve-2.0-native-release.apk
```

Uninstalling clears the local session/cache; sign in again with the invite code
and the current household password. The household/device configuration stays in
Worker/D1.

### Signing key

The release keystore and its password are stored **outside the repository**:

- `C:\Temp\opencode\water-valve-release.keystore`
- `C:\Temp\opencode\water-valve-keystore-password.txt`

Keep a backup. Future APKs must be signed with the same key to update this
installation. Never commit the keystore or password.

### Notes

- MQTT credentials and device token are **not** in the APK.
- The app talks to `https://water-valve-api-v2.czqwertyyy.workers.dev`.
- Countdown/manual commands are queued in D1 and delivered on the ESP32 sync
  (up to ~15 minutes) unless the board is also controlled over MQTT.
