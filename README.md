# Water Valve Scheduler v2

Central configuration + native Android app + ESP32 cellular controller for one
water valve.

```
Android (Compose) ──HTTPS──> Cloudflare Worker v2 ──D1 (config, commands, audit)
                                   ▲
                                   │ HTTPS sync every 15 min
                                   │
ESP32 (A7672X) ──MQTT/TLS 8883──> HiveMQ Cloud
```

- **Worker v2:** `https://water-valve-api-v2.czqwertyyy.workers.dev`
- **D1:** `water-valve-v2` (APAC)
- **Android:** native Compose, no WebView
- **Firmware:** ESP32 + A7672X, MQTT real-time + HTTPS config sync

## Download Android APK

- **Latest:** [water-valve-2.1-native-release.apk](./releases/water-valve-2.1-native-release.apk)
  - SHA-256: `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
  - GitHub page (login required if the repo is private):
    <https://github.com/Rawpasterbin/esp32/blob/main/releases/water-valve-2.1-native-release.apk>
- **Previous:** [water-valve-2.0-native-release.apk](./releases/water-valve-2.0-native-release.apk)
  - SHA-256: `D8F82D055F0A0AB1EE63334133BF2D8154D1C430730B888399AF419EC89AEEAC`
- v2.1 uses the same signing key as v2.0, so it installs over v2.0 directly.
  If the debug-signed `com.example.esp32` is installed, uninstall it first
  (different signing key), then install. Login with the invite code and current
  household password; server data is unchanged.

## Repository layout

| Path | Contents |
|---|---|
| `worker/v2/index.js` | Worker API: auth, state, config revision, commands, device sync/ack |
| `migrations/0001_init.sql` | D1 schema |
| `migrations/0002_auth_attempts.sql` | Login/rate-limit table |
| `migrations/0003_rate_limits.sql` | Per-member command rate-limit table |
| `wrangler.v2.toml` | Worker/D1 config |
| `scripts/test-api.ps1` | API smoke tests (local or production) |
| `esp32_water_valve/` | Firmware + wiring/provisioning README |
| `docs/` | Analysis, architecture, migration notes |
| Android project | `C:\Users\ssde\AndroidStudioProjects\ESP32` (separate git repo) |

## Quick start (local API)

```powershell
# Terminal 1
npx wrangler dev --config wrangler.v2.toml --local --port 8787

# Terminal 2 (first time only)
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0003_rate_limits.sql
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

`.dev.vars` must contain `SETUP_TOKEN=...` (git-ignored).

## Production deploy

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0003_rate_limits.sql
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
npx wrangler deploy --config wrangler.v2.toml
```

Run the full API suite against production:

```powershell
$env:API_BASE = 'https://water-valve-api-v2.czqwertyyy.workers.dev'
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

## Create the real household

```powershell
$env:SETUP_TOKEN = Get-Content C:\Temp\opencode\water-valve-setup-token.txt -Raw
powershell -ExecutionPolicy Bypass -File scripts/create-household.ps1 `
  -HouseholdName 'บ้านของเรา'
```

- Prompts for the household password (min 8 characters); the password is not
  written to disk.
- Saves the one-time `deviceToken` and IDs to
  `C:\Temp\opencode\water-valve-household.json` with a user-only ACL.
- Prints the `set token ...` / `set url ...` provisioning commands.
- Use `-Force` only when you intentionally want to replace the saved file.
- Treat the token file as a secret; never commit it.

Before provisioning firmware, read `docs/SECURITY.md`: the legacy HiveMQ
credential must be rotated first.

## Android

```powershell
cd C:\Users\ssde\AndroidStudioProjects\ESP32
.\gradlew.bat assembleDebug :app:testDebugUnitTest --no-daemon
```

- Debug/release default to the production API.
- Local override: `.\gradlew.bat assembleDebug -PapiBaseUrl=http://10.0.2.2:8787/`
- Session token is encrypted with Android Keystore (AES-GCM).
- Manual valve control queues a command in D1. It reaches the device over MQTT
  when connected, otherwise on the next 15-minute sync (TTL 20 minutes).
- There is no MQTT credential in the APK. Real-time app control will require a
  Worker-issued scoped credential or relay.

## Firmware

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "$env:USERPROFILE\Documents\Arduino\libraries" esp32_water_valve
```

See `esp32_water_valve/README.md` for wiring, provisioning, MQTT topics, the
single-TLS-session limitation, safety behavior, and data budget.

## Status

Implemented and verified:

- Worker v2 + D1 deployed; 95/95 API checks pass against production (including cross-household isolation, device-token scope, logout revocation, password/device-token rotation, and login/command rate limiting).
- Production household `Home` + device created (credentials outside the repo); device sync verified against production.
- Debug APK: `C:\Temp\opencode\water-valve-2.0-native-debug.apk` (installable; release build is unsigned).
- **v1 scope: one household/valve per MQTT deployment** — firmware topics are still global; see `docs/SECURITY.md`.
- Config revision + `If-Match` conflict handling.
- Command queue with idempotency, 20-minute TTL, latest-wins supersede.
- Android login, dashboard, mode/schedule editing, command queue, Keystore token.
- **Android runtime smoke test passed on a real phone** (Infinix X6815B, Android 12): login, mode/schedule edits, open/close with simulated device sync/ack, conflict dialog, offline cache across process restart, password change, logout/relogin. See `docs/ANDROID_RUNTIME_TEST.md`.
- **ESP32 board connected** (COM15, WiFi bench build v2.1.0): WiFi + HTTPS sync `HTTP 200`, revision 16, device online in D1; a real `close` command was delivered on sync and acked `200`.
- **MQTT connected** to HiveMQ Cloud with the new device user: `CONNACK 0`; `getStatus` and an MQTT `close` command were verified. The old leaked `test` user must still be disabled/deleted. See `docs/MQTT_SETUP.md`.
- Firmware compiles in cellular and Wi-Fi bench modes; MQTT + HTTPS sync code.

Still requires hardware verification:

- Relay active level and valve wiring (`RELAY_ACTIVE_HIGH`).
- A7672X CA upload/TLS handshake on the carrier network (current board is WiFi bench, not cellular).
- Disable/delete the old leaked HiveMQ `test` user.
- Power stability under cellular transmit bursts.
- Real valve open/close with the relay/valve connected (only a no-op close was tested).
- Real-time Android MQTT control (optional phase).

See `docs/ARCHITECTURE_CENTRAL_CONFIG.md` §17–18,
`docs/HARDWARE_TEST_CHECKLIST.md`, **`docs/SECURITY.md`** (legacy MQTT
credential must be rotated before hardware deployment), and
`docs/RUNBOOK.md` for deploy/backup/rotation procedures.
