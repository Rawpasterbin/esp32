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

## System overview

| Component | Details |
|---|---|
| Worker API | `https://water-valve-api-v2.czqwertyyy.workers.dev` |
| Database | Cloudflare D1 `water-valve-v2` (APAC region) |
| Application | Native Android with Jetpack Compose, no WebView |
| Firmware | ESP32 + A7672X with MQTT real-time and HTTPS config sync |

The Worker owns all state: mode and schedules (config), the command queue, and the audit log. The application reads and writes over HTTPS, while the device pulls the desired configuration every 15 minutes and reports the outcome of each command.

## Installing the Android application

- GitHub Release: <https://github.com/Rawpasterbin/esp32/releases/tag/v2.1-native>
- Direct download (v2.1): <https://github.com/Rawpasterbin/esp32/releases/download/v2.1-native/water-valve-2.1-native-release.apk>
- File in this repository: [water-valve-2.1-native-release.apk](./releases/water-valve-2.1-native-release.apk)
  SHA-256 `73A376BDF50D72572D35C6B2B5E25AAB86AECCAAFF74C68B6795AF7802B1CC5F`
- Previous version: [water-valve-2.0-native-release.apk](./releases/water-valve-2.0-native-release.apk)
  SHA-256 `D8F82D055F0A0AB1EE63334133BF2D8154D1C430730B888399AF419EC89AEEAC`

Version 2.1 is signed with the same key as 2.0 and installs over it directly. If a debug-signed `com.example.esp32` package is present, it must be uninstalled first because the signing key differs. Afterwards, log in with the invite code and the current household password; server-side data is unchanged.

## Command lifecycle

1. The user issues an open or close command from the application. The Worker records it in the `commands` table with status `pending` and a 20-minute lifetime.
2. The device synchronizes on its 15-minute interval, performs the command, and acknowledges it, moving the record to `acked`.
3. If the command expires before the device retrieves it, the status becomes `expired` with `error='ttl'` and the command must be reissued.
4. If a new command is issued while the previous one is still pending, it supersedes the old one with `error='superseded'`. This prevents an older pending open command from being delivered after a newer close command.

One limitation follows from this design: the application does not publish over MQTT itself, so commands are not delivered instantly even when the device is connected to MQTT. The MQTT path is used for commands originating elsewhere and for real-time control in a later phase. Instant control requires a relay through the Worker or a scoped credential issued by the Worker, neither of which is implemented yet.

## Repository layout

| Path | Contents |
|---|---|
| `worker/v2/index.js` | Worker API: authentication, state, config revision, commands, device sync/ack |
| `migrations/0001_init.sql` | Core schema: households, members, sessions, devices, configs, commands, audit_log |
| `migrations/0002_auth_attempts.sql` | Failed login tracking table |
| `migrations/0003_rate_limits.sql` | Per-member command rate limit table |
| `wrangler.v2.toml.example` | Worker and D1 configuration template; copy to `wrangler.v2.toml` (local, git-ignored) and fill in your own `database_id` |
| `scripts/test-api.ps1` | API test suite for local and production environments |
| `scripts/create-household.ps1` | Creates a household and device token with restricted file permissions |
| `esp32_water_valve/` | Firmware and wiring/provisioning documentation |
| `docs/` | Analysis, architecture, runbook, and security documents |
| `releases/` | Signed APK files and checksums |

The Android application lives in a separate repository (Jetpack Compose, no WebView).

## Local development and testing

For the first run, create the local config from the template and fill in the D1
`database_id`:

```powershell
Copy-Item wrangler.v2.toml.example wrangler.v2.toml
# edit database_id in wrangler.v2.toml (this file is git-ignored)
```

```powershell
# Terminal 1
npx wrangler dev --config wrangler.v2.toml --local --port 8787

# Terminal 2 (first run only)
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --local --file=migrations/0003_rate_limits.sql

powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

`.dev.vars` must contain `SETUP_TOKEN` and is excluded from git. When the `SETUP_TOKEN` environment variable is not set, the test script reads the value from this file.

## Production deployment

```powershell
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0001_init.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0002_auth_attempts.sql
npx wrangler d1 execute water-valve-v2 --config wrangler.v2.toml --remote --file=migrations/0003_rate_limits.sql

