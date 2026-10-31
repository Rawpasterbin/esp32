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

## Repository layout

| Path | Contents |
|---|---|
| `worker/v2/index.js` | Worker API: auth, state, config revision, commands, device sync/ack |
| `migrations/0001_init.sql` | D1 schema |
| `migrations/0002_auth_attempts.sql` | Login/rate-limit table |
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
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

`.dev.vars` must contain `SETUP_TOKEN=...` (git-ignored).

## Production deploy

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
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

- Worker v2 + D1 deployed; 90/90 API checks pass against production (including cross-household isolation, device-token scope, logout revocation, password/device-token rotation, and auth rate limiting).
- Config revision + `If-Match` conflict handling.
- Command queue with idempotency, 20-minute TTL, latest-wins supersede.
- Android login, dashboard, mode/schedule editing, command queue, Keystore token.
- Firmware compiles in cellular and Wi-Fi bench modes; MQTT + HTTPS sync code.

Still requires hardware/user verification:

- Relay active level and valve wiring (`RELAY_ACTIVE_HIGH`).
- A7672X CA upload/TLS handshake on the carrier network.
- HiveMQ user/ACL for the firmware topics.
- Power stability under cellular transmit bursts.
- Android runtime smoke test on a real device or an emulator with enough RAM.
- Real-time Android MQTT control (optional phase).

See `docs/ARCHITECTURE_CENTRAL_CONFIG.md` §17–18,
`docs/HARDWARE_TEST_CHECKLIST.md`, **`docs/SECURITY.md`** (legacy MQTT
credential must be rotated before hardware deployment), and
`docs/RUNBOOK.md` for deploy/backup/rotation procedures.
