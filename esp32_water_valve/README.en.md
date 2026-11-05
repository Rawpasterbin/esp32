# Water Valve Controller v2 (ESP32 + A7672X)

> Language: [ไทย](README.md) | **English**

Firmware for a single water valve, driven by an ESP32 over a 4G/5G SIM.
Cloudflare Worker v2 and D1 are the source of truth for mode and schedules;
MQTT is the real-time command and status channel.

```
Android app ──HTTPS──> Worker v2 ──D1 (config, commands, audit)
                            ▲
                            │ HTTPS sync (15 min)
                            │
ESP32 ──MQTT/TLS 8883──> HiveMQ Cloud
```

> The Android application currently uses HTTPS and the Worker command queue. MQTT is the ESP32 channel and will support real-time control from the application in a later phase.

## Design notes

- Mode and schedules come from Worker v2 and are no longer stored on the phone.
- Every command has a `commandId` (the last 8 are remembered across reboots, so a QoS1 redelivery cannot execute twice) and a TTL (`expiresAt`).
- Overnight schedules, such as 22:00–06:00, work correctly: the after-midnight part uses the previous day's flag.
- The valve is forced closed at boot and closed again if it stays open longer than `maxopen` minutes, even without a network connection.
- Wi-Fi is used for testing only; production uses the A7672X modem.
- No WebView and no dependency on the web UI.

## Hardware

| Signal | ESP32 pin | Notes |
|---|---|---|
| Modem UART RX | GPIO16 | ESP32 RX ← A7672X TX |
| Modem UART TX | GPIO17 | ESP32 TX → A7672X RX |
| Modem power | 5 V | Use a supply that can deliver 2 A peaks |
| Relay IN | GPIO23 | Active level must be verified on the actual board |
| Relay VCC/GND | 5 V / GND | Common ground with the ESP32 |
| Valve | Relay COM/NO | Use a relay rated for the valve current |

**Power:** do not power the A7672X from the ESP32 3.3 V pin. Cellular
transmit bursts can brown out the board and reset the modem. Use a separate
5 V supply with a common ground, and add bulk capacitance (≥ 470 µF) near
the modem if needed.

**Relay safety:** the default is `RELAY_ACTIVE_HIGH 1` (HIGH = valve open),
matching the old firmware. Some relay modules are active-low. Before
connecting the valve, flash the firmware, watch the relay LED, and confirm:

- boot → relay off / valve closed
- `set`/`sync`/command → relay clicks only when expected

If the logic is inverted, change `RELAY_ACTIVE_HIGH` to `0` and reflash.

For a solenoid valve, add a flyback diode across the coil and a fuse on the
valve supply. Do not switch mains voltage on a breadboard.

## Required libraries

Install with Library Manager or `arduino-cli lib install`:

- `TinyGSM` **0.12.0** (A7672X support)
- `PubSubClient` **2.8**
- `ArduinoJson` **7.x** (tested 7.4.3)
- ESP32 core **3.3.11** (`esp32:esp32`)

`Preferences`, `WiFi`, `WiFiClientSecure` and `esp_task_wdt` come with the
ESP32 core.

## Build

Cellular production build (default, `USE_CELLULAR=1`):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" `
  esp32_water_valve
```

Wi-Fi test build (`USE_CELLULAR=0`):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "<arduino-libraries>" `
  --build-property "compiler.cpp.extra_flags=-DUSE_CELLULAR=0" `
  esp32_water_valve
```

Flash the board (use the actual port):

```powershell
arduino-cli upload -p <serial-port> --fqbn esp32:esp32:esp32 esp32_water_valve
```

The Wi-Fi build is larger (TLS and Wi-Fi use about 1.05 MB, or 80% of the default app partition) but still fits. The cellular build is approximately 460 KB.

## First-time provisioning

Open the serial monitor at **115200** baud, line ending **Newline**.
Provisioning is stored in NVS and survives reboots.

1. Create the household/device with Worker setup (once per household):

```powershell
$body = @{
  householdName = '<household-name>'
  deviceName    = '<device-name>'
  password      = '<household-password>'
  timezone      = 'Asia/Bangkok'
} | ConvertTo-Json

Invoke-RestMethod -Method Post `
  -Uri 'https://<your-worker-url>/v1/setup' `
  -Headers @{ 'X-Setup-Token' = '<SETUP_TOKEN>' } `
  -ContentType 'application/json' `
  -Body $body
```

The response contains `inviteCode`, `deviceId` and `deviceToken` (`dev_...`).
Keep the device token secret; it is shown only once.

2. On the ESP32 serial console:

```text
set token dev_xxxxxxxxxxxxxxxx
set apn internet
set apnuser <apn-user-if-required>
set apnpass <apn-password-if-required>
set url https://<your-worker-url>
set mqtthost <hivemq-host>
set mqttuser <hivemq-user>
set mqttpass <hivemq-password>
set maxopen 30
sync
status
reboot
```

For the Wi-Fi test build only:

```text
set ssid <wifi-name>
set pass <wifi-password>
```

Notes:

- `set url` strips trailing slashes.
- `set maxopen` accepts 1–240 minutes; the default is 30.
- `status` prints secrets masked.
- `sync` forces an immediate HTTPS sync.
- The Worker URL is **not** compiled in; provision it with `set url` before syncing.
- The MQTT host and credentials are **not** compiled in; provision them with
  `set mqtthost`, `set mqttuser`/`set mqttpass` before using MQTT.
- The default APN is `internet`; change it for your carrier if needed.

## Serial commands

| Command | Meaning |
|---|---|
| `help` | Show the command list |
| `show` | Print configuration (secrets masked) |
| `set token <dev_...>` | Device token from `/v1/setup` |
| `set apn <name>` | Cellular APN |
| `set apnuser <u>` / `set apnpass <p>` | APN credentials |
| `set ssid <name>` / `set pass <p>` | Wi-Fi test build only |
| `set url <https://...>` | Worker base URL |
| `set mqtthost <host>` | MQTT broker hostname (must be provisioned) |
| `set mqttuser <u>` / `set mqttpass <p>` | MQTT credentials |
| `set maxopen <1..240>` | Local maximum open time |
| `sync` | Force HTTPS config/command sync |
| `status` | Publish MQTT status and print config |
| `reboot` | Restart the ESP32 |

## Timezone

Schedules and NTP use a fixed **UTC+7 (Asia/Bangkok)** offset (`GMT_OFFSET_SEC` in the sketch). The Worker household has a `timezone` field, but the firmware does not read it yet. If the installation is not in UTC+7, change `GMT_OFFSET_SEC` and redeploy. The Android application renders schedule times in the device's local timezone, so the device must use the same timezone as the valve.

## MQTT topics

> **Current limitation:** topics are global (`myhouse/valve/...`) and the HiveMQ user is shared. One household per MQTT broker is supported until per-device topics or scoped credentials are implemented.

MQTT is optional: if `mqttuser`/`mqttpass` are empty, the device still
connects, syncs config every 15 minutes, and runs schedules/queued commands.
The only difference is that real-time MQTT control/status is unavailable.

Default prefix: `myhouse/valve/`.

| Topic | Direction | Payload |
|---|---|---|
| `control` | app → ESP32 | `ON` / `OFF`, or JSON (below) |
| `status` | ESP32 → app | state JSON (below) |
| `mode` | legacy | `{"mode":"auto"}` — overwritten by next sync |
| `schedule` | legacy | `{"schedules":[...]}` — overwritten by next sync |

Control JSON:

```json
{
  "cmd": "open",
  "commandId": "cmd_...",
  "expiresAt": "2026-10-30T12:00:00.000Z"
}
```

Supported `cmd`: `open`, `close`, `toggle`, `getStatus`, `sync`, `reboot`.
`ON`/`OFF` remain for compatibility and have no TTL. Commands with an
`expiresAt` are rejected when the local clock is past the TTL (plus 5 s
grace). The last 8 command IDs are remembered in NVS, so the same
`commandId` is executed at most once even after a reboot or MQTT redelivery.
Opening is rejected in auto mode (`auto_mode`); **closing is always allowed**
so a manual close can stop the valve after the household switches to auto.

Status JSON (abridged):

```json
{
  "valve": false,
  "online": true,
  "mode": "auto",
  "scheduleCount": 2,
  "ntp": true,
  "time": 1790000000,
  "rssi": -67,
  "uptime": 3600,
  "fw": "2.1.0",
  "lastCommandId": "cmd_...",
  "lastRejectedId": "cmd_...",
  "lastRejectedError": "auto_mode"
}
```

`lastCommandId` is the real-time ack for a successful MQTT command.
`lastRejectedId`/`lastRejectedError` report a rejected command. The next
HTTPS sync also marks `lastCommandId` as acked in D1, so an ack is not lost
if the app misses the status message.

## HTTPS sync

Every 15 minutes (60 s retry after a failure) the firmware calls:

```
POST /v1/devices/sync
Authorization: Bearer dev_...
Content-Type: application/json

{
  "valve": false,
  "mode": "auto",
  "rssi": -67,
  "uptimeSeconds": 3600,
  "firmwareVersion": "2.1.0",
  "lastCommandId": "cmd_...",
  "fault": null
}
```

> An MQTT reconnect does **not** force an HTTPS sync. Sync runs on schedule,
> right after a network reconnect, or when the serial `sync` command is used.
> This keeps the SIM data budget predictable when MQTT flaps.

