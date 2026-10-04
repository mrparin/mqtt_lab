#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================= ตั้งค่าฮาร์ดแวร์ =================
#define DHTPIN 15
#define DHTTYPE DHT22
#define RELAY_PIN 5
#define RESET_PIN 0 // ใช้ปุ่ม BOOT บนบอร์ด ESP32

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

// ================= ฟังก์ชันอัปเดตหน้าจอ =================
void updateDisplay() {
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
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  
  if (error) {
    Serial.print("JSON Parse Error: ");
    Serial.println(error.c_str());
    return;
  }

  if (String(topic) == topic_cmd) {
    if (String((const char*)doc["cmd"]) == "relay") {
      relayState = (doc["state"] == 1);
      digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

      StaticJsonDocument<256> ackDoc;
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
  while (!client.connected()) {
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
      delay(5000);
    }
  }
}

// ================= ฟังก์ชันตั้งค่า =================
void setup() {
  Serial.begin(115200);
  pinMode(RESET_PIN, INPUT_PULLUP);
  
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Starting AP...");
  lcd.setCursor(0, 1);
  lcd.print("ESP32_SmartFarm");
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  dht.begin();

  WiFiManager wm;
  // ตรวจสอบว่ามีการกดปุ่ม BOOT ค้างไว้ตอนเริ่มบูทบอร์ดหรือไม่
  if (digitalRead(RESET_PIN) == LOW) {
    Serial.println("Reset Button Pressed. Clearing WiFi Settings...");
    wm.resetSettings(); // ล้างค่า WiFi และ Custom Parameter เดิมทิ้ง
    Serial.println("Settings Cleared. Restarting...");
    delay(1000);
    ESP.restart(); // รีสตาร์ทบอร์ดเพื่อให้เข้าสู่โหมด AP ใหม่
  }
  
  // สร้าง Input Field ในหน้า Portal
  WiFiManagerParameter custom_mqtt_server("server", "MQTT Server IP", mqtt_server, 40);
  WiFiManagerParameter custom_farm("farm", "Farm Name", farm_name, 40);
  WiFiManagerParameter custom_zone("zone", "Zone Name", zone_name, 40);
  WiFiManagerParameter custom_device("dev", "Device ID", device_id, 40);

  wm.addParameter(&custom_mqtt_server);
  wm.addParameter(&custom_farm);
  wm.addParameter(&custom_zone);
  wm.addParameter(&custom_device);

  // เริ่มกระบวนการเชื่อมต่อ หรือเปิด AP 192.168.4.1 หากไม่สำเร็จ
  if(!wm.autoConnect("ESP32_SmartFarm")) {
    Serial.println("Failed to connect and hit timeout");
    delay(3000);
    ESP.restart();
  }

  lcd.clear();
  Serial.println("WiFi connected");

  // อ่านค่าที่ผู้ใช้กรอกมาเก็บลงตัวแปร
  strcpy(mqtt_server, custom_mqtt_server.getValue());
  strcpy(farm_name, custom_farm.getValue());
  strcpy(zone_name, custom_zone.getValue());
  strcpy(device_id, custom_device.getValue());

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
  client.setCallback(callback);
}

// ================= ลูปการทำงานหลัก =================
void loop() {
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
      
      StaticJsonDocument<256> doc;
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