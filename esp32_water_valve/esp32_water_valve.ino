#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

const int RELAY_PIN = 23; // ขาควบคุมรีเลย์วาล์วน้ำ

const char* ssid = "noonit_2.4G";
const char* password = "0821404025";

const char* broker = "74bd39d864644eebb6efba20dadd4e93.s1.eu.hivemq.cloud";
const int mqtt_port = 8883;
const char* mqtt_username = "test";
const char* mqtt_password = "test1234";

const char* STATUS_TOPIC = "myhouse/valve/status";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

bool valveOn = false;

// ส่งสถานะเป็น JSON ให้แอปอ่านได้ (แอปรอรับ {"valve":true/false,...})
// สำคัญ: อย่าใส่ "mode" ที่นี่เด็ดขาด — แอปเป็นคนจัดการโหมด (auto/manual)
// ถ้า ESP32 ส่ง mode มา แอปจะถูกเขียนทับกลับเป็น manual ทำให้โหมดอัตโนมัติพัง
void publishStatus() {
  String json = "{\"valve\":" + String(valveOn ? "true" : "false") + ",\"online\":true}";
  mqtt.publish(STATUS_TOPIC, json.c_str());
  Serial.println("Status sent: " + json);
}

void setValve(bool on) {
  valveOn = on;
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  publishStatus();
  Serial.println(on ? "-> Water Valve: OPEN (เปิดน้ำ)" : "-> Water Valve: CLOSED (ปิดน้ำ)");
}

void callback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (int i = 0; i < length; i++) message += (char)payload[i];
  message.trim();
  Serial.print("Received: "); Serial.println(message);

  // รองรับทั้ง JSON ที่แอปส่ง ({"cmd":"open"}) และข้อความตรง (ON/OFF)
  if (message == "ON" || message.indexOf("\"open\"") >= 0) {
    setValve(true);
  } else if (message == "OFF" || message.indexOf("\"close\"") >= 0) {
    setValve(false);
  } else if (message.indexOf("\"toggle\"") >= 0) {
    setValve(!valveOn);
  } else if (message.indexOf("getStatus") >= 0) {
    publishStatus();
  } else if (message.indexOf("\"reboot\"") >= 0) {
    ESP.restart();
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);

  Serial.print("Connecting to WiFi: "); Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.println("\nWiFi connected! IP: " + WiFi.localIP().toString());

  espClient.setInsecure();
  mqtt.setServer(broker, mqtt_port);
  mqtt.setCallback(callback);
  mqtt.setKeepAlive(60); // ประหยัดข้อมูล: ping ทุก 60 วินาที
}

void loop() {
  if (!mqtt.connected()) {
    // ถ้า WiFi หลุด -> reconnect WiFi ก่อนลอง MQTT
    if (WiFi.status() != WL_CONNECTED) {
      WiFi.reconnect();
      delay(500);
      return;
    }
    Serial.print("Connecting to HiveMQ Cloud...");
    if (mqtt.connect("ESP32_WaterValve_Client", mqtt_username, mqtt_password)) {
      Serial.println(" connected!");
      mqtt.subscribe("myhouse/valve/control", 1); // QoS 1 เชื่อถือได้กว่า
      publishStatus(); // แจ้งสถานะทันทีที่กลับมาเชื่อมต่อ
    } else {
      Serial.print(" failed, rc="); Serial.print(mqtt.state());
      Serial.println(" try again in 10 seconds");
      delay(10000); // ถอยหลังยาวขึ้น ประหยัด data ตอนเน็ตเสีย
    }
  }
  mqtt.loop();
}
