#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ================= กำหนดค่าเริ่มต้น =================
const char* ssid = "3bb-wifi-2.4G";
const char* password = "0816384110";
const char* mqtt_server = "192.168.1.5"; 
const char* device_id = "ESP32_Dev01";

// กำหนดโครงสร้าง Topic
const char* topic_telemetry = "farm1/zoneA/dev01/telemetry";
const char* topic_cmd = "farm1/zoneA/dev01/cmd";
const char* topic_ack = "farm1/zoneA/dev01/ack";
const char* topic_status = "farm1/zoneA/dev01/status";

// ตั้งค่าฮาร์ดแวร์
#define DHTPIN 15
#define DHTTYPE DHT22
#define RELAY_PIN 5 // กำหนดขา Relay

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);
LiquidCrystal_I2C lcd(0x27, 20, 4); 

// ตัวแปรสถานะ
unsigned long lastMsg = 0;
bool relayState = false;
float currentTemp = 0.0;
float currentHum = 0.0;

// ================= ฟังก์ชันอัปเดตหน้าจอ =================
void updateDisplay() {
  lcd.setCursor(0, 0);
  lcd.print("IP: "); 
  lcd.print(WiFi.localIP());
  lcd.print("      "); // เคลียร์ตัวอักษรตกค้าง

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

// ================= ฟังก์ชันเชื่อมต่อ WiFi =================
void setup_wifi() {
  delay(10);
  Serial.print("Connecting to ");
  Serial.println(ssid);
  
  lcd.setCursor(0, 0);
  lcd.print("Connecting WiFi...");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected");
  lcd.clear();
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
    const char* cmd = doc["cmd"];
    int state = doc["state"];

    if (String(cmd) == "relay") {
      relayState = (state == 1);
      digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

      // ส่ง Acknowledge กลับไปยังระบบหลังบ้าน
      StaticJsonDocument<256> ackDoc;
      ackDoc["cmd"] = "relay";
      ackDoc["status"] = "success";
      ackDoc["executed_state"] = relayState;
      char buffer[256];
      serializeJson(ackDoc, buffer);
      client.publish(topic_ack, buffer);
      
      updateDisplay(); // อัปเดตจอทันที
    }
  }
}

// ================= ฟังก์ชันรักษาสถานะ MQTT & LWT =================
void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    
    // ตั้งค่า LWT ให้ส่ง {"status":"offline"} แบบ Retain ทันทีที่หลุด
    if (client.connect(device_id, topic_status, 0, true, "{\"status\":\"offline\"}")) {
      Serial.println("connected");
      
      // ส่งสถานะ Online แบบ Retain เมื่อต่อสำเร็จ
      client.publish(topic_status, "{\"status\":\"online\"}", true);
      client.subscribe(topic_cmd);
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
  
  lcd.init();
  lcd.backlight();
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW); // ปิด Relay เริ่มต้น
  dht.begin();
  
  setup_wifi();
  
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

// ================= ลูปการทำงานหลัก =================
void loop() {
  if (!client.connected()) {
    reconnect();
  }
  client.loop(); // รักษาการเชื่อมต่อและดักจับข้อความขาเข้า

  unsigned long now = millis();
  // อ่านและส่งค่าเซ็นเซอร์ทุก 5 วินาที แบบ Non-blocking
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
      
      client.publish(topic_telemetry, buffer);
      updateDisplay();
    } else {
      Serial.println("Failed to read from DHT sensor!");
    }
  }
}