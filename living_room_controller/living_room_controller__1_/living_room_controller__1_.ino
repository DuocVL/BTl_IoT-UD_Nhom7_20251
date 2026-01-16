#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h> 
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <time.h> // Thư viện lấy thời gian thực
 
// --- CẤU HÌNH PHẦN CỨNG ---
#define PIN_NEOPIXEL 23
#define NUM_PIXELS   16
 
// Fan 1 (L9110)
#define PIN_FAN1_A   18 
#define PIN_FAN1_B   19
 
// Fan 2 (L9110) - Giữ nguyên chân cũ của bạn
#define PIN_FAN2_A   21 
#define PIN_FAN2_B   22
 
// SENSOR PINS (Thêm mới)
#define PIN_SHT_SDA  32 // Custom I2C SDA
#define PIN_SHT_SCL  33 // Custom I2C SCL
#define PIN_MQ2_AO   34 // Analog Output của MQ2
#define PIN_MQ2_DO   35 // Digital Output của MQ2 (Cảnh báo ngưỡng)
 
// PWM Settings
#define PWM_FREQ     5000
#define PWM_RES      8
#define PWM_LV0      0   
#define PWM_LV1      160 
#define PWM_LV2      255
 
// Time Server (NTP) để lấy timestamp
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // GMT+7 Vietnam
const int   daylightOffset_sec = 0;
 
// --- ĐỊNH DANH THIẾT BỊ (UUID) ---
const char* ID_LED_STRIP_1 = "47e0143b-98d5-45a5-8b46-c03e9480f3e9"; 
const char* ID_LED_STRIP_2 = "d81cef67-8490-4fd8-87d3-e864c998183c"; 
const char* ID_FAN_1       = "b792016c-8a51-4dc1-bdc4-cfb9c7679296";
const char* ID_FAN_2       = "e91c75b5-5a0c-4ec1-81a7-fb00660939cf";
 
// --- MQTT CONFIG ---
const char* mqtt_server = "68.183.188.187";
const int   mqtt_port   = 1885;
const char* mqtt_user   = "mqtt_admin";
const char* mqtt_pass   = "12345678@abc";
const char* topic_telemetry = "smart_home/controller-01/telemetry";
 
// --- OBJECTS ---
WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel pixels(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
Adafruit_SHT31 sht31 = Adafruit_SHT31();
 
// --- VARIABLES ---
unsigned long lastTelemetryTime = 0;
const long telemetryInterval = 5000; // Gửi data mỗi 5 giây
 
// --- QUẢN LÝ TRẠNG THÁI ---
struct DeviceState {
  String id;
  String type; 
  bool state;    
  int value;     
  String color;
  unsigned long timer_end_at; 
};
 
DeviceState dev_led1 = {ID_LED_STRIP_1, "light", false, 100, "#FFFFFF", 0};
DeviceState dev_led2 = {ID_LED_STRIP_2, "light", false, 100, "#FFFFFF", 0};
DeviceState dev_fan1 = {ID_FAN_1,       "fan",   false, 0,   "",        0};
DeviceState dev_fan2 = {ID_FAN_2,       "fan",   false, 0,   "",        0};
 
// --- HÀM HỖ TRỢ ---
 
// Lấy thời gian hiện tại dạng Unix Timestamp (ms)
unsigned long long getEpochTimeMs() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return 0;
  }
  time_t now;
  time(&now);
  return (unsigned long long)now * 1000; // Chuyển giây sang mili giây
}
 
uint32_t hexToColor(String hex) {
  if (hex.startsWith("#")) hex.remove(0, 1);
  long number = strtol(hex.c_str(), NULL, 16);
  return pixels.Color((number >> 16) & 0xFF, (number >> 8) & 0xFF, number & 0xFF);
}
 
void controlFanHW(int pinPWM, int pinLow, int level) {
  int pwmVal = 0;
  switch (level) {
    case 1:  pwmVal = PWM_LV1; break;
    case 2:  pwmVal = PWM_LV2; break;
    default: pwmVal = PWM_LV0; break;
  }
  digitalWrite(pinLow, LOW); 
  ledcWrite(pinPWM, pwmVal); 
}
 
void updateLeds() {
  uint32_t c1 = dev_led1.state ? hexToColor(dev_led1.color) : 0;
  for(int i=0; i<8; i++) {
    if(dev_led1.state) {
      uint8_t r = (uint8_t)(c1 >> 16) * dev_led1.value / 100;
      uint8_t g = (uint8_t)(c1 >> 8)  * dev_led1.value / 100;
      uint8_t b = (uint8_t)c1         * dev_led1.value / 100;
      pixels.setPixelColor(i, pixels.Color(r,g,b));
    } else {
      pixels.setPixelColor(i, 0);
    }
  }
 
  uint32_t c2 = dev_led2.state ? hexToColor(dev_led2.color) : 0;
  for(int i=8; i<16; i++) {
     if(dev_led2.state) {
      uint8_t r = (uint8_t)(c2 >> 16) * dev_led2.value / 100;
      uint8_t g = (uint8_t)(c2 >> 8)  * dev_led2.value / 100;
      uint8_t b = (uint8_t)c2         * dev_led2.value / 100;
      pixels.setPixelColor(i, pixels.Color(r,g,b));
    } else {
      pixels.setPixelColor(i, 0);
    }
  }
  pixels.show();
}
 
void updateFans() {
  int speed1 = dev_fan1.state ? dev_fan1.value : 0;
  int speed2 = dev_fan2.state ? dev_fan2.value : 0;
  controlFanHW(PIN_FAN1_A, PIN_FAN1_B, speed1);
  controlFanHW(PIN_FAN2_A, PIN_FAN2_B, speed2);
}
 
void publishStatus(DeviceState &dev) {
  StaticJsonDocument<256> doc;
  doc["device_id"] = dev.id;
  doc["type"] = dev.type;
  JsonObject params = doc.createNestedObject("params");
  if (dev.type == "fan") {
      params["state"] = (dev.state && dev.value > 0) ? "ON" : "OFF";
      params["speed"] = dev.value;
  } else {
      params["state"] = dev.state ? "ON" : "OFF";
      params["brightness"] = dev.value;
      params["color"] = dev.color;
  }
 
  if (dev.timer_end_at > 0 && dev.state) {
    long remaining = (dev.timer_end_at - millis()) / 60000;
    params["timer_left"] = (remaining < 0) ? 0 : remaining;
  }
 
  char payloadBuffer[256];
  serializeJson(doc, payloadBuffer);
  char topicBuffer[128]; 
  snprintf(topicBuffer, sizeof(topicBuffer), "smart_home/controller-01/%s/status", dev.id.c_str());
  client.publish(topicBuffer, payloadBuffer, true); 
}
 
// === HÀM ĐỌC SENSOR VÀ GỬI TELEMETRY ===
void handleTelemetry() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = currentMillis;
 
    // 1. Đọc SHT31
    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
 
    if (isnan(t)) t = 0.0;
    if (isnan(h)) h = 0.0;
 
    // 2. Đọc MQ2 (Analog)
    // Giá trị ADC ESP32 từ 0-4095
    int gasValue = analogRead(PIN_MQ2_AO);
 
    // 3. Lấy Timestamp (Unix ms)
    unsigned long long timestamp = getEpochTimeMs();
 
    // 4. Đóng gói JSON
    StaticJsonDocument<256> doc;
    // Format float 2 số thập phân bằng cách ép kiểu hoặc làm tròn trước
    doc["temperature"] = ((int)(t * 100)) / 100.0; 
    doc["humidity"] = ((int)(h * 100)) / 100.0;
    doc["gas"] = gasValue;
    doc["timestamp"] = timestamp;
 
    char buffer[256];
    serializeJson(doc, buffer);
 
    // 5. Gửi lên MQTT
    if (client.connected()) {
      client.publish(topic_telemetry, buffer);
      Serial.print("Telemetry sent: ");
      Serial.println(buffer);
    }
  }
}
 
