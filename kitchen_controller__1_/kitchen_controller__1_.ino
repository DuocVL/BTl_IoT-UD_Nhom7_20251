#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <time.h>

#define PIN_NEOPIXEL 23
#define NUM_PIXELS   16

#define PIN_FAN1_A   18
#define PIN_FAN1_B   19
#define PIN_FAN2_A   21
#define PIN_FAN2_B   22
#define PIN_SHT_SDA  32
#define PIN_SHT_SCL  33
#define PIN_MQ2_AO   34
#define PIN_MQ2_DO   35
#define PIN_FIRE_SENSOR 27

#define PWM_FREQ     5000
#define PWM_RES      8
#define PWM_LV0      0  

#define PWM_LV1      160
#define PWM_LV2      210
#define PWM_LV3      255

#define GAS_THRESHOLD_PPM 300
#define GAS_HYSTERESIS    50
const unsigned long GAS_ALERT_COOLDOWN = 180000; // 3 phút (3 * 60 * 1000 ms)

const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;

const char* ID_LED_STRIP_1 = "47e0143b-98d5-45a5-8b46-c03e9480f3e9";
const char* ID_LED_STRIP_2 = "d81cef67-8490-4fd8-87d3-e864c998183c";
const char* ID_FAN_1       = "b792016c-8a51-4dc1-bdc4-cfb9c7679296";
const char* ID_FAN_2       = "e91c75b5-5a0c-4ec1-81a7-fb00660939cf";

const char* mqtt_server = "68.183.188.187";
const int   mqtt_port   = 1885;
const char* mqtt_user   = "mqtt_admin";
const char* mqtt_pass   = "12345678@abc";
const char* topic_telemetry = "smart_home/controller-01/telemetry";
const char* topic_warning   = "smart_home/eeb1dee9-db9f-48bc-89e4-fc1fd9586ede/warning";

WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel pixels(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
Adafruit_SHT31 sht31 = Adafruit_SHT31();

unsigned long lastTelemetryTime = 0;
const long telemetryInterval = 5000;

// --- Biến trạng thái cảnh báo ---
bool isFireAlertSent = false;
bool isGasAlertSent = false;
unsigned long lastGasAlertTime = 0; // Lưu thời gian lần gửi cảnh báo Gas cuối cùng

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

unsigned long long getEpochTimeMs() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return 0;
  }
  time_t now;
  time(&now);
  // Trả về UTC+7 (Cộng 7 giờ)
  return ((unsigned long long)now + (7 * 3600)) * 1000;
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
    case 3:  pwmVal = PWM_LV3; break;
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

void sendWarning(String alertType, String severity, String msg) {
  if (!client.connected()) return;

  StaticJsonDocument<256> doc;
  doc["alertType"] = alertType;
  doc["severity"] = severity;
  doc["message"] = msg;
  // Thêm timestamp vào warning nếu cần
  doc["timestamp"] = getEpochTimeMs();

  char buffer[256];
  serializeJson(doc, buffer);
  client.publish(topic_warning, buffer);
  Serial.print("WARNING SENT: ");
  Serial.println(buffer);
}

