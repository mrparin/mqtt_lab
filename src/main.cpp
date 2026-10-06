#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================= ตั้งค่าฮาร์ดแวร์ =================
#define DHTPIN 15
#define DHTTYPE DHT22
#define RELAY_PIN 5
#define WIFI_CONFIG_PIN 27 // Normally-open switch: GPIO27 to GND

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);
LiquidCrystal_I2C lcd(0x27, 20, 4);

// ================= ตัวแปรสำหรับ Custom Parameters =================
char mqtt_server[40] = "192.168.1.5";
char farm_name[40] = "farm1";
char zone_name[40] = "zoneA";
char device_id[40] = "dev01";

// String สำหรับประกอบร่าง Topic อัตโนมัติ
String topic_telemetry;
String topic_cmd;
String topic_ack;
String topic_status;

unsigned long lastMsg = 0;
bool relayState = false;
float currentTemp = 0.0;
float currentHum = 0.0;

WiFiManager wm;
WiFiManagerParameter custom_mqtt_server("server", "MQTT Server IP", mqtt_server, sizeof(mqtt_server) - 1);
WiFiManagerParameter custom_farm("farm", "Farm Name", farm_name, sizeof(farm_name) - 1);
WiFiManagerParameter custom_zone("zone", "Zone Name", zone_name, sizeof(zone_name) - 1);
WiFiManagerParameter custom_device("dev", "Device ID", device_id, sizeof(device_id) - 1);
bool configSaved = false;
const unsigned long SWITCH_DEBOUNCE_MS = 30;

// Persist custom fields separately from WiFi credentials.
void loadConfig() {
  Preferences prefs;
  if (!prefs.begin("mqtt-lab", true)) return;
  prefs.getString("server", mqtt_server).toCharArray(mqtt_server, sizeof(mqtt_server));
  prefs.getString("farm", farm_name).toCharArray(farm_name, sizeof(farm_name));
  prefs.getString("zone", zone_name).toCharArray(zone_name, sizeof(zone_name));
  prefs.getString("device", device_id).toCharArray(device_id, sizeof(device_id));
  prefs.end();
}

bool validTopicPart(const char* value) {
  return value[0] != '\0' && strpbrk(value, "/+#") == nullptr;
}

void saveConfig() {
  Preferences prefs;
  if (!prefs.begin("mqtt-lab", false)) {
    Serial.println("Cannot save custom settings!");
    return;
  }
  prefs.putString("server", mqtt_server);
  prefs.putString("farm", farm_name);
  prefs.putString("zone", zone_name);
  prefs.putString("device", device_id);
  prefs.end();
}

void checkConfigSwitch() {
  // static ทำให้จำค่าเดิมได้ แม้ฟังก์ชันจะจบและถูกเรียกใหม่ในรอบถัดไป
  static bool lastReading = HIGH;
  static bool stableState = HIGH;
  static unsigned long changedAt = 0;
  const unsigned long now = millis();
  const bool reading = digitalRead(WIFI_CONFIG_PIN);

  // สวิตช์อาจเด้ง HIGH/LOW หลายครั้งเมื่อกด จึงเริ่มจับเวลาใหม่ทุกครั้งที่ค่าเปลี่ยน
  if (reading != lastReading) {
    lastReading = reading;
    changedAt = now;
  }

  // ยังไม่นิ่งครบ 30 ms ให้กลับไปทำงานอื่น ไม่ต้องหยุดรอด้วย delay()
  if (now - changedAt < SWITCH_DEBOUNCE_MS) return;
  if (reading == stableState) return;

  // ยอมรับสถานะใหม่เมื่อค่าคงที่ครบเวลา เปิด Portal เฉพาะตอนเปลี่ยนเป็น LOW
  stableState = reading;
  if (stableState != LOW || wm.getConfigPortalActive()) return;
  digitalWrite(RELAY_PIN, LOW);
  relayState = false;
  if (client.connected()) {
    client.publish(topic_status.c_str(), "{\"status\":\"offline\"}", true);
    client.disconnect();
  }
  Serial.println("Switch pressed. Opening WiFi configuration portal...");
  wm.startConfigPortal("ESP32_SmartFarm");
}

// ================= ฟังก์ชันอัปเดตหน้าจอ =================
void updateDisplay() {
  if (wm.getConfigPortalActive()) return;
  lcd.setCursor(0, 0);
  lcd.print("IP: "); 
  lcd.print(WiFi.localIP());
  lcd.print("      ");

  lcd.setCursor(0, 1);
  lcd.print("Temp: "); 
  lcd.print(currentTemp, 1);
  lcd.print(" C    ");

  lcd.setCursor(0, 2);
  lcd.print("Hum : "); 
  lcd.print(currentHum, 1);
  lcd.print(" %    ");

  lcd.setCursor(0, 3);
  lcd.print("Relay: "); 
  lcd.print(relayState ? "ON " : "OFF");
  lcd.print("        ");

  Serial.println("====================");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("Temp: "); Serial.print(currentTemp, 1); Serial.println(" C");
  Serial.print("Hum : "); Serial.print(currentHum, 1); Serial.println(" %");
  Serial.print("Relay: "); Serial.println(relayState ? "ON" : "OFF");
}