$env:SETUP_TOKEN = '<setup-token>'
$env:SETUP_TOKEN | npx wrangler secret put SETUP_TOKEN --config wrangler.v2.toml
npx wrangler deploy --config wrangler.v2.toml
```

All migration files use `CREATE TABLE IF NOT EXISTS`, so re-running them is safe. Existing tables and indexes are not altered to match a new schema, however; structural changes require a new migration file.

Running the full suite against production:

```powershell
$env:API_BASE = 'https://water-valve-api-v2.czqwertyyy.workers.dev'
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/test-api.ps1
```

This suite creates two real households to verify cross-household isolation. Running it against production leaves test data in D1 that must be removed manually following `docs/RUNBOOK.md` section 9.

## Creating a household

```powershell
$env:SETUP_TOKEN = '<setup-token>'
powershell -ExecutionPolicy Bypass -File scripts/create-household.ps1 `
  -HouseholdName 'บ้านของเรา'
```

- The script prompts for the household password (at least 8 characters) and does not write it to disk.
- The output contains a one-time `deviceToken` and the relevant identifiers. A JSON file is written outside the repository at a location controlled by the `-OutFile` parameter, with permissions restricted to the current user.
- The script prints the device provisioning commands: `set token ...` and `set url ...`.
- Use `-Force` only when replacing an existing output file intentionally.
- The generated file is a secret and must never be committed.

Read `docs/SECURITY.md` before provisioning firmware: the legacy HiveMQ credential leaked and must be rotated in the console first.

## Building the Android application

```powershell
cd <android-project>
.\gradlew.bat assembleDebug :app:testDebugUnitTest --no-daemon
```

- Debug and release builds target the production API by default.
- To target a local instance from the emulator: `.\gradlew.bat assembleDebug -PapiBaseUrl=http://10.0.2.2:8787/`
- The session token is encrypted with Android Keystore (AES-GCM); the server stores only its hash.
- The APK contains no MQTT credentials.