The response contains `revision`, `mode`, `schedules`, an optional pending
`command` and `serverTime`. If the clock is not set yet, `serverTime` is
used to set it before evaluating schedules.

### Important A7672X limitation: one TLS session

TinyGSM's A7672X driver uses SSL session 0 for every secure client. MQTT
and HTTPS therefore cannot stay open at the same time. The firmware:

1. disconnects MQTT (sends DISCONNECT + closes the TLS session),
2. performs the HTTPS sync/ack,
3. reconnects MQTT on the next loop iteration.

During this short window (a few seconds every 15 minutes) MQTT commands
are not delivered. For critical control, the app should also POST
`/v1/households/:hid/commands` so the command is queued in D1 and delivered by the next
sync. Do not "fix" this by opening two TinyGSM secure clients: the modem
session and `sockets[0]` routing will collide.

## Data budget (399 MB/month SIM)

The single-session design means a fresh TLS handshake for every sync and MQTT reconnect. A realistic estimate:

| Traffic | Estimate |
|---|---|
| HTTPS sync every 15 min (handshake + request/response) | 30–60 MB/month |
| MQTT reconnect after each sync | 20–40 MB/month |
| Status every 5 min | 3–5 MB/month |
| Commands/acks | < 1 MB/month |
| **Total** | **~55–105 MB/month** |

This leaves ample headroom under 399 MB. If the carrier charges for signaling or the modem retries aggressively, check the SIM dashboard after the first week and increase `SYNC_INTERVAL_MS` if needed.

## Safety behavior

- Relay is driven to **closed** before anything else in `setup()`.
- `maxopen` (default 30 min) is enforced locally; the valve closes even if
  the network is down. The fault is reported as `max_runtime`.
- A `close` command is accepted in both manual and auto mode. An `open`
  command is accepted only in manual mode.
- A manual `close` during an active auto schedule is latched in NVS and wins
  until that window ends. Without the latch the 10-second `autoCheck` would
  reopen the valve. Changing to manual mode clears the latch.
- A new `open` command clears the latch; a schedule that becomes inactive
  also clears it.
- Commands with a TTL are rejected if the clock is not synchronized.
- A 120 s task watchdog resets the ESP32 if the loop hangs.
- The valve state is not persisted across reboot; it always boots closed.

## TLS certificate rotation

The deployed Worker host uses a chain rooted at **GlobalSign Root CA (RSA)** for Cloudflare:

leaf → Google Trust Services WE1 → GTS Root R4 (cross-signed) → GlobalSign Root CA

The firmware's `ROOT_CA` is that RSA root. MQTT (HiveMQ Cloud) uses a different chain rooted at **ISRG Root X1** (Let's Encrypt), which is stored in `ROOT_CA_HIVEMQ`. The Wi-Fi build creates a persistent bundle (`ROOT_CA` + `ROOT_CA_HIVEMQ`) and calls `setCACert()` once. The bundle must stay alive for the lifetime of the client; a temporary `String` causes mbedTLS to report "X509 - Format not recognized".

If Cloudflare or HiveMQ changes the chain:

1. Inspect the real chain from a client that reaches the endpoint:
   `openssl s_client -showcerts -connect <host>:443|8883 -servername <host>`
2. Extract the root CA PEM.
3. Replace `ROOT_CA` (HTTPS) or `ROOT_CA_HIVEMQ` (MQTT) in `esp32_water_valve.ino`.
4. Reflash. The firmware uploads the configured CA to the modem as `cfroot.pem` at boot.

The modem certificate file is small (1–2 KB) and is uploaded once per boot, so a factory-reset modem recovers automatically.

## Troubleshooting

| Symptom | Check |
|---|---|
| `modem.init failed` | 5 V supply/current, TX/RX crossed, SIM seated, antenna, `SerialAT.begin` pins |
| `GPRS connect failed` | APN, SIM data plan, signal; try `set apn` |
| `MQTT failed, rc=-2` | TLS/network; CA, clock, port 8883 |
| `MQTT failed, rc=4` | Bad MQTT username/password |
| `MQTT failed, rc=5` | HiveMQ ACL does not allow the topic |
| `Sync HTTP 401` | Device token wrong/revoked; run `set token` again |
| `Sync HTTP 0` | DNS/TLS/connect failure; check serial log and CA |
| `TLS: CA upload failed` | Modem file system; power-cycle modem, check `AT+CCERTLIST` |
| Relay inverted | Set `RELAY_ACTIVE_HIGH 0`, reflash, retest without the valve |
| Valve closes unexpectedly | `maxopen` reached, schedule ended, or MQTT `close` |

## Files

- `esp32_water_valve.ino` — firmware
- `README.en.md` — this file
- Worker API: `../worker/v2/index.js`
- Schema: `../migrations/0001_init.sql`
- Main README: `../README.en.md`