// ================= ฟังก์ชันประมวลผลคำสั่ง (Callback) =================
void callback(char* topic, byte* payload, unsigned int length) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  
  if (error) {
    Serial.print("JSON Parse Error: ");
    Serial.println(error.c_str());
    return;
  }

  if (String(topic) == topic_cmd) {
    if (String(doc["cmd"] | "") == "relay") {
      const int state = doc["state"].as<int>();
      if (!doc["state"].is<int>() || (state != 0 && state != 1)) {
        Serial.println("Invalid relay state: expected integer 0 or 1");
        return;
      }
      relayState = (state == 1);
      digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

      JsonDocument ackDoc;
      ackDoc["cmd"] = "relay";
      ackDoc["status"] = "success";
      ackDoc["executed_state"] = relayState;
      char buffer[256];
      serializeJson(ackDoc, buffer);
      client.publish(topic_ack.c_str(), buffer);
      
      updateDisplay();
    }
  }
}

// ================= ฟังก์ชันรักษาสถานะ MQTT & LWT =================
void reconnect() {
  static unsigned long lastAttempt = 0;
  static bool attempted = false;
  if (wm.getConfigPortalActive()) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (attempted && millis() - lastAttempt < 5000) return;
  attempted = true;
  {
    Serial.print("Attempting MQTT connection...");
    
    // ตั้งค่า LWT (ใช้ .c_str() เพื่อแปลง String เป็น char array)
    if (client.connect(device_id, topic_status.c_str(), 0, true, "{\"status\":\"offline\"}")) {
      Serial.println("connected");
      
      client.publish(topic_status.c_str(), "{\"status\":\"online\"}", true);
      client.subscribe(topic_cmd.c_str());
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");

    }
    lastAttempt = millis();
  }
}

// ================= ฟังก์ชันตั้งค่า =================
void applyConfig();

void setup() {
  Serial.begin(115200);
  pinMode(WIFI_CONFIG_PIN, INPUT_PULLUP);
  Serial.println("Firmware: external switch WiFi portal (GPIO27 to GND)");

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi...");
  lcd.setCursor(0, 1);
  lcd.print("ESP32_SmartFarm");
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  dht.begin();

  loadConfig();
  custom_mqtt_server.setValue(mqtt_server, sizeof(mqtt_server) - 1);
  custom_farm.setValue(farm_name, sizeof(farm_name) - 1);
  custom_zone.setValue(zone_name, sizeof(zone_name) - 1);
  custom_device.setValue(device_id, sizeof(device_id) - 1);
  wm.setConnectTimeout(20);
  wm.setConfigPortalTimeout(180);
  wm.setConfigPortalBlocking(false);
  wm.setAPCallback([](WiFiManager*) {
    lcd.clear();
    lcd.print("ESP32_SmartFarm");
    lcd.setCursor(0, 1);
    lcd.print("Open 192.168.4.1");
  });

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_farm);
  wm.addParameter(&custom_zone);
  wm.addParameter(&custom_device);

  wm.setSaveParamsCallback([]() { configSaved = true; });
  wm.autoConnect("ESP32_SmartFarm");
  applyConfig();
  client.setCallback(callback);
  client.setSocketTimeout(3);
  WiFi.setAutoReconnect(true);
  updateDisplay();
}

void applyConfig() {
  // อ่านค่าที่ผู้ใช้กรอกมาเก็บลงตัวแปร
  const char* server = custom_mqtt_server.getValue();
  if (server[0] != '\0') snprintf(mqtt_server, sizeof(mqtt_server), "%s", server);
  if (validTopicPart(custom_farm.getValue()))
    snprintf(farm_name, sizeof(farm_name), "%s", custom_farm.getValue());
  if (validTopicPart(custom_zone.getValue()))
    snprintf(zone_name, sizeof(zone_name), "%s", custom_zone.getValue());
  if (validTopicPart(custom_device.getValue()))
    snprintf(device_id, sizeof(device_id), "%s", custom_device.getValue());
  saveConfig();

  // สร้าง Topic อัตโนมัติจาก Parameter ที่ได้รับ
  topic_telemetry = String(farm_name) + "/" + String(zone_name) + "/" + String(device_id) + "/telemetry";
  topic_cmd = String(farm_name) + "/" + String(zone_name) + "/" + String(device_id) + "/cmd";
  topic_ack = String(farm_name) + "/" + String(zone_name) + "/" + String(device_id) + "/ack";
  topic_status = String(farm_name) + "/" + String(zone_name) + "/" + String(device_id) + "/status";

  // พิมพ์ตรวจสอบ Topic ทาง Serial Monitor
  Serial.println("--- Current Topics ---");
  Serial.println(topic_telemetry);
  Serial.println(topic_cmd);
  Serial.println(topic_ack);
  Serial.println(topic_status);

  client.setServer(mqtt_server, 1883);

}

// ================= ลูปการทำงานหลัก =================
void loop() {
  checkConfigSwitch();
  const bool portalWasActive = wm.getConfigPortalActive();
  wm.process();
  if (configSaved) {
    configSaved = false;
    if (client.connected()) {
      client.publish(topic_status.c_str(), "{\"status\":\"offline\"}", true);
      client.disconnect();
    }
    applyConfig();
  }
  if (portalWasActive && !wm.getConfigPortalActive()) {
    lcd.clear();
    updateDisplay();
  }
  if (wm.getConfigPortalActive()) {
    delay(10);
    return;
  }
  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  unsigned long now = millis();
  if (now - lastMsg > 5000) {
    lastMsg = now;
    float t = dht.readTemperature();
    float h = dht.readHumidity();

    if (!isnan(t) && !isnan(h)) {
      currentTemp = t;
      currentHum = h;
      
      JsonDocument doc;
      doc["temp"] = currentTemp;
      doc["hum"] = currentHum;
      doc["relay_state"] = relayState;
      char buffer[256];
      serializeJson(doc, buffer);
      
      client.publish(topic_telemetry.c_str(), buffer);
      updateDisplay();
    } else {
      Serial.println("Failed to read from DHT sensor!");
    }
  }
}
