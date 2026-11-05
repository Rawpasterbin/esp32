# Water Valve Scheduler v2

> Language: [ไทย](README.md) | **English**

A single-valve water control system built from a central API on Cloudflare Workers, a native Android application, and ESP32 firmware connected over a cellular network.

```
Android (Compose) ──HTTPS──> Cloudflare Worker v2 ──D1 (config, commands, audit)
                                   ▲
                                   │ HTTPS sync every 15 min
                                   │
ESP32 (A7672X) ──MQTT/TLS 8883──> HiveMQ Cloud
```

## Components

| Component | Details |
|---|---|
| Worker API | Cloudflare Workers + D1 for config, command queue, and audit log |
| Application | Native Android with Jetpack Compose, no WebView |
| Firmware | ESP32 + A7672X with MQTT command/status and HTTPS sync every 15 minutes |
| MQTT | HiveMQ Cloud over TLS 8883 |

The Worker owns all state: mode, schedules, commands, and audit history. The app reads and writes over HTTPS; the device pulls the desired configuration every 15 minutes and reports the outcome.

## Features

- Login with an invite code and household password; member sessions and device tokens are separate.
- Multiple devices can edit mode and schedules concurrently using `revision` and `If-Match`.
- Commands have a `commandId`, a 20-minute TTL, idempotency, and latest-wins superseding.
- Overnight schedules are supported, and the valve closes automatically after `maxopen`.
- The app stores its session in Android Keystore and keeps an offline cache.
- No MQTT credentials in the APK; all commands go through the Worker.

## Repository layout

| Path | Contents |
|---|---|
| `worker/v2/index.js` | Worker API: auth, state, config revision, commands, device sync/ack |
| `migrations/0001_init.sql` | Core schema: households, members, sessions, devices, configs, commands, audit_log |
| `migrations/0002_auth_attempts.sql` | Failed login tracking table |
| `migrations/0003_rate_limits.sql` | Per-member command rate limit table |
| `wrangler.v2.toml.example` | Worker/D1 template; copy to `wrangler.v2.toml` and set `database_id` |
| `scripts/test-api.ps1` | API test suite for local and production environments |
| `scripts/create-household.ps1` | Creates a household and device token with restricted file permissions |
| `esp32_water_valve/` | Firmware and wiring/provisioning documentation |
| `releases/` | Signed APK and checksums |

The Android application lives in a separate repository (Jetpack Compose, no WebView).

## Local development

Create the local config from the template and fill in the D1 `database_id`:

```powershell
Copy-Item wrangler.v2.toml.example wrangler.v2.toml
# edit database_id in wrangler.v2.toml (this file is git-ignored)
```

```powershell
# Terminal 1
npx wrangler dev --config wrangler.v2.toml --local --port 8787

# Terminal 2 (first run)
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0003_rate_limits.sql

powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

`.dev.vars` must contain `SETUP_TOKEN` and is excluded from git. The test script reads the environment or this file.

## Deploy

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0003_rate_limits.sql

$env:SETUP_TOKEN = '<setup-token>'
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
npx wrangler deploy --config wrangler.v2.toml
```

Migrations use `CREATE TABLE IF NOT EXISTS`, so re-running them is safe. Schema changes require a new migration file.

Test against a deployed Worker:

```powershell
$env:API_BASE = 'https://<your-worker-url>'
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

The suite creates real households when run against production and does not clean them up automatically. Prefer a local D1, or remove the test rows from D1 manually afterwards.

## Creating a household

```powershell
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/create-household.ps1 `
  -ApiBase 'https://<your-worker-url>' `
  -HouseholdName '<household-name>'
```

- The script prompts for the household password (at least 8 characters) and does not write it to disk.
- The output contains a one-time `deviceToken`. The JSON file is written outside the repository at the `-OutFile` location with user-only permissions.
- The script prints the provisioning commands `set token ...` and `set url ...`.
- The output file is a secret and must never be committed.

## Android

Latest APK:

- [water-valve-2.1-native-release.apk](./releases/water-valve-2.1-native-release.apk)
- SHA-256: `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
- GitHub Release: <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>

```powershell
adb install -r water-valve-2.1-native-release.apk
```

If a debug-signed build is installed, uninstall it first:

```powershell
adb uninstall com.example.esp32
adb install water-valve-2.1-native-release.apk
```

Build from source (separate repository):

```powershell
cd <android-project>
.\gradlew.bat assembleDebug :app:testDebugUnitTest --no-daemon
```

- Debug and release builds use the API configured in the build config.
- Point the emulator at a local Worker: `.\gradlew.bat assembleDebug -PapiBaseUrl=http://10.0.2.2:8787/`
- The session token is encrypted with Android Keystore; the server stores only its hash.
- The APK contains no MQTT credentials.

## Firmware

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" esp32_water_valve
```

Wiring, provisioning, MQTT topics, the A7672X single-TLS-session limitation, data usage, and safety are documented in `esp32_water_valve/README.en.md`.

## API

Every endpoint under `/v1/households/...` requires `Authorization: Bearer <session token>`, and the session must belong to a member of that household. Otherwise the API returns 403.

| Method | Path | Auth | Purpose |
|---|---|---|---|
| GET | `/health` | – | Server version and time |
| POST | `/v1/setup` | `X-Setup-Token` | Creates a household and device token (plaintext returned once) |
| POST | `/v1/auth/join` | – | Joins a household with invite code, password, and displayName |
| POST | `/v1/auth/login` | – | Issues a session token (valid for 30 days) |
| POST | `/v1/auth/logout` | session | Revokes the current session |
| GET | `/v1/me` | session | Member and household information |
| GET | `/v1/households/:hid/state` | session | Latest config and device status |
| PUT | `/v1/households/:hid/config` | session | Writes mode/schedules; requires `If-Match` or `baseRevision` |
| POST | `/v1/households/:hid/password` | session | Changes the household password and revokes other sessions |
| POST | `/v1/households/:hid/devices/:did/rotate-token` | session | Issues a new device token |
| POST | `/v1/households/:hid/commands` | session | Queues an open/close command (supports `idempotencyKey`) |
| GET | `/v1/households/:hid/commands` | session | Lists recent commands and expires stale ones |
| POST | `/v1/devices/sync` | device token | Syncs config, retrieves commands, reports status |
| POST | `/v1/devices/status` | device token | Updates status only |
| GET | `/v1/devices/desired` | device token | Retrieves the desired configuration |
| POST | `/v1/devices/commands/:id/ack` | device token | Acknowledges a command result |

Error codes: `BAD_REQUEST` 400, `UNAUTHORIZED` 401, `FORBIDDEN` 403, `NOT_FOUND` 404, `CONFLICT` 409, `VALIDATION` 422, `PRECONDITION_REQUIRED` 428, `RATE_LIMITED` 429, `INTERNAL` 500

## Constants and limits

| Item | Value | Defined in |
|---|---|---|
| Device sync interval | 15 minutes (retry after 60 seconds on failure) | `SYNC_INTERVAL_MS` |
| Online threshold | Last seen within 20 minutes | `DEVICE_ONLINE_WINDOW_MS` |
| Command lifetime | 20 minutes | `COMMAND_TTL_SECONDS` |
| Maximum schedules | 8 entries | `validateConfig` |
| Session lifetime | 30 days | `SESSION_DAYS` |
| Failed logins | 5 per minute per IP | `auth_attempts` |
| Command rate | 10 per minute per member | `rate_limits` |
| Maximum valve open time | 30 minutes (configurable 1–240) | `maxopen` |
| Expected data usage | Approximately 55–105 MB per month | firmware |

## Troubleshooting

| Symptom | Likely cause | Resolution |
|---|---|---|
| Device shows as offline | Sync failure or last contact older than 20 minutes | Inspect the `Sync HTTP ...` log |
| `Sync HTTP 401` | Invalid or rotated device token | Issue a new token and provision with `set token` |
| `Sync HTTP 0` | DNS, TLS, CA, or APN problem | Verify `AT+CCERTLIST`, APN, and CA |
| Command remains pending | Device was offline longer than the TTL | Reissue the command when it is online |
| 429 while logging in | Five failed attempts within one minute | Wait one minute |
| 429 while issuing commands | More than 10 commands per minute per member | Wait one minute |
| 409 while saving config | Another user saved first, so the revision changed | Reload and save again |
| 428 while saving config | `If-Match` was not sent | Send the most recently read revision |
| Worker returns 500 | Invalid D1 binding, incomplete migrations, or malformed body | Run `npx wrangler tail --config wrangler.v2.toml` |
| Valve closes unexpectedly | `maxopen` reached, schedule ended, or a `close` command was delivered | Check the `audit_log` table |

## Before real valve operation

- Verify the relay active level (`RELAY_ACTIVE_HIGH`) against the actual board before connecting a valve.
- Use an MQTT credential created for this device only.
- MQTT topics are still global (`myhouse/valve/...`), so one household per MQTT deployment is supported.
- The valve is forced closed at boot and closes automatically when open longer than `maxopen`.
- Commands with a TTL are rejected while the device clock is not synchronized.
- Use a separate breaker or cutoff for the valve.

## Security

- Worker/D1 stores only hashes of session and device tokens; plaintext is returned once when created or rotated.
- The APK contains no MQTT credentials; all commands go through the Worker.
- `.dev.vars`, `wrangler.v2.toml`, keystores, and token files are git-ignored; never commit them.
- Restrict the device MQTT user to `myhouse/valve/control`, `status`, `mode`, and `schedule`.
- After rotating a device token or MQTT credential, provision the device again with `set token` / `set mqttuser` / `set mqttpass`.
- One household per MQTT broker is supported because topics are still global.

## Terminology

Technical terms are kept in English where they are proper names or where translation would distort the meaning: Worker, D1, API, MQTT, TLS, HTTPS, ESP32, A7672X, HiveMQ, Cloudflare, Compose, APK, token, firmware, provisioning, sync, deploy, build, release, repository, revision, ETag, If-Match, JSON, Keystore, NVS, GPIO, relay, watchdog, maxopen, schedule, TTL, idempotency, ACL, CA, and NTP.
