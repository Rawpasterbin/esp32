# Water Valve Controller v2 (ESP32 + A7672X)

Firmware for one water valve, driven by an ESP32 over a 4G/5G SIM.
Cloudflare Worker v2 + D1 is the source of truth for mode and schedules;
MQTT is the real-time command/status channel.

```
Android app ──HTTPS──> Worker v2 ──D1 (config, commands, audit)
                            ▲
                            │ HTTPS sync (15 min)
                            │
ESP32 ──MQTT/TLS 8883──> HiveMQ Cloud <──MQTT/TLS── Android app
```

## What changed from v1

- Mode and schedules come from Worker v2, not from the phone.
- Every command has a `commandId` (the last 8 are remembered across reboots,
  so a QoS1 redelivery cannot execute twice) and a TTL (`expiresAt`).
- Overnight schedules (for example 22:00–06:00) work correctly: the
  after-midnight part uses the previous day's flag.
- The valve is forced **closed** at boot and closed again if it stays open
  longer than `maxopen` minutes, even with no network.
- Wi-Fi is only a bench/debug option; production uses the A7672X modem.
- No WebView, no web UI dependency.

## Hardware

| Signal | ESP32 pin | Notes |
|---|---|---|
| Modem UART RX | GPIO16 | ESP32 RX ← A7672X TX |
| Modem UART TX | GPIO17 | ESP32 TX → A7672X RX |
| Modem power | 5 V | Use a supply that can deliver 2 A peaks |
| Relay IN | GPIO23 | Active level must be verified on the bench |
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
  --libraries "$env:USERPROFILE\Documents\Arduino\libraries" `
  esp32_water_valve
```

Wi-Fi bench build (`USE_CELLULAR=0`):

```powershell
arduino-cli compile --fqbn esp32:esp32:esp32 `
  --libraries "$env:USERPROFILE\Documents\Arduino\libraries" `
  --build-property "compiler.cpp.extra_flags=-DUSE_CELLULAR=0" `
  esp32_water_valve
```

Flash (replace `COM5`):

```powershell
arduino-cli upload -p COM5 --fqbn esp32:esp32:esp32 esp32_water_valve
```

The Wi-Fi build is larger (TLS + Wi-Fi ≈ 1.05 MB / 80% of the default app
partition) but still fits. The cellular build is ≈ 460 KB.

## First-time provisioning

Open the serial monitor at **115200** baud, line ending **Newline**.
Provisioning is stored in NVS and survives reboots.

1. Create the household/device with Worker setup (once per household):

```powershell
$body = @{
  householdName = 'Home'
  deviceName    = 'Valve 1'
  password      = '<household-password>'
  timezone      = 'Asia/Bangkok'
} | ConvertTo-Json

Invoke-RestMethod -Method Post `
  -Uri 'https://water-valve-api-v2.czqwertyyy.workers.dev/v1/setup' `
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
set url https://water-valve-api-v2.czqwertyyy.workers.dev
set mqttuser <hivemq-user>
set mqttpass <hivemq-password>
set maxopen 30
sync
status
reboot
```

For the Wi-Fi bench build only:

```text
set ssid <wifi-name>
set pass <wifi-password>
```

Notes:

- `set url` strips trailing slashes.
- `set maxopen` accepts 1–240 minutes; the default is 30.
- `status` prints secrets masked.
- `sync` forces an immediate HTTPS sync.
- The default Worker URL and MQTT host are compiled in but can be
  overridden with `set url` and `set mqttuser`/`set mqttpass`.
- The default APN is `internet`; change it for your carrier if needed.

## Serial commands

| Command | Meaning |
|---|---|
| `help` | Show the command list |
| `show` | Print configuration (secrets masked) |
| `set token <dev_...>` | Device token from `/v1/setup` |
| `set apn <name>` | Cellular APN |
| `set apnuser <u>` / `set apnpass <p>` | APN credentials |
| `set ssid <name>` / `set pass <p>` | Wi-Fi bench only |
| `set url <https://...>` | Worker base URL |
| `set mqttuser <u>` / `set mqttpass <p>` | MQTT credentials |
| `set maxopen <1..240>` | Local maximum open time |
| `sync` | Force HTTPS config/command sync |
| `status` | Publish MQTT status and print config |
| `reboot` | Restart the ESP32 |

## Timezone

Schedules and NTP use a fixed **UTC+7 (Asia/Bangkok)** offset
(`GMT_OFFSET_SEC` in the sketch). The Worker household has a `timezone`
field, but the firmware does not read it yet. If the house is not in
UTC+7, change `GMT_OFFSET_SEC` and redeploy. The Android app renders
schedule times in the phone's local timezone, so the phone must use the
same timezone as the valve.

## MQTT topics

> **v1 isolation warning:** topics are global (`myhouse/valve/...`) and the
> HiveMQ user is shared. Deploy only one household/valve per MQTT broker until
> per-device topics or scoped credentials are implemented. See
> `../docs/SECURITY.md`.

MQTT is optional for v1: if `mqttuser`/`mqttpass` are empty, the device still
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
`/v1/commands` so the command is queued in D1 and delivered by the next
sync. Do not "fix" this by opening two TinyGSM secure clients: the modem
session and `sockets[0]` routing will collide.

## Data budget (399 MB/month SIM)

The single-session design means a fresh TLS handshake for every sync and
MQTT reconnect. A realistic estimate:

| Traffic | Estimate |
|---|---|
| HTTPS sync every 15 min (handshake + request/response) | 30–60 MB/month |
| MQTT reconnect after each sync | 20–40 MB/month |
| Status every 5 min | 3–5 MB/month |
| Commands/acks | < 1 MB/month |
| **Total** | **~55–105 MB/month** |

This leaves ample headroom under 399 MB. If the carrier charges for
signaling or the modem retries aggressively, check the SIM dashboard
after the first week and increase `SYNC_INTERVAL_MS` if needed.

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

The firmware trusts **GlobalSign ECC Root CA - R4**, the root currently
used by `workers.dev` (leaf → Google Trust Services WE1 → GlobalSign ECC
Root CA R4). If Cloudflare changes the chain:

1. Get the new root CA PEM.
2. Replace the `ROOT_CA` string in `esp32_water_valve.ino`.
3. Reflash. The firmware re-uploads it to the modem as `cfroot.pem` at boot.

The modem certificate file is small (~700 bytes) and is uploaded once per
boot, so a factory-reset modem recovers automatically.

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
- `README.md` — this file
- Worker API: `../worker/v2/index.js`
- Schema: `../migrations/0001_init.sql`
- Architecture: `../docs/ARCHITECTURE_CENTRAL_CONFIG.md`