void handleSafetySensors() {
  unsigned long currentMillis = millis();

  // 1. Fire Sensor
  bool isFire = (digitalRead(PIN_FIRE_SENSOR) == LOW);
  if (isFire) {
    if (!isFireAlertSent) {
      sendWarning("FIRE", "CRITICAL", "Báo động có đám cháy");
      isFireAlertSent = true;
    }
  } else {
    //delay(5000);
    isFireAlertSent = false;
  }

  // 2. Gas Sensor
  int gasValue = analogRead(PIN_MQ2_AO);
 
  if(gasValue >= 1000){
    if(!isGasAlertSent){
      sendWarning("GAS", "WARNING", "Cảnh báo rò rỉ khí ga, hãy kiểm tra bình ga ngay");
      isGasAlertSent = true;
    }
  }else{
    //delay(5000);
    isGasAlertSent = false;
  }
 
  // if (gasValue >= GAS_THRESHOLD_PPM) {
  //   // --- Xử lý Quạt (Ưu tiên an toàn, luôn kiểm tra) ---
  //   // Nếu quạt chưa bật hoặc chưa ở mức 1, cưỡng chế bật ngay
  //   bool needUpdate = false;
  //   if (!dev_fan1.state || dev_fan1.value != 1) {
  //     dev_fan1.state = true; dev_fan1.value = 1; needUpdate = true;
  //   }
  //   if (!dev_fan2.state || dev_fan2.value != 1) {
  //     dev_fan2.state = true; dev_fan2.value = 1; needUpdate = true;
  //   }
  //   if (needUpdate) {
  //     updateFans();
  //     publishStatus(dev_fan1);
  //     publishStatus(dev_fan2);
  //   }

  //   // --- Xử lý Gửi Cảnh báo (Có giới hạn 3 phút) ---
  //   // Gửi nếu: Chưa gửi lần nào (isGasAlertSent == false) HOẶC Đã quá 3 phút từ lần gửi cuối
  //   if (!isGasAlertSent || (currentMillis - lastGasAlertTime >= GAS_ALERT_COOLDOWN)) {
  //     sendWarning("GAS", "WARNING", "Cảnh báo rò rỉ khí ga, hãy kiểm tra bình ga ngay");
  //     isGasAlertSent = true;
  //     lastGasAlertTime = currentMillis; // Lưu mốc thời gian
  //   }
  // }
  // else if (gasValue < (GAS_THRESHOLD_PPM - GAS_HYSTERESIS)) {
  //   // Chỉ xử lý khi trạng thái chuyển từ Nguy hiểm -> An toàn
  //   if (isGasAlertSent) {
  //     isGasAlertSent = false;
  //     // Reset timer để lần sau bị lại sẽ báo ngay lập tức
  //     lastGasAlertTime = 0;

  //     // Tắt quạt
  //     Serial.println("GAS CLEARED: Turning OFF Fans");
  //     dev_fan1.state = false; dev_fan1.value = 0;
  //     dev_fan2.state = false; dev_fan2.value = 0;
  //     updateFans();
  //     publishStatus(dev_fan1);
  //     publishStatus(dev_fan2);
  //   }
  // }
}

void handleTelemetry() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = currentMillis;

    float t = sht31.readTemperature();
    float h = sht31.readHumidity();

    if (isnan(t)) t = 0.0;
    if (isnan(h)) h = 0.0;

    int gasValue = analogRead(PIN_MQ2_AO);
    unsigned long long timestamp = getEpochTimeMs();

    StaticJsonDocument<256> doc;
    doc["temperature"] = ((int)(t * 100)) / 100.0;
    doc["humidity"] = ((int)(h * 100)) / 100.0;
    doc["gas"] = gasValue;
    doc["timestamp"] = timestamp;

    char buffer[256];
    serializeJson(doc, buffer);
   
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
          if (s >= 0 && s <= 3) {
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

  ledcAttach(PIN_FAN1_A, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_FAN2_A, PWM_FREQ, PWM_RES);
  pinMode(PIN_FAN1_B, OUTPUT);
  pinMode(PIN_FAN2_B, OUTPUT);
  digitalWrite(PIN_FAN1_B, LOW);
  ledcWrite(PIN_FAN1_A, 0);
  digitalWrite(PIN_FAN2_B, LOW); ledcWrite(PIN_FAN2_A, 0);
 
  Wire.begin(PIN_SHT_SDA, PIN_SHT_SCL);
 
  if (!sht31.begin(0x44)) {  
    Serial.println("Couldn't find SHT31");
  } else {
    Serial.println("SHT31 Found!");
  }

  pinMode(PIN_MQ2_AO, INPUT);
  pinMode(PIN_MQ2_DO, INPUT);
  pinMode(PIN_FIRE_SENSOR, INPUT_PULLUP);

  WiFiManager wm;
  if(!wm.autoConnect("SmartHome_Config", "12345678")) {
    ESP.restart();
  }
 
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Waiting for NTP time sync...");
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

  handleTelemetry();
  handleSafetySensors();
}
