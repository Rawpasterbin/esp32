#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <time.h>

const int RELAY_PIN = 23; // ขาควบคุมรีเลย์วาล์วน้ำ

const char* ssid = "noonit_2.4G";
const char* password = "0821404025";

const char* broker = "74bd39d864644eebb6efba20dadd4e93.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_username = "test";
const char* mqtt_password = "test1234";

// Timezone ของไทย UTC+7 (ถ้าอยู่ที่อื่นต้องแก้ตรงนี้)
const long GMT_OFFSET_SEC = 7 * 3600;
const long DAYLIGHT_OFFSET_SEC = 0;

const char* CONTROL_TOPIC = "myhouse/valve/control";
const char* MODE_TOPIC = "myhouse/valve/mode";
const char* SCHEDULE_TOPIC = "myhouse/valve/schedule";
const char* STATUS_TOPIC = "myhouse/valve/status";

// ความถี่ตรวจตารางเวลา (10 วิ)
const unsigned long SCHEDULE_CHECK_MS = 10000;

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);
Preferences prefs;

bool valveOn = false;
String valveMode = "manual";   // "auto" | "manual" (บันทึกใน NVS)
String schedulesJson = "";     // JSON array ของตารางเวลา (บันทึกใน NVS)

unsigned long lastScheduleCheck = 0;

/* ============ สถานะ ============ */
// ส่งสถานะเป็น JSON ให้แอปอ่าน (แอปรอรับ {"valve":true/false,...})
void publishStatus() {
  String json = "{\"valve\":" + String(valveOn ? "true" : "false") + ",\"online\":true}";
  mqtt.publish(STATUS_TOPIC, json.c_str());
  Serial.println("Status sent: " + json);
}

void setValve(bool on) {
  if (valveOn == on) return;
  valveOn = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  publishStatus();
  Serial.println(on ? "-> Water Valve: OPEN (เปิดน้ำ)" : "-> Water Valve: CLOSED (ปิดน้ำ)");
}

/* ============ การจัดการตารางเวลา (NVS) ============ */
void saveSchedules() {
  prefs.putString("schedules", schedulesJson);
  Serial.println("Schedules saved: " + schedulesJson);
}

void saveMode() {
  prefs.putString("mode", valveMode);
  Serial.println("Mode saved: " + valveMode);
}

/* ดึงค่า field จาก JSON อย่างง่าย (string / bool / number / array)
   - ค้นหาแบบ "key": เพื่อไม่ให้ไปชนกับค่าอื่น เช่น {"type":"mode","mode":"auto"} */
String extractField(const String& obj, const char* key) {
  String k = String("\"") + key + "\":";
  int i = obj.indexOf(k);
  if (i < 0) return "";
  i += k.length();
  while (i < (int)obj.length() && (obj[i] == ':' || obj[i] == ' ')) i++;
  if (i >= (int)obj.length()) return "";
  char c = obj[i];
  if (c == '"') {
    int end = obj.indexOf('"', i + 1);
    if (end < 0) return "";
    return obj.substring(i + 1, end);
  } else if (c == '[') {
    int depth = 0;
    for (int j = i; j < (int)obj.length(); j++) {
      if (obj[j] == '[') depth++;
      else if (obj[j] == ']') { depth--; if (depth == 0) return obj.substring(i, j + 1); }
    }
    return "";
  } else {
    int j = i;
    while (j < (int)obj.length() && obj[j] != ',' && obj[j] != '}' && obj[j] != ' ') j++;
    return obj.substring(i, j);
  }
}

bool parseDays(const String& arr, bool days[7]) {
  for (int i = 0; i < 7; i++) days[i] = false;
  String inner = arr;
  inner.replace("[", "");
  inner.replace("]", "");
  int idx = 0;
  int pos = 0;
  while (idx < 7) {
    int comma = inner.indexOf(',', pos);
    String tok = (comma < 0) ? inner.substring(pos) : inner.substring(pos, comma);
    tok.trim();
    days[idx] = (tok == "1");
    if (comma < 0) break;
    pos = comma + 1;
    idx++;
  }
  return true;
}

bool parseTimeField(const String& obj, const char* key, int& h, int& m) {
  String t = extractField(obj, key);
  if (t.length() < 5) return false;
  h = t.substring(0, 2).toInt();
  m = t.substring(3, 5).toInt();
  return true;
}

