#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --- Config ---
const char* ssid = "3bb-wifi-2.4G";
const char* password = "0816384110";
const char* mqtt_server = "crop-io.com"; 

const char* topic_telemetry = "farm1/zoneA/dev01/telemetry";
const char* topic_cmd = "farm1/zoneA/dev01/cmd";
const char* topic_ack = "farm1/zoneA/dev01/ack";

#define DHTPIN 15
#define DHTTYPE DHT22
#define RELAY_PIN 5 // GPIO pin for relay control เปลียนเป็นGPIO 19 ตามวงจร

DHT dht(DHTPIN, DHTTYPE);
WiFiClient espClient;
PubSubClient client(espClient);
LiquidCrystal_I2C lcd(0x27, 20, 4); 

unsigned long lastMsg = 0;
bool relayState = false;
float currentTemp = 0.0;
float currentHum = 0.0;

void updateDisplay() {
  // อัปเดต LCD
  lcd.setCursor(0, 0);
  lcd.print("IP: "); 
  lcd.print(WiFi.localIP());
  lcd.print("    "); 

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

  // อัปเดต Serial Monitor
  Serial.println("====================");
  Serial.print("IP: "); Serial.println(WiFi.localIP());
  Serial.print("Temp: "); Serial.print(currentTemp, 1); Serial.println(" C");
  Serial.print("Hum : "); Serial.print(currentHum, 1); Serial.println(" %");
  Serial.print("Relay: "); Serial.println(relayState ? "ON" : "OFF");
}

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

void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<200> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) return;

  if (String(topic) == topic_cmd) {
    const char* cmd = doc["cmd"];
    int state = doc["state"];

    if (String(cmd) == "relay") {
      relayState = (state == 1);
      digitalWrite(RELAY_PIN, relayState ? HIGH : LOW);

      StaticJsonDocument<200> ackDoc;
      ackDoc["cmd"] = "relay";
      ackDoc["status"] = "success";
      ackDoc["executed_state"] = relayState;
      char buffer[200];
      serializeJson(ackDoc, buffer);
      client.publish(topic_ack, buffer);
      
      // อัปเดตหน้าจอทันทีเมื่อสถานะ Relay เปลี่ยน
      updateDisplay();
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32_Dev01")) {
      Serial.println("connected");
      client.subscribe(topic_cmd);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  
  lcd.init();
  lcd.backlight();
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  dht.begin();
  
  setup_wifi();
  
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

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
      
      StaticJsonDocument<200> doc;
      doc["temp"] = currentTemp;
      doc["hum"] = currentHum;
      doc["relay_state"] = relayState;
      char buffer[200];
      serializeJson(doc, buffer);
      client.publish(topic_telemetry, buffer);
      
      // อัปเดตหน้าจอทุก 5 วินาทีตามรอบการอ่านเซ็นเซอร์
      updateDisplay();
    } else {
      Serial.println("Failed to read from DHT sensor!");
    }
  }
}