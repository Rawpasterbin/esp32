# Water Valve APK releases

> Language: [ไทย](README.md) | **English**

## v2.1-native

- **File:** `water-valve-2.1-native-release.apk`
- **Build:** release variant, `versionCode 2`, `versionName 2.1-native`
- **Package:** `com.example.esp32`
- **Min SDK:** 24, **target SDK:** 34
- **APK SHA-256:** `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
- **Signing key SHA-256:** `6A:73:42:E7:13:B8:C0:41:9A:59:60:FA:E1:8B:43:0D:8E:AE:FA:68:2C:FB:3F:ED:8B:CD:74:83:D7:2F:2E:7E`
- **GitHub Release:** <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>

### Install

```powershell
adb install -r water-valve-2.1-native-release.apk
```

If a debug-signed build is installed, uninstall it first:

```powershell
adb uninstall com.example.esp32
adb install water-valve-2.1-native-release.apk
```

Uninstalling clears the local session and cache. Sign in again with the invite code and current household password; the household/device data remains in Worker/D1.

### Signing key

The release keystore and password are stored outside the repository. Back them up: future APKs must use the same key to update an existing installation. Never commit the keystore or password.

### Notes

- The APK contains no MQTT credentials or device token.
- Manual commands are queued in D1 and delivered on the next device sync (up to about 15 minutes).
- The app communicates with the Worker configured at build time.
