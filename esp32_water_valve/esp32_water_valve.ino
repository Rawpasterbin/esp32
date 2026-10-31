/*
 * Water Valve Controller v2
 * ------------------------------------------------------------------
 * ESP32 + A7670/A7672X (4G) or Wi-Fi (bench only) + relay on GPIO 23.
 *
 * Source of truth : Cloudflare Worker v2 + D1
 * Realtime channel: MQTT over TLS 8883 (HiveMQ Cloud)
 * Config sync     : HTTPS POST /v1/devices/sync every 15 minutes
 *
 * Safety:
 *  - Relay is driven to CLOSED at boot.
 *  - Every command carries a TTL (20 min for the HTTPS queue, optional for
 *    MQTT) and a commandId (idempotent, last 8 IDs remembered).
 *  - MAX_OPEN_MINUTES is enforced locally, even with no network.
 *  - Overnight schedules (e.g. 22:00-06:00) use the previous day's flag
 *    for the after-midnight part.
 *
 * Provisioning (serial 115200):
 *   help
 *   show
 *   set token dev_xxxxxxxx
 *   set apn internet
 *   set apnuser <user>
 *   set apnpass <pass>
 *   set ssid <wifi-name>          (bench only)
 *   set pass <wifi-password>      (bench only)
 *   set url https://your-worker.workers.dev
 *   set mqttuser <user>
 *   set mqttpass <pass>
 *   set maxopen 30
 *   sync
 *   status
 *   reboot
 *
 * Build flags:
 *   USE_CELLULAR=1 (default) -> A7672X on Serial2 RX16/TX17
 *   USE_CELLULAR=0           -> Wi-Fi (bench testing)
 *
 * Libraries: PubSubClient, ArduinoJson >= 7, TinyGSM >= 0.12
 */

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <esp_task_wdt.h>
#include <sys/time.h>
#include <time.h>

#ifndef USE_CELLULAR
#define USE_CELLULAR 1
#endif

#if USE_CELLULAR
#define TINY_GSM_MODEM_A7672X
#define TINY_GSM_RX_BUFFER 1024
#include <TinyGsmClient.h>
#else
#include <WiFi.h>
#include <WiFiClientSecure.h>
#endif

/* ===================== Hardware / defaults ===================== */

#define FIRMWARE_VERSION "2.1.0"

const int RELAY_PIN = 23;
// VERIFY ON THE BENCH: HIGH = open on the current relay module.
// If the module is active-low, set this to 0.
#define RELAY_ACTIVE_HIGH 1

const long GMT_OFFSET_SEC = 7 * 3600; // Asia/Bangkok
const long DAYLIGHT_OFFSET_SEC = 0;

const char *DEFAULT_WORKER_URL = "https://water-valve-api-v2.czqwertyyy.workers.dev";
const char *DEFAULT_MQTT_HOST = "74bd39d864644eebb6efba20dadd4e93.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORT = 8883;

const char *CONTROL_TOPIC = "myhouse/valve/control";
const char *MODE_TOPIC = "myhouse/valve/mode";
const char *SCHEDULE_TOPIC = "myhouse/valve/schedule";
const char *STATUS_TOPIC = "myhouse/valve/status";

/* ===================== Timing / limits ===================== */

const unsigned long SCHEDULE_CHECK_MS = 10000;      // 10 s local schedule engine
const unsigned long STATUS_INTERVAL_MS = 5UL * 60UL * 1000UL;   // 5 min
const unsigned long SYNC_INTERVAL_MS = 15UL * 60UL * 1000UL;    // 15 min
const unsigned long SYNC_RETRY_MS = 60UL * 1000UL;              // retry failed sync
const unsigned long MQTT_RETRY_MIN_MS = 10UL * 1000UL;
const unsigned long MQTT_RETRY_MAX_MS = 5UL * 60UL * 1000UL;
const unsigned long CLOCK_RETRY_MS = 30UL * 1000UL;
const uint16_t MQTT_BUFFER_SIZE = 1024;
const uint8_t MAX_SCHEDULES = 8;
const uint8_t RECENT_COMMAND_SLOTS = 8;
const uint16_t DEFAULT_MAX_OPEN_MINUTES = 30;
const uint16_t COMMAND_TTL_GRACE_S = 5; // allow small clock skew

/* Cloudflare's current workers.dev chain is rooted at
 * GlobalSign ECC Root CA - R4 (valid until 2038).
 * Replace this PEM if Cloudflare rotates the CA. */
static const char ROOT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----
MIIB3DCCAYOgAwIBAgINAgPlfvU/k/2lCSGypjAKBggqhkjOPQQDAjBQMSQwIgYDVQQLExtHbG9i
YWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkds
b2JhbFNpZ24wHhcNMTIxMTEzMDAwMDAwWhcNMzgwMTE5MDMxNDA3WjBQMSQwIgYDVQQLExtHbG9i
YWxTaWduIEVDQyBSb290IENBIC0gUjQxEzARBgNVBAoTCkdsb2JhbFNpZ24xEzARBgNVBAMTCkds
b2JhbFNpZ24wWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAAS4xnnTj2wlDp8uORkcA6SumuU5BwkW
ymOxuYb4ilfBV85C+nOh92VC/x7BALJucw7/xyHlGKSq2XE/qNS5zowdo0IwQDAOBgNVHQ8BAf8E
BAMCAYYwDwYDVR0TAQH/BAUwAwEB/zAdBgNVHQ4EFgQUVLB7rUW44kB/+wpu+74zyTyjhNUwCgYI
KoZIzj0EAwIDRwAwRAIgIk90crlgr/HmnKAWBVBfw147bmF0774BxL4YSFlhgjICICadVGNA3jdg
UM/I2O2dgq43mLyjj0xMqTQrbO/7lZsm
-----END CERTIFICATE-----
)EOF";

/* ===================== Network clients ===================== */

#if USE_CELLULAR
HardwareSerial SerialAT(2);       // RX = 16, TX = 17
TinyGsm modem(SerialAT);
// The A7672X TinyGSM driver only exposes a single TLS session (session 0),
// so MQTT and HTTPS must share one client and never run at the same time.
TinyGsmClientSecure net(modem, 0);
#else
WiFiClientSecure net;
#endif

PubSubClient mqtt(net);
Preferences prefs;

/* ===================== Runtime state ===================== */

struct ScheduleRule {
  bool enabled;
  uint8_t days;       // bit0 = Monday ... bit6 = Sunday
  uint16_t startMin;  // minutes after midnight
  uint16_t endMin;
};

ScheduleRule rules[MAX_SCHEDULES];
uint8_t ruleCount = 0;

String valveMode = "manual";
String schedulesJson = "";
String workerUrl = DEFAULT_WORKER_URL;
String deviceToken = "";
String mqttHost = DEFAULT_MQTT_HOST;
String mqttUser = "";
String mqttPass = "";
String apn = "internet";
String apnUser = "";
String apnPass = "";
String wifiSsid = "";
String wifiPass = "";
String lastCommandId = "";
String recentCommandIds[RECENT_COMMAND_SLOTS];
uint8_t recentCommandCount = 0;
uint8_t recentCommandNext = 0;
String lastRejectedId = "";
String lastRejectedError = "";
String fault = "";

bool valveOn = false;
bool maxRuntimeLatched = false;
bool tlsReady = false;
bool modemReady = false;

uint32_t configRevision = 0;
uint16_t maxOpenMinutes = DEFAULT_MAX_OPEN_MINUTES;

unsigned long valveOpenedAt = 0;
unsigned long lastScheduleCheck = 0;
unsigned long lastStatusPublish = 0;
unsigned long lastSyncAt = 0;
unsigned long lastSyncAttempt = 0;
unsigned long lastMqttAttempt = 0;
unsigned long lastClockAttempt = 0;
unsigned long lastModemMaintain = 0;
unsigned long mqttRetryMs = MQTT_RETRY_MIN_MS;

/* ===================== Small helpers ===================== */

bool timeSynced() {
  return time(nullptr) > 1700000000; // after Nov 2023
}

String chipId() {
  uint32_t low = (uint32_t)(ESP.getEfuseMac() & 0xFFFFFFFFULL);
  return String(low, HEX);
}

String maskSecret(const String &value) {
  if (value.isEmpty()) return "(empty)";
  if (value.length() <= 6) return "***";
  return value.substring(0, 3) + "..." + value.substring(value.length() - 3);
}

void saveString(const char *key, const String &value) {
  prefs.putString(key, value);
}

void loadString(const char *key, String &target, const char *fallback = "") {
  target = prefs.getString(key, fallback);
}

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
long daysFromCivil(int y, int m, int d) {
  y -= (m <= 2);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (long)era * 146097 + (long)doe - 719468;
}

// Parses "2026-10-29T12:00:00.000Z" (UTC) into epoch seconds.
time_t isoToEpoch(const char *iso) {
  if (!iso || !*iso) return 0;
  int y, mo, d, h, mi, se;
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
  if (y < 2020 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  long days = daysFromCivil(y, mo, d);
  return (time_t)days * 86400 + h * 3600 + mi * 60 + se;
}

void setSystemTimeUtc(time_t epoch) {
  if (epoch <= 1700000000) return;
  struct timeval tv;
  tv.tv_sec = epoch;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  Serial.print("Clock set to epoch ");
  Serial.println((long)epoch);
}

bool syncClockFromModem() {
#if USE_CELLULAR
  int y, mo, d, h, mi, se;
  float tz = 0;
  if (!modem.getNetworkTime(&y, &mo, &d, &h, &mi, &se, &tz)) return false;
  long days = daysFromCivil(y, mo, d);
  time_t epoch = (time_t)days * 86400 + h * 3600 + mi * 60 + se - (time_t)(tz * 3600.0f);
  setSystemTimeUtc(epoch);
  return true;
#else
  return false;
#endif
}

/* ===================== Schedules ===================== */

bool parseHhMm(JsonVariantConst value, uint16_t &minutes) {
  if (!value.is<const char *>()) return false;
  const char *text = value.as<const char *>();
  if (!text || strlen(text) < 5) return false;
  int h = (text[0] - '0') * 10 + (text[1] - '0');
  int m = (text[3] - '0') * 10 + (text[4] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) return false;
  minutes = (uint16_t)(h * 60 + m);
  return true;
}

bool parseSchedules(const String &json) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) {
    Serial.print("Schedules: JSON error ");
    Serial.println(err.c_str());
    return false;
  }
  JsonArray array = doc.as<JsonArray>();
  if (array.isNull()) return false;

  uint8_t count = 0;
  for (JsonObject item : array) {
    if (count >= MAX_SCHEDULES) {
      Serial.println("Schedules: too many items, extra ignored");
      break;
    }
    ScheduleRule rule;
    rule.enabled = item["enabled"] | false;
    rule.days = 0;
    uint16_t start = 0, end = 0;
    if (!parseHhMm(item["start"], start) || !parseHhMm(item["end"], end)) {
      Serial.println("Schedules: invalid HH:MM, item skipped");
      continue;
    }
    rule.startMin = start;
    rule.endMin = end;

    JsonArray days = item["days"].as<JsonArray>();
    if (!days.isNull()) {
      uint8_t i = 0;
      for (JsonVariant v : days) {
        if (i >= 7) break;
        if (v.as<bool>()) rule.days |= (uint8_t)(1u << i);
        i++;
      }
    }
    rules[count++] = rule;
  }
  ruleCount = count;
  Serial.print("Schedules: loaded ");
  Serial.println(ruleCount);
  return true;
}

bool scheduleActiveNow(time_t now) {
  if (valveMode != "auto" || ruleCount == 0) return false;
  struct tm tinfo;
  localtime_r(&now, &tinfo);
  int nowMin = tinfo.tm_hour * 60 + tinfo.tm_min;
  int today = (tinfo.tm_wday + 6) % 7;       // Monday = 0 ... Sunday = 6
  int yesterday = (today + 6) % 7;

  for (uint8_t i = 0; i < ruleCount; i++) {
    const ScheduleRule &rule = rules[i];
    if (!rule.enabled) continue;

    if (rule.startMin <= rule.endMin) {
      // Same-day window, e.g. 06:00-06:30.
      if ((rule.days & (1u << today)) && nowMin >= rule.startMin && nowMin < rule.endMin) {
        return true;
      }
    } else {
      // Overnight window, e.g. 22:00-06:00:
      // - before midnight belongs to today's flag
      // - after midnight belongs to yesterday's flag
      if ((rule.days & (1u << today)) && nowMin >= rule.startMin) return true;
      if ((rule.days & (1u << yesterday)) && nowMin < rule.endMin) return true;
    }
  }
  return false;
}

void saveSchedules() {
  prefs.putString("schedules", schedulesJson);
  prefs.putUInt("revision", configRevision);
}

/* Recent command IDs survive reboots so a QoS1 redelivery of an old MQTT
 * command cannot reopen the valve after a newer close command. */
void saveRecentCommands() {
  JsonDocument doc;
  JsonArray array = doc.to<JsonArray>();
  for (uint8_t i = 0; i < recentCommandCount; i++) {
    array.add(recentCommandIds[i]);
  }
  String json;
  serializeJson(doc, json);
  prefs.putString("recentCmds", json);
}

void loadRecentCommands() {
  recentCommandCount = 0;
  recentCommandNext = 0;

  String json = prefs.getString("recentCmds", "");
  if (json.isEmpty()) return;

  JsonDocument doc;
  if (deserializeJson(doc, json)) return;
  JsonArray array = doc.as<JsonArray>();
  if (array.isNull()) return;

  for (JsonVariant value : array) {
    if (recentCommandCount >= RECENT_COMMAND_SLOTS) break;
    String id = value.as<String>();
    if (!id.isEmpty()) recentCommandIds[recentCommandCount++] = id;
  }
  recentCommandNext = recentCommandCount % RECENT_COMMAND_SLOTS;
}

bool commandAlreadyExecuted(const String &id) {
  if (id.isEmpty()) return false;
  if (id == lastCommandId) return true;
  for (uint8_t i = 0; i < recentCommandCount; i++) {
    if (recentCommandIds[i] == id) return true;
  }
  return false;
}

void rememberCommand(const String &id) {
  if (id.isEmpty()) return;

  bool known = commandAlreadyExecuted(id);
  lastCommandId = id;
  prefs.putString("lastCmdId", lastCommandId);
  if (known) return;

  recentCommandIds[recentCommandNext] = id;
  recentCommandNext = (recentCommandNext + 1) % RECENT_COMMAND_SLOTS;
  if (recentCommandCount < RECENT_COMMAND_SLOTS) recentCommandCount++;
  saveRecentCommands();
}

/* ===================== Valve ===================== */

void publishStatus();

void applyRelay(bool on) {
#if RELAY_ACTIVE_HIGH
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
#else
  digitalWrite(RELAY_PIN, on ? LOW : HIGH);
#endif
}

void setValve(bool on, const char *reason) {
  if (valveOn == on) return;
  valveOn = on;
  applyRelay(on);
  if (on) {
    valveOpenedAt = millis();
    maxRuntimeLatched = false;
  }
  Serial.print(on ? "Valve OPEN (" : "Valve CLOSED (");
  Serial.print(reason);
  Serial.println(")");
  publishStatus();
}

void checkMaxRuntime() {
  if (!valveOn || maxRuntimeLatched) return;
  unsigned long limitMs = (unsigned long)maxOpenMinutes * 60UL * 1000UL;
  if (millis() - valveOpenedAt < limitMs) return;

  maxRuntimeLatched = true;
  fault = "max_runtime";
  Serial.println("SAFETY: max runtime reached, closing valve");
  setValve(false, "max_runtime");
}

void autoCheck() {
  if (valveMode != "auto") return;
  time_t now = time(nullptr);
  if (!timeSynced()) return;

  bool active = scheduleActiveNow(now);
  if (active) {
    if (!maxRuntimeLatched) setValve(true, "schedule");
  } else {
    if (maxRuntimeLatched) {
      maxRuntimeLatched = false;
      if (fault == "max_runtime") fault = "";
    }
    setValve(false, "schedule");
  }
}

/* ===================== Network ===================== */

bool networkConnected() {
#if USE_CELLULAR
  return modemReady && modem.isGprsConnected();
#else
  return WiFi.status() == WL_CONNECTED;
#endif
}

int currentRssi() {
#if USE_CELLULAR
  if (!modemReady) return 0;
  int16_t csq = modem.getSignalQuality();
  if (csq == 99 || csq < 0 || csq > 31) return 0;
  return -113 + 2 * csq; // CSQ -> dBm
#else
  return WiFi.RSSI();
#endif
}

bool connectNetwork() {
#if USE_CELLULAR
  if (!modemReady) return false;
  if (modem.isGprsConnected()) return true;

  Serial.print("GPRS connect, APN=");
  Serial.println(apn);
  if (!modem.gprsConnect(apn.c_str(), apnUser.c_str(), apnPass.c_str())) {
    Serial.println("GPRS connect failed");
    return false;
  }
  Serial.println("GPRS connected");
  return modem.isGprsConnected();
#else
  if (WiFi.status() == WL_CONNECTED) return true;
  if (wifiSsid.isEmpty()) {
    Serial.println("WiFi: no SSID provisioned");
    return false;
  }
  Serial.print("WiFi connect: ");
  Serial.println(wifiSsid);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());
    return true;
  }
  Serial.println("WiFi connect failed");
  return false;
#endif
}

void configureTls() {
  if (tlsReady) return;
#if USE_CELLULAR
  // The A7672X keeps certificates in its own file system. Upload once per
  // boot (small, ~700 bytes) so a factory-reset modem recovers automatically.
  bool uploaded = net.addCertificate("cfroot.pem", ROOT_CA, (uint16_t)strlen(ROOT_CA));
  if (!uploaded) {
    Serial.println("TLS: CA upload failed (continuing; modem may reject TLS)");
  }
  net.setCertificate("cfroot.pem");
#else
  net.setCACert(ROOT_CA);
#endif
  tlsReady = true;
}

void ensureClock() {
  if (timeSynced()) return;
  if (millis() - lastClockAttempt < CLOCK_RETRY_MS) return;
  lastClockAttempt = millis();

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.google.com");
#if USE_CELLULAR
  // NTP may be blocked by some carriers; fall back to the modem's +CCLK.
  if (!timeSynced()) syncClockFromModem();
#endif
}

/* ===================== Worker v2 sync ===================== */

bool executeCommand(const String &action, const String &commandId,
                    const String &expiresAt, bool requireTtl, String &error) {
  // Idempotency must win over TTL: an already-executed command that is
  // redelivered later must be acked, not reported as expired/failed.
  if (commandAlreadyExecuted(commandId)) {
    return true;
  }

  time_t now = time(nullptr);
  time_t expires = isoToEpoch(expiresAt.c_str());

  if (requireTtl) {
    if (expires == 0) {
      error = "missing_ttl";
      return false;
    }
    if (!timeSynced()) {
      error = "no_clock";
      return false;
    }
    if (now > expires + COMMAND_TTL_GRACE_S) {
      error = "expired";
      return false;
    }
  }

  bool open;
  if (action == "open" || action == "on") {
    open = true;
  } else if (action == "close" || action == "off") {
    open = false;
  } else {
    error = "unknown_action";
    return false;
  }

  // Auto mode owns opening. Closing is always allowed so a manual close can
  // stop the valve even after the household switched to auto mode.
  if (open && valveMode == "auto") {
    error = "auto_mode";
    return false;
  }

  if (open) {
    maxRuntimeLatched = false;
    if (fault == "max_runtime") fault = "";
  }
  setValve(open, commandId.isEmpty() ? "mqtt" : "command");
  rememberCommand(commandId);
  return true;
}

/* ===================== Minimal HTTPS client =====================
 * ESP32 core 3.x HTTPClient requires NetworkClient, which TinyGSM's
 * GsmClient does not implement. This small client speaks HTTP/1.1 over
 * the shared TLS socket and understands Content-Length + chunked bodies.
 */

// Globals instead of a struct parameter: the Arduino preprocessor inserts
// function prototypes before local type definitions.
int httpStatus = 0;
String httpBody = "";

bool decodeChunkedBody(const String &raw, size_t start, String &body) {
  size_t pos = start;
  body = "";
  while (pos < raw.length()) {
    int lineEnd = raw.indexOf("\r\n", pos);
    if (lineEnd < 0) return false;
    String sizeLine = raw.substring(pos, lineEnd);
    int semi = sizeLine.indexOf(';');
    if (semi >= 0) sizeLine = sizeLine.substring(0, semi);
    sizeLine.trim();
    if (sizeLine.isEmpty()) return false;
    char *endPtr = nullptr;
    long chunkSize = strtol(sizeLine.c_str(), &endPtr, 16);
    if (endPtr == sizeLine.c_str() || chunkSize < 0) return false;
    pos = (size_t)lineEnd + 2;
    if (chunkSize == 0) {
      return pos + 2 <= raw.length(); // final CRLF
    }
    if (pos + (size_t)chunkSize + 2 > raw.length()) return false;
    body += raw.substring(pos, pos + (size_t)chunkSize);
    pos += (size_t)chunkSize + 2;
  }
  return false;
}

bool httpPostJson(const String &url, const String &payload, const String &bearer) {
  httpStatus = 0;
  httpBody = "";

  if (!url.startsWith("https://")) {
    Serial.println("HTTP: only https:// URLs are supported");
    return false;
  }

  String rest = url.substring(8);
  int slash = rest.indexOf('/');
  String host = (slash >= 0) ? rest.substring(0, slash) : rest;
  String path = (slash >= 0) ? rest.substring(slash) : "/";
  int colon = host.indexOf(':');
  uint16_t port = 443;
  if (colon >= 0) {
    port = (uint16_t)host.substring(colon + 1).toInt();
    host = host.substring(0, colon);
  }
  if (host.isEmpty()) return false;

  net.setTimeout(20000);
#if USE_CELLULAR
  if (!net.connect(host.c_str(), port, 20)) {
#else
  if (!net.connect(host.c_str(), port)) {
#endif
    Serial.print("HTTP: connect failed: ");
    Serial.println(host);
    return false;
  }

  String request;
  request.reserve(320 + payload.length());
  request += "POST ";
  request += path;
  request += " HTTP/1.1\r\nHost: ";
  request += host;
  request += "\r\nAuthorization: Bearer ";
  request += bearer;
  request += "\r\nContent-Type: application/json\r\nContent-Length: ";
  request += String(payload.length());
  request += "\r\nConnection: close\r\n\r\n";
  request += payload;

  size_t sent = net.write(reinterpret_cast<const uint8_t *>(request.c_str()), request.length());
  if (sent != request.length()) {
    Serial.println("HTTP: short write");
    net.stop();
    return false;
  }

  String raw;
  raw.reserve(2048);
  const unsigned long totalTimeout = 25000;
  const unsigned long idleTimeout = 8000;
  unsigned long started = millis();
  unsigned long lastData = millis();
  int headerEnd = -1;
  long contentLength = -1;
  bool chunked = false;
  size_t bodyStart = 0;
  bool complete = false;

  while (millis() - started < totalTimeout) {
    bool gotData = false;
    while (net.available()) {
      int c = net.read();
      if (c < 0) break;
      raw += (char)c;
      gotData = true;
      if (raw.length() > 16384) break;
    }
    if (gotData) lastData = millis();
    if (raw.length() > 16384) {
      Serial.println("HTTP: response too large");
      break;
    }

    if (headerEnd < 0) {
      headerEnd = raw.indexOf("\r\n\r\n");
      if (headerEnd >= 0) {
        bodyStart = (size_t)headerEnd + 4;
        int sp = raw.indexOf(' ');
        if (sp > 0) httpStatus = raw.substring(sp + 1, sp + 4).toInt();
        String headers = raw.substring(0, headerEnd);
        headers.toLowerCase();
        int cl = headers.indexOf("content-length:");
        if (cl >= 0) contentLength = headers.substring(cl + 15).toInt();
        chunked = headers.indexOf("transfer-encoding: chunked") >= 0;
      }
    }

    if (headerEnd >= 0) {
      if (chunked) {
        complete = decodeChunkedBody(raw, bodyStart, httpBody);
      } else if (contentLength >= 0) {
        if (raw.length() >= bodyStart + (size_t)contentLength) {
          httpBody = raw.substring(bodyStart, bodyStart + (size_t)contentLength);
          complete = true;
        }
      } else if (!net.connected() && !net.available()) {
        httpBody = raw.substring(bodyStart);
        complete = true;
      }
    }

    if (complete) break;
    if (!net.connected() && !net.available()) {
      // Server closed. Use whatever arrived; Cloudflare normally sends
      // Content-Length or chunked framing, so this is only a fallback.
      if (headerEnd >= 0 && !chunked && contentLength < 0) {
        httpBody = raw.substring(bodyStart);
      }
      break;
    }
    if (millis() - lastData > idleTimeout) {
      Serial.println("HTTP: response timeout");
      break;
    }
    delay(20);
  }

  net.stop();

  if (httpStatus > 0 && !complete && httpBody.isEmpty() && headerEnd >= 0) {
    httpBody = raw.substring(bodyStart);
  }
  return httpStatus > 0;
}

bool ackCommand(const String &commandId, const char *status, const String &error) {
  if (commandId.isEmpty() || deviceToken.isEmpty() || !networkConnected()) return false;

  String url = workerUrl + "/v1/devices/commands/" + commandId + "/ack";
  JsonDocument doc;
  doc["status"] = status;
  if (error.isEmpty()) {
    doc["error"] = nullptr;
  } else {
    doc["error"] = error;
  }
  String body;
  serializeJson(doc, body);

  bool posted = httpPostJson(url, body, deviceToken);

  Serial.print("Ack ");
  Serial.print(commandId);
  Serial.print(" -> ");
  Serial.println(httpStatus);
  return posted && httpStatus == 200;
}

bool applySyncPayload(const String &payload) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("Sync: JSON error ");
    Serial.println(err.c_str());
    return false;
  }
  if (!(doc["ok"] | false)) {
    Serial.println("Sync: API returned ok=false");
    return false;
  }

  JsonObject data = doc["data"].as<JsonObject>();
  if (data.isNull()) return false;

  uint32_t revision = data["revision"] | 0;
  const char *mode = data["mode"] | "manual";

  if (strcmp(mode, "auto") == 0 || strcmp(mode, "manual") == 0) {
    if (valveMode != mode) {
      valveMode = mode;
      prefs.putString("mode", valveMode);
      Serial.print("Mode from server: ");
      Serial.println(valveMode);
    }
  }

  if (data["schedules"].is<JsonArray>()) {
    String newSchedules;
    serializeJson(data["schedules"], newSchedules);
    if (newSchedules != schedulesJson) {
      schedulesJson = newSchedules;
      parseSchedules(schedulesJson);
      saveSchedules();
      Serial.println("Schedules updated from server");
    }
  }

  configRevision = revision;
  prefs.putUInt("revision", configRevision);

  // Optional server clock (ISO UTC) — useful before NTP succeeds.
  const char *serverTime = data["serverTime"] | "";
  if (!timeSynced() && serverTime && *serverTime) {
    setSystemTimeUtc(isoToEpoch(serverTime));
  }

  if (data["command"].is<JsonObject>()) {
    JsonObject cmd = data["command"].as<JsonObject>();
    String id = cmd["id"] | "";
    String action = cmd["action"] | "";
    String expiresAt = cmd["expiresAt"] | "";

    String error;
    bool ok = executeCommand(action, id, expiresAt, true, error);
    if (ok) {
      ackCommand(id, "acked", "");
    } else {
      Serial.print("Command rejected: ");
      Serial.println(error);
      ackCommand(id, strcmp(error.c_str(), "expired") == 0 ? "expired" : "failed", error);
    }
  }

  autoCheck();
  return true;
}

bool deviceSync() {
  if (deviceToken.isEmpty()) {
    Serial.println("Sync skipped: no device token (use: set token dev_...)");
    return false;
  }
  if (!networkConnected()) {
    Serial.println("Sync skipped: no network");
    return false;
  }

  JsonDocument doc;
  doc["valve"] = valveOn;
  doc["mode"] = valveMode;
  doc["rssi"] = currentRssi();
  doc["uptimeSeconds"] = (uint32_t)(millis() / 1000UL);
  doc["firmwareVersion"] = FIRMWARE_VERSION;
  if (lastCommandId.isEmpty()) {
    doc["lastCommandId"] = nullptr;
  } else {
    doc["lastCommandId"] = lastCommandId;
  }
  if (fault.isEmpty()) {
    doc["fault"] = nullptr;
  } else {
    doc["fault"] = fault;
  }
  String body;
  serializeJson(doc, body);

  String url = workerUrl + "/v1/devices/sync";

  // A7672X exposes only one TLS session. Pause MQTT while HTTPS runs;
  // the loop reconnects immediately afterwards.
  if (mqtt.connected()) {
    mqtt.disconnect();
    delay(300);
  }

  bool posted = httpPostJson(url, body, deviceToken);

  Serial.print("Sync HTTP ");
  Serial.println(httpStatus);

  lastMqttAttempt = 0;
  mqttRetryMs = MQTT_RETRY_MIN_MS;

  if (!posted) return false;
  if (httpStatus == 401) {
    Serial.println("Sync: device token rejected");
    return false;
  }
  if (httpStatus != 200) return false;

  return applySyncPayload(httpBody);
}

/* ===================== MQTT ===================== */

void publishStatus() {
  if (!mqtt.connected()) return;

  JsonDocument doc;
  doc["valve"] = valveOn;
  doc["online"] = true;
  doc["mode"] = valveMode;
  doc["scheduleCount"] = ruleCount;
  doc["ntp"] = timeSynced();
  doc["time"] = (long)time(nullptr);
  doc["rssi"] = currentRssi();
  doc["uptime"] = (uint32_t)(millis() / 1000UL);
  doc["fw"] = FIRMWARE_VERSION;
  if (fault.isEmpty()) {
    doc["fault"] = nullptr;
  } else {
    doc["fault"] = fault;
  }
  if (!lastCommandId.isEmpty()) doc["lastCommandId"] = lastCommandId;
  if (!lastRejectedId.isEmpty()) {
    doc["lastRejectedId"] = lastRejectedId;
    doc["lastRejectedError"] = lastRejectedError;
  }

  char buffer[768];
  size_t len = serializeJson(doc, buffer, sizeof(buffer));
  mqtt.publish(STATUS_TOPIC, buffer, len);
}

void handleMqttCommand(const String &message) {
  String action;
  String commandId;
  String expiresAt;
  bool requireTtl = false;

  if (message == "ON") {
    action = "open";
  } else if (message == "OFF") {
    action = "close";
  } else {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, message);
    if (err) {
      Serial.println("MQTT: invalid JSON");
      return;
    }

    const char *cmd = doc["cmd"] | "";
    const char *act = doc["action"] | "";
    if (strcmp(cmd, "open") == 0 || strcmp(cmd, "close") == 0 ||
        strcmp(cmd, "toggle") == 0 || strcmp(cmd, "getStatus") == 0 ||
        strcmp(cmd, "sync") == 0 || strcmp(cmd, "reboot") == 0) {
      action = cmd;
    } else if (strcmp(act, "on") == 0) {
      action = "open";
    } else if (strcmp(act, "off") == 0) {
      action = "close";
    } else {
      action = cmd;
    }

    commandId = doc["commandId"] | "";
    expiresAt = doc["expiresAt"] | "";
    requireTtl = !expiresAt.isEmpty();
  }

  if (action == "getStatus") {
    publishStatus();
    return;
  }
  if (action == "sync") {
    lastSyncAt = 0; // force on next loop
    return;
  }
  if (action == "reboot") {
    delay(100);
    ESP.restart();
  }
  if (action == "toggle") {
    action = valveOn ? "close" : "open";
  }

  String error;
  bool ok = executeCommand(action, commandId, expiresAt, requireTtl, error);
  if (!ok) {
    Serial.print("MQTT command rejected: ");
    Serial.println(error);
    if (!commandId.isEmpty()) {
      lastRejectedId = commandId;
      lastRejectedError = error;
    }
  } else if (!commandId.isEmpty()) {
    lastRejectedId = "";
    lastRejectedError = "";
  }

  // MQTT holds the only TLS session, so the HTTP ack endpoint cannot be
  // called here. Status is the realtime ack (lastCommandId / lastRejectedId);
  // the next device sync also auto-acks lastCommandId in D1.
  publishStatus();
}

void mqttCallback(char *topic, byte *payload, unsigned int length) {
  String message;
  message.reserve(length + 1);
  for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();

  String t = String(topic);
  Serial.print("MQTT [");
  Serial.print(t);
  Serial.print("]: ");
  Serial.println(message);

  if (t == CONTROL_TOPIC) {
    handleMqttCommand(message);
    return;
  }

  // Legacy compatibility: the old web UI published mode/schedule here.
  // Worker v2 is the source of truth; these values will be overwritten by
  // the next 15-minute sync. Prefer changing mode/schedule through the API.
  if (t == MODE_TOPIC) {
    JsonDocument doc;
    if (deserializeJson(doc, message)) return;
    const char *mode = doc["mode"] | "";
    if (strcmp(mode, "auto") == 0 || strcmp(mode, "manual") == 0) {
      valveMode = mode;
      prefs.putString("mode", valveMode);
      Serial.print("Legacy MQTT mode -> ");
      Serial.println(valveMode);
      autoCheck();
    }
    return;
  }

  if (t == SCHEDULE_TOPIC) {
    JsonDocument doc;
    if (deserializeJson(doc, message)) return;
    if (doc["schedules"].is<JsonArray>()) {
      String next;
      serializeJson(doc["schedules"], next);
      schedulesJson = next;
      parseSchedules(schedulesJson);
      saveSchedules();
      Serial.println("Legacy MQTT schedules applied (will be replaced by sync)");
      autoCheck();
    }
    return;
  }
}

bool mqttConnect() {
  if (mqttHost.isEmpty() || mqttUser.isEmpty() || mqttPass.isEmpty()) {
    return false;
  }
  if (!networkConnected()) return false;

  String clientId = "valve-" + chipId();
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(20);
  mqtt.setBufferSize(MQTT_BUFFER_SIZE);

  Serial.print("MQTT connect as ");
  Serial.println(clientId);
  bool ok = mqtt.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
  if (!ok) {
    Serial.print("MQTT failed, rc=");
    Serial.println(mqtt.state());
    return false;
  }

  Serial.println("MQTT connected");
  mqtt.subscribe(CONTROL_TOPIC, 1);
  mqtt.subscribe(MODE_TOPIC, 1);
  mqtt.subscribe(SCHEDULE_TOPIC, 1);
  publishStatus();
  return true;
}

/* ===================== Serial provisioning ===================== */

void printHelp() {
  Serial.println();
  Serial.println("Commands:");
  Serial.println("  show");
  Serial.println("  set token <dev_...>");
  Serial.println("  set apn <name> | set apnuser <u> | set apnpass <p>");
  Serial.println("  set ssid <name> | set pass <password>   (Wi-Fi bench only)");
  Serial.println("  set url <https://...>");
  Serial.println("  set mqttuser <u> | set mqttpass <p>");
  Serial.println("  set maxopen <minutes>");
  Serial.println("  sync | status | reboot");
  Serial.println();
}

void printConfig() {
  Serial.println();
  Serial.println("--- config ---");
  Serial.print("fw="); Serial.println(FIRMWARE_VERSION);
  Serial.print("mode="); Serial.println(valveMode);
  Serial.print("revision="); Serial.println(configRevision);
  Serial.print("schedules="); Serial.println(ruleCount);
  Serial.print("valve="); Serial.println(valveOn ? "open" : "closed");
  Serial.print("url="); Serial.println(workerUrl);
  Serial.print("token="); Serial.println(maskSecret(deviceToken));
  Serial.print("apn="); Serial.println(apn);
  Serial.print("mqtt="); Serial.println(mqttHost);
  Serial.print("mqttuser="); Serial.println(maskSecret(mqttUser));
  Serial.print("mqttpass="); Serial.println(maskSecret(mqttPass));
  Serial.print("wifi="); Serial.println(wifiSsid);
  Serial.print("maxopen="); Serial.println(maxOpenMinutes);
  Serial.print("rssi="); Serial.println(currentRssi());
  Serial.print("clock="); Serial.println(timeSynced() ? "ok" : "not synced");
  Serial.println("--------------");
  Serial.println();
}

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.isEmpty()) return;

  if (line == "help") {
    printHelp();
    return;
  }
  if (line == "show") {
    printConfig();
    return;
  }
  if (line == "reboot") {
    Serial.println("Rebooting...");
    delay(200);
    ESP.restart();
  }
  if (line == "sync") {
    Serial.println(deviceSync() ? "Sync OK" : "Sync failed");
    return;
  }
  if (line == "status") {
    publishStatus();
    printConfig();
    return;
  }

  if (line.startsWith("set ")) {
    int firstSpace = line.indexOf(' ', 4);
    if (firstSpace < 0) {
      Serial.println("Usage: set <key> <value>");
      return;
    }
    String key = line.substring(4, firstSpace);
    String value = line.substring(firstSpace + 1);
    value.trim();

    if (key == "token") {
      deviceToken = value;
      prefs.putString("token", deviceToken);
    } else if (key == "apn") {
      apn = value;
      prefs.putString("apn", apn);
    } else if (key == "apnuser") {
      apnUser = value;
      prefs.putString("apnuser", apnUser);
    } else if (key == "apnpass") {
      apnPass = value;
      prefs.putString("apnpass", apnPass);
    } else if (key == "ssid") {
      wifiSsid = value;
      prefs.putString("ssid", wifiSsid);
    } else if (key == "pass") {
      wifiPass = value;
      prefs.putString("wpass", wifiPass);
    } else if (key == "url") {
      while (value.endsWith("/")) value.remove(value.length() - 1);
      workerUrl = value;
      prefs.putString("url", workerUrl);
    } else if (key == "mqttuser") {
      mqttUser = value;
      prefs.putString("mqttuser", mqttUser);
    } else if (key == "mqttpass") {
      mqttPass = value;
      prefs.putString("mqttpass", mqttPass);
    } else if (key == "maxopen") {
      long minutes = value.toInt();
      if (minutes < 1 || minutes > 240) {
        Serial.println("maxopen must be 1..240");
        return;
      }
      maxOpenMinutes = (uint16_t)minutes;
      prefs.putUShort("maxopen", maxOpenMinutes);
    } else {
      Serial.println("Unknown key");
      return;
    }
    Serial.print("Saved ");
    Serial.println(key);
    return;
  }

  Serial.println("Unknown command. Type: help");
}

/* ===================== Setup ===================== */

void loadConfig() {
  prefs.begin("valve", false);

  loadString("mode", valveMode, "manual");
  loadString("schedules", schedulesJson, "");
  loadString("lastCmdId", lastCommandId, "");
  loadRecentCommands();
  if (!lastCommandId.isEmpty()) rememberCommand(lastCommandId);
  loadString("token", deviceToken, "");
  loadString("url", workerUrl, DEFAULT_WORKER_URL);
  loadString("apn", apn, "internet");
  loadString("apnuser", apnUser, "");
  loadString("apnpass", apnPass, "");
  loadString("ssid", wifiSsid, "");
  loadString("wpass", wifiPass, "");
  loadString("mqttuser", mqttUser, "");
  loadString("mqttpass", mqttPass, "");

  configRevision = prefs.getUInt("revision", 0);
  maxOpenMinutes = prefs.getUShort("maxopen", DEFAULT_MAX_OPEN_MINUTES);
  if (maxOpenMinutes < 1 || maxOpenMinutes > 240) maxOpenMinutes = DEFAULT_MAX_OPEN_MINUTES;

  if (!schedulesJson.isEmpty()) parseSchedules(schedulesJson);

  Serial.print("Restored mode=");
  Serial.print(valveMode);
  Serial.print(" schedules=");
  Serial.print(ruleCount);
  Serial.print(" revision=");
  Serial.println(configRevision);
}

void setupWatchdog() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t cfg = {
      .timeout_ms = 120000,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&cfg);
#else
  esp_task_wdt_init(120, true);
#endif
  esp_task_wdt_add(NULL);
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(RELAY_PIN, OUTPUT);
  applyRelay(false); // fail-safe: valve closed at boot

  loadConfig();

  Serial.println();
  Serial.println("==========================================");
  Serial.println(" Water Valve v" FIRMWARE_VERSION);
  Serial.print(" Relay pin: "); Serial.println(RELAY_PIN);
  Serial.print(" Relay active level: "); Serial.println(RELAY_ACTIVE_HIGH ? "HIGH" : "LOW");
  Serial.println("==========================================");
  if (deviceToken.isEmpty()) {
    Serial.println("No device token. Provision with: set token dev_...");
    printHelp();
  }

  setupWatchdog();

#if USE_CELLULAR
  Serial.println("Starting A7672X on Serial2 (RX16/TX17)...");
  SerialAT.begin(115200, SERIAL_8N1, 16, 17);
  delay(3000);
  modemReady = modem.init();
  if (!modemReady) {
    Serial.println("modem.init failed, trying restart...");
    modemReady = modem.restart();
  }
  Serial.println(modemReady ? "Modem ready" : "Modem NOT ready");
#else
  Serial.println("Wi-Fi bench mode");
#endif

  if (connectNetwork()) {
    configureTls();
    ensureClock();
  }

  mqtt.setServer(mqttHost.c_str(), MQTT_PORT);
  mqtt.setCallback(mqttCallback);

  if (timeSynced()) autoCheck();
}

/* ===================== Loop ===================== */

void loop() {
  esp_task_wdt_reset();
  handleSerial();

  // 1. Network (re)connect.
  if (!networkConnected()) {
    if (connectNetwork()) {
      configureTls();
      ensureClock();
      lastSyncAt = 0;       // sync immediately after reconnect
      lastMqttAttempt = 0;  // MQTT immediately after reconnect
    }
  }

  // 2. Clock.
  ensureClock();

  // 3. MQTT (primary realtime channel).
  if (networkConnected()) {
    if (!mqtt.connected()) {
      if (millis() - lastMqttAttempt >= mqttRetryMs) {
        lastMqttAttempt = millis();
        if (mqttConnect()) {
          mqttRetryMs = MQTT_RETRY_MIN_MS;
        } else {
          mqttRetryMs = min(mqttRetryMs * 2, MQTT_RETRY_MAX_MS);
        }
      }
    } else {
      mqtt.loop();
    }
  }

  // 4. Worker config/command sync every 15 min (or retry after 60 s).
  if (networkConnected()) {
    bool due = (lastSyncAt == 0) || (millis() - lastSyncAt >= SYNC_INTERVAL_MS);
    bool retryReady = (lastSyncAttempt == 0) || (millis() - lastSyncAttempt >= SYNC_RETRY_MS);
    if (due && retryReady) {
      lastSyncAttempt = millis();
      if (deviceSync()) {
        lastSyncAt = millis();
      }
    }
  }

  // 5. Local schedule engine (works offline).
  if (millis() - lastScheduleCheck >= SCHEDULE_CHECK_MS) {
    lastScheduleCheck = millis();
    autoCheck();
  }

  // 6. Safety: max runtime.
  checkMaxRuntime();

  // 7. Periodic status.
  if (millis() - lastStatusPublish >= STATUS_INTERVAL_MS) {
    lastStatusPublish = millis();
    publishStatus();
  }

#if USE_CELLULAR
  // 8. Keep the PDP context alive without hammering the modem.
  if (modemReady && millis() - lastModemMaintain >= 60000UL) {
    lastModemMaintain = millis();
    modem.maintain();
  }
#endif

  delay(10);
}