/* เช็คว่าตอนนี้ควรเปิดวาล์วไหม (ตามตารางที่เก็บไว้) */
bool scheduleActiveNow() {
  if (valveMode != "auto" || schedulesJson.length() == 0) return false;

  time_t now = time(nullptr);
  if (now < 1500000000) return false; // ยังไม่ได้ sync เวลา -> ไม่ประเมิน (กันปิดผิดพลาด)
  struct tm tinfo;
  localtime_r(&now, &tinfo);
  int nowMin = tinfo.tm_hour * 60 + tinfo.tm_min;
  int appDayIdx = (tinfo.tm_wday + 6) % 7; // จันทร์=0 ... อาทิตย์=6 (ตรงกับแอป)

  int pos = 0;
  while ((pos = schedulesJson.indexOf('{', pos)) >= 0) {
    int end = schedulesJson.indexOf('}', pos);
    if (end < 0) break;
    String obj = schedulesJson.substring(pos, end + 1);
    pos = end + 1;

    if (extractField(obj, "enabled") != "true") continue;

    bool days[7];
    String daysArr = extractField(obj, "days");
    if (daysArr.length() == 0) continue;
    parseDays(daysArr, days);
    if (!days[appDayIdx]) continue;

    int sh, sm, eh, em;
    if (!parseTimeField(obj, "start", sh, sm)) continue;
    if (!parseTimeField(obj, "end", eh, em)) continue;
    int startMin = sh * 60 + sm;
    int endMin = eh * 60 + em;

    if (startMin <= endMin) {
      if (nowMin >= startMin && nowMin < endMin) return true;
    } else {
      // ตารางข้ามเที่ยงคืน เช่น 22:00 - 06:00
      if (nowMin >= startMin || nowMin < endMin) return true;
    }
  }
  return false;
}

/* ตรวจตารางเวลา: ถ้าโหมด auto -> บังคับวาล์วให้ตรงตามตาราง */
void autoCheck() {
  if (valveMode != "auto") return;
  bool shouldOpen = scheduleActiveNow();
  setValve(shouldOpen);
}

/* ============ MQTT ============ */
void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  Serial.print("Received ["); Serial.print(topic); Serial.print("]: ");
  Serial.println(message);

  String t = String(topic);

  if (t == MODE_TOPIC) {
    // {"type":"mode","mode":"auto"} หรือ {"mode":"manual"}
    String m = extractField(message, "mode");
    if (m == "auto" || m == "manual") {
      valveMode = m;
      saveMode();
      autoCheck(); // ประเมินทันทีเมื่อสลับเป็น auto
    }
    return;
  }

  if (t == SCHEDULE_TOPIC) {
    // {"type":"schedule","schedules":[ ... ]}
    String arr = extractField(message, "schedules");
    if (arr.length() > 0) {
      schedulesJson = arr;
      saveSchedules();
      autoCheck();
    }
    return;
  }

  // CONTROL_TOPIC: คำสั่งเปิด/ปิดจากแอป
  if (message == "ON" || extractField(message, "cmd") == "open") {
    setValve(true);
  } else if (message == "OFF" || extractField(message, "cmd") == "close") {
    setValve(false);
  } else if (extractField(message, "cmd") == "toggle") {
    setValve(!valveOn);
  } else if (extractField(message, "cmd") == "getStatus") {
    publishStatus();
  } else if (extractField(message, "cmd") == "reboot") {
    delay(100);
    ESP.restart();
  }
}

/* ============ Setup ============ */
void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  prefs.begin("valve", false);
  valveMode = prefs.getString("mode", "manual");
  schedulesJson = prefs.getString("schedules", "");
  Serial.print("Restored mode="); Serial.println(valveMode);
  Serial.print("Restored schedules="); Serial.println(schedulesJson);

  Serial.print("Connecting to WiFi: "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());

  // ซิงก์เวลาจาก NTP (SNTP จะ sync ซ้ำอัตโนมัติทุก ~1 ชม.)
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, "pool.ntp.org", "time.nist.gov");

  espClient.setInsecure();
  mqtt.setServer(broker, mqtt_port);
  mqtt.setCallback(callback);
  mqtt.setKeepAlive(60); // ประหยัดข้อมูล: ping ทุก 60 วินาที
}

/* ============ Loop ============ */
void loop() {
  if (!mqtt.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      delay(500);
      return;
    }
    Serial.print("Connecting to HiveMQ Cloud...");
    if (mqtt.connect("ESP32_WaterValve_Client", mqtt_username, mqtt_password)) {
      Serial.println(" connected!");
      mqtt.subscribe(CONTROL_TOPIC, 1); // QoS 1 เชื่อถือได้กว่า
      mqtt.subscribe(MODE_TOPIC, 1);
      mqtt.subscribe(SCHEDULE_TOPIC, 1);
      publishStatus();       // แจ้งสถานะทันทีที่กลับมาเชื่อมต่อ
      autoCheck();           // ใช้ตารางที่เก็บไว้ทันทีเมื่อกลับมาเชื่อมต่อ
    } else {
      Serial.print(" failed, rc="); Serial.print(mqtt.state());
      Serial.println(" try again in 10 seconds");
      delay(10000); // ถอยหลังยาวขึ้น ประหยัด data ตอนเน็ตเสีย
    }
  }
  mqtt.loop();

  // ตรวจตารางเวลาเป็นช่วงๆ (ทำงานเอง แม้แอปปิด)
  if (millis() - lastScheduleCheck >= SCHEDULE_CHECK_MS) {
    lastScheduleCheck = millis();
    autoCheck();
  }
}