## Firmware

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" esp32_water_valve
```

Wiring, initial provisioning, MQTT topics, the A7672X single-TLS-session limitation, data usage, and safety requirements are documented in `esp32_water_valve/README.en.md`.

Values configurable over serial include `set maxopen <1..240>` (minutes, default 30), `set mqttuser`, `set mqttpass`, `set token`, and `set url`. Run `reboot` after configuring.

## API

Every endpoint under `/v1/households/...` requires `Authorization: Bearer <session token>`, and the session must belong to a member of that household. Otherwise the API returns 403.

| Method | Path | Authentication | Purpose |
|---|---|---|---|
| GET | `/health` | – | Server version and time |
| POST | `/v1/setup` | `X-Setup-Token` | Creates a household and device token (plaintext returned once) |
| POST | `/v1/auth/join` | – | Joins a household with invite code, password, and displayName |
| POST | `/v1/auth/login` | – | Issues a session token (valid for 30 days) |
| POST | `/v1/auth/logout` | session | Revokes the current session |
| GET | `/v1/me` | session | Member and household information |
| GET | `/v1/households/:hid/state` | session | Latest config and device status |
| PUT | `/v1/households/:hid/config` | session | Writes mode and schedules; requires `If-Match` or `baseRevision` |
| POST | `/v1/households/:hid/password` | session | Changes the household password and revokes other sessions |
| POST | `/v1/households/:hid/devices/:did/rotate-token` | session | Issues a new device token |
| POST | `/v1/households/:hid/commands` | session | Queues an open or close command (supports `idempotencyKey`) |
| GET | `/v1/households/:hid/commands` | session | Lists recent commands and expires stale ones |
| POST | `/v1/devices/sync` | device token | Synchronizes config, retrieves commands, reports status |
| POST | `/v1/devices/status` | device token | Updates status only |
| GET | `/v1/devices/desired` | device token | Retrieves the desired configuration |
| POST | `/v1/devices/commands/:id/ack` | device token | Acknowledges the result of a command |

Error codes returned by the Worker: `BAD_REQUEST` 400, `UNAUTHORIZED` 401, `FORBIDDEN` 403, `NOT_FOUND` 404, `CONFLICT` 409 (revision mismatch), `VALIDATION` 422, `PRECONDITION_REQUIRED` 428 (missing `If-Match`), `RATE_LIMITED` 429, `INTERNAL` 500

## Constants and system limits

| Item | Value | Defined in |
|---|---|---|
| Device sync interval | 15 minutes (retry after 60 seconds on failure) | Firmware `SYNC_INTERVAL_MS` |
| Online threshold | Last seen within 20 minutes | Worker `DEVICE_ONLINE_WINDOW_MS` |
| Command lifetime | 20 minutes | Worker `COMMAND_TTL_SECONDS` |
| Maximum schedules | 8 entries | Worker `validateConfig` |
| Session lifetime | 30 days | Worker `SESSION_DAYS` |
| Failed logins | 5 per minute per IP | `auth_attempts` table |
| Command rate | 10 per minute per member | `rate_limits` table |
| Maximum valve open time | 30 minutes (configurable 1–240) | Firmware `maxopen` |
| Expected data usage | Approximately 10–30 MB per month | – |

## Troubleshooting

| Symptom | Likely cause | Resolution |
|---|---|---|
| Device shows as offline | Sync failure or last contact older than 20 minutes | Inspect the firmware log line `Sync HTTP ...` |
| `Sync HTTP 401` | Invalid or rotated device token | Rotate the token and provision with `set token` over serial |
| `Sync HTTP 0` | DNS, TLS, CA, or APN problem | Verify `AT+CCERTLIST` and the APN, then retry |
| Command remains pending | The device was offline longer than the command lifetime | Reissue the command when the device is online |
| 429 while logging in | Five failed attempts within one minute | Wait one minute or clear `auth_attempts` as described in `docs/RUNBOOK.md` section 9 |
| 429 while issuing commands | More than 10 commands per minute per member | Wait one minute and retry |
| 409 while saving config | Another user saved first, so the revision changed | Reload and save again |
| 428 while saving config | `If-Match` was not sent | Send the most recently read revision |
| Worker returns 500 | Invalid D1 binding, incomplete migrations, or malformed body | Run `npx wrangler tail water-valve-api-v2 --config wrangler.v2.toml` |
| Valve closes unexpectedly | `maxopen` reached, schedule ended, or a `close` command was delivered | Check the logs and the `audit_log` table |

## Requirements before real valve operation

- One household per MQTT deployment. MQTT topics are still global (`myhouse/valve/...`) and share a single HiveMQ user, so a second household on the same broker could receive another household's commands. Per-device topics must be implemented first (see `docs/SECURITY.md`).
- Verify the relay active level against the actual board (`RELAY_ACTIVE_HIGH`); otherwise an open command can produce a close action.
- The valve is forced closed at boot and closes automatically when open longer than `maxopen`. Both behaviours are intentional.
- Commands with a lifetime are rejected while the device clock is not synchronized (NTP). Verify device time before testing.
- The legacy HiveMQ credential must be rotated before deployment.

## Development status

Completed and verified:

| Area | Status |
|---|---|
| Worker v2 and D1 | Deployed to production; the API suite passes 95/95, including cross-household isolation, device token scope, session revocation on logout, password and token rotation, and rate limiting |
| Household and device | Created in production; credentials are stored outside the repository and device sync works |
| Android application v2.1 | Signed and installed on a physical device (Android 12); verified login, mode and schedule edits, open/close with simulated sync/ack, conflict dialog, offline cache across process restarts, password change, and logout/relogin |
| Firmware | Compiles for both cellular and Wi-Fi modes; MQTT and HTTPS sync verified |
| Physical board | HTTPS sync returns `HTTP 200`, the device appears online in D1, and a `close` command was delivered during a sync cycle and acknowledged |
| MQTT | Connects to HiveMQ Cloud with the new device user (`CONNACK 0`); `getStatus` and a `close` command were verified |

Not yet completed:

| Item | Note |
|---|---|
| Relay active level and valve wiring | The valve has not been connected |
| A7672X CA upload and TLS handshake on the carrier network | Testing so far used Wi-Fi |
| Disabling or deleting the leaked HiveMQ user | Must be performed in the console |
| Power stability during cellular transmission | Not yet measured |
| Real valve open and close | Only a no-op close was tested |
| Real-time control from the application | Planned for a later phase; requires a scoped credential |

## References

- `docs/ARCHITECTURE_CENTRAL_CONFIG.md` sections 17–18 — architecture and design decisions
- `docs/HARDWARE_TEST_CHECKLIST.md` — hardware test procedures
- `docs/SECURITY.md` — secret handling and credential rotation
- `docs/RUNBOOK.md` — deployment, backup, token rotation, and incident response

## Terminology

Technical terms are kept in English where they are proper names or where translation would distort the meaning: Worker, D1, API, MQTT, TLS, ESP32, A7672X, HiveMQ, Cloudflare, Compose, APK, token, firmware, provisioning, sync, deploy, build, release, commit, repository, revision, ETag, If-Match, JSON, Keystore, NVS, GPIO, relay, watchdog, maxopen, schedule, TTL, idempotency, ACL, CA, and NTP.