void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) return;
 
  const char* r_id = doc["device_id"];
  String receivedId = String(r_id);
  JsonObject params = doc["params"];
  DeviceState* target = nullptr;
 
  if (receivedId == ID_LED_STRIP_1) target = &dev_led1;
  else if (receivedId == ID_LED_STRIP_2) target = &dev_led2;
  else if (receivedId == ID_FAN_1) target = &dev_fan1;
  else if (receivedId == ID_FAN_2) target = &dev_fan2;
 
  if (target) {
    if (params.containsKey("state")) {
      const char* st = params["state"];
      target->state = (strcmp(st, "ON") == 0);
      if (target->type == "fan" && !target->state) target->value = 0;
      if (target->type == "fan" && target->state && target->value == 0) target->value = 1;
    }
    if (target->type == "light") {
      if (params.containsKey("brightness")) target->value = params["brightness"];
      if (params.containsKey("color")) target->color = params["color"].as<String>();
    } else {
      if (params.containsKey("speed")) {
          int s = params["speed"];
          if (s >= 0 && s <= 2) {
              target->value = s;
              target->state = (s > 0);
          }
      }
    }
    if (params.containsKey("timer_off")) {
      int minutes = params["timer_off"];
      target->timer_end_at = (minutes > 0) ? (millis() + minutes * 60000) : 0;
    }
    updateLeds();
    updateFans();
    publishStatus(*target);
  }
}
 
void setup() {
  Serial.begin(115200);
 
  pixels.begin();
  pixels.show();
 
  // Config PWM Fan
  ledcAttach(PIN_FAN1_A, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_FAN2_A, PWM_FREQ, PWM_RES);
  pinMode(PIN_FAN1_B, OUTPUT);
  pinMode(PIN_FAN2_B, OUTPUT);
  digitalWrite(PIN_FAN1_B, LOW); ledcWrite(PIN_FAN1_A, 0);
  digitalWrite(PIN_FAN2_B, LOW); ledcWrite(PIN_FAN2_A, 0);
 
  // === INIT SENSORS ===
  // Cấu hình I2C trên chân custom
  Wire.begin(PIN_SHT_SDA, PIN_SHT_SCL); 
  if (!sht31.begin(0x44)) {   // 0x44 là địa chỉ I2C mặc định của SHT31
    Serial.println("Couldn't find SHT31");
  } else {
    Serial.println("SHT31 Found!");
  }
 
  // Cấu hình MQ2
  pinMode(PIN_MQ2_AO, INPUT);
  pinMode(PIN_MQ2_DO, INPUT);
 
  // WiFi Manager
  WiFiManager wm;
  if(!wm.autoConnect("SmartHome_Config", "12345678")) {
    ESP.restart();
  } 
  // === CONFIG TIME (NTP) ===
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Waiting for NTP time sync...");
  // Chờ 1 chút để sync time (optional)
 
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}
 
void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32_Controller_01";
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      client.subscribe("smart_home/controller-01/cmd");
      publishStatus(dev_led1);
      publishStatus(dev_led2);
      publishStatus(dev_fan1);
      publishStatus(dev_fan2);
    } else {
      delay(5000);
    }
  }
}
 
void checkTimer(DeviceState &dev) {
  if (dev.state && dev.timer_end_at > 0) {
    if (millis() > dev.timer_end_at) {
      dev.state = false;
      dev.value = 0; 
      dev.timer_end_at = 0;
      if (dev.type == "light") updateLeds();
      else updateFans();
      publishStatus(dev);
    }
  }
}
 
void loop() {
  if (!client.connected()) reconnect();
  client.loop();
 
  checkTimer(dev_led1);
  checkTimer(dev_led2);
  checkTimer(dev_fan1);
  checkTimer(dev_fan2);
 
  // Gọi hàm đọc và gửi sensor
  handleTelemetry();
}