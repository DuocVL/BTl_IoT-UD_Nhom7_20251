#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <time.h>

// --- ĐỊNH NGHĨA CHÂN CẮM (PIN CONFIGURATION) ---
#define PIN_NEOPIXEL 23   // Chân dữ liệu dải LED RGB
#define NUM_PIXELS   16   // Tổng số bóng LED trên dải

// Định nghĩa chân điều khiển Quạt (Dùng mạch cầu H hoặc Transistor)
#define PIN_FAN1_A   18   // Chân PWM điều tốc quạt 1
#define PIN_FAN1_B   19   // Chân mức thấp (GND) cho quạt 1
#define PIN_FAN2_A   21   // Chân PWM điều tốc quạt 2
#define PIN_FAN2_B   22   // Chân mức thấp (GND) cho quạt 2

// Cảm biến môi trường và an toàn
#define PIN_SHT_SDA  32   // Chân I2C SDA cho cảm biến nhiệt độ SHT31
#define PIN_SHT_SCL  33   // Chân I2C SCL cho cảm biến nhiệt độ SHT31
#define PIN_MQ2_AO   34   // Chân Analog đọc nồng độ Gas/Khói
#define PIN_MQ2_DO   35   // Chân Digital cảnh báo Gas (ngưỡng cứng)
#define PIN_FIRE_SENSOR 27 // Cảm biến lửa (Mức thấp LOW là có lửa)

// --- CẤU HÌNH PWM CHO QUẠT ---
#define PWM_FREQ     5000 // Tần số 5kHz
#define PWM_RES      8    // Độ phân giải 8-bit (0-255)
#define PWM_LV0      0    // Tắt
#define PWM_LV1      160  // Tốc độ thấp
#define PWM_LV2      210  // Tốc độ trung bình
#define PWM_LV3      255  // Tốc độ tối đa

// --- CẤU HÌNH CẢNH BÁO ---
#define GAS_THRESHOLD_PPM 300
#define GAS_HYSTERESIS    50
const unsigned long GAS_ALERT_COOLDOWN = 180000; // Chống spam cảnh báo (3 phút)

// --- CẤU HÌNH THỜI GIAN (NTP) ---
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600; // Múi giờ Việt Nam (UTC+7)
const int   daylightOffset_sec = 0;

// --- ĐỊNH DANH THIẾT BỊ (UUID) ---
const char* ID_LED_STRIP_1 = "47e0143b-98d5-45a5-8b46-c03e9480f3e9";
const char* ID_LED_STRIP_2 = "d81cef67-8490-4fd8-87d3-e864c998183c";
const char* ID_FAN_1       = "b792016c-8a51-4dc1-bdc4-cfb9c7679296";
const char* ID_FAN_2       = "e91c75b5-5a0c-4ec1-81a7-fb00660939cf";

// --- CẤU HÌNH MQTT ---
const char* mqtt_server = "68.183.188.187";
const int   mqtt_port   = 1885;
const char* mqtt_user   = "mqtt_admin";
const char* mqtt_pass   = "12345678@abc";
const char* topic_telemetry = "smart_home/controller-01/telemetry"; // Gửi dữ liệu cảm biến
const char* topic_warning   = "smart_home/eeb1dee9-db9f-48bc-89e4-fc1fd9586ede/warning"; // Gửi cảnh báo cháy/gas

// Khởi tạo các đối tượng điều khiển
WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel pixels(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
Adafruit_SHT31 sht31 = Adafruit_SHT31();

// Biến quản lý thời gian gửi dữ liệu định kỳ
unsigned long lastTelemetryTime = 0;
const long telemetryInterval = 5000; // 5 giây gửi 1 lần

// Biến trạng thái cảnh báo để tránh gửi tin nhắn liên tục
bool isFireAlertSent = false;
bool isGasAlertSent = false;
unsigned long lastGasAlertTime = 0; 

// Cấu trúc dữ liệu lưu trạng thái từng thiết bị
struct DeviceState {
  String id;           // UUID thiết bị
  String type;         // Loại: light hoặc fan
  bool state;          // Trạng thái: true (Bật), false (Tắt)
  int value;           // Độ sáng (0-100) hoặc Tốc độ quạt (0-3)
  String color;        // Mã màu Hex (chỉ cho LED)
  unsigned long timer_end_at; // Thời điểm tự động tắt (ms)
};

// Khởi tạo trạng thái mặc định cho 4 thiết bị
DeviceState dev_led1 = {ID_LED_STRIP_1, "light", false, 100, "#FFFFFF", 0};
DeviceState dev_led2 = {ID_LED_STRIP_2, "light", false, 100, "#FFFFFF", 0};
DeviceState dev_fan1 = {ID_FAN_1,       "fan",   false, 0,   "",        0};
DeviceState dev_fan2 = {ID_FAN_2,       "fan",   false, 0,   "",        0};

/**
 * Lấy thời gian hiện tại từ NTP server theo định dạng Epoch Miliseconds
 */
unsigned long long getEpochTimeMs() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)) return 0;
  time_t now;
  time(&now);
  return ((unsigned long long)now + (7 * 3600)) * 1000;
}

/**
 * Chuyển đổi mã màu Hex (vd: #FF0000) sang định dạng RGB 32-bit cho NeoPixel
 */
uint32_t hexToColor(String hex) {
  if (hex.startsWith("#")) hex.remove(0, 1);
  long number = strtol(hex.c_str(), NULL, 16);
  return pixels.Color((number >> 16) & 0xFF, (number >> 8) & 0xFF, number & 0xFF);
}

/**
 * Điều khiển phần cứng quạt thông qua PWM
 * @param level: Mức tốc độ từ 0 đến 3
 */
void controlFanHW(int pinPWM, int pinLow, int level) {
  int pwmVal = 0;
  switch (level) {
    case 1:  pwmVal = PWM_LV1; break;
    case 2:  pwmVal = PWM_LV2; break;
    case 3:  pwmVal = PWM_LV3; break;
    default: pwmVal = PWM_LV0; break;
  }
  digitalWrite(pinLow, LOW);   // Đảm bảo chân đối diện luôn ở mức thấp
  ledcWrite(pinPWM, pwmVal);   // Xuất xung PWM điều tốc
}

/**
 * Cập nhật màu sắc thực tế lên dải LED dựa vào biến trạng thái
 * Chia 16 LED thành 2 nhóm: 0-7 (LED 1) và 8-15 (LED 2)
 */
void updateLeds() {
  // Nhóm 1 (LED Strip 1)
  uint32_t c1 = dev_led1.state ? hexToColor(dev_led1.color) : 0;
  for(int i=0; i<8; i++) {
    if(dev_led1.state) {
      // Tính toán độ sáng dựa trên tỷ lệ % (dev_led1.value)
      uint8_t r = (uint8_t)(c1 >> 16) * dev_led1.value / 100;
      uint8_t g = (uint8_t)(c1 >> 8)  * dev_led1.value / 100;
      uint8_t b = (uint8_t)c1         * dev_led1.value / 100;
      pixels.setPixelColor(i, pixels.Color(r,g,b));
    } else pixels.setPixelColor(i, 0);
  }

  // Nhóm 2 (LED Strip 2)
  uint32_t c2 = dev_led2.state ? hexToColor(dev_led2.color) : 0;
  for(int i=8; i<16; i++) {
     if(dev_led2.state) {
      uint8_t r = (uint8_t)(c2 >> 16) * dev_led2.value / 100;
      uint8_t g = (uint8_t)(c2 >> 8)  * dev_led2.value / 100;
      uint8_t b = (uint8_t)c2         * dev_led2.value / 100;
      pixels.setPixelColor(i, pixels.Color(r,g,b));
    } else pixels.setPixelColor(i, 0);
  }
  pixels.show(); // Đẩy dữ liệu ra bóng LED
}

/**
 * Cập nhật tốc độ quạt thực tế
 */
void updateFans() {
  int speed1 = dev_fan1.state ? dev_fan1.value : 0;
  int speed2 = dev_fan2.state ? dev_fan2.value : 0;
  controlFanHW(PIN_FAN1_A, PIN_FAN1_B, speed1);
  controlFanHW(PIN_FAN2_A, PIN_FAN2_B, speed2);
}

/**
 * Gửi trạng thái hiện tại của thiết bị lên server MQTT
 */
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

  // Nếu có hẹn giờ, tính toán số phút còn lại để báo về App
  if (dev.timer_end_at > 0 && dev.state) {
    long remaining = (dev.timer_end_at - millis()) / 60000;
    params["timer_left"] = (remaining < 0) ? 0 : remaining;
  }

  char payloadBuffer[256];
  serializeJson(doc, payloadBuffer);
  
  char topicBuffer[128];
  snprintf(topicBuffer, sizeof(topicBuffer), "smart_home/controller-01/%s/status", dev.id.c_str());
  client.publish(topicBuffer, payloadBuffer, true); // true = Retain message (giữ trạng thái trên server)
}

/**
 * Gửi tin nhắn cảnh báo (Cháy/Gas) lên topic warning
 */
void sendWarning(String alertType, String severity, String msg) {
  if (!client.connected()) return;

  StaticJsonDocument<256> doc;
  doc["alertType"] = alertType;
  doc["severity"] = severity;
  doc["message"] = msg;
  doc["timestamp"] = getEpochTimeMs();

  char buffer[256];
  serializeJson(doc, buffer);
  client.publish(topic_warning, buffer);
}

/**
 * Kiểm tra các cảm biến an toàn và xử lý logic báo động
 */
void handleSafetySensors() {
  // 1. Kiểm tra cảm biến lửa
  bool isFire = (digitalRead(PIN_FIRE_SENSOR) == LOW);
  if (isFire) {
    if (!isFireAlertSent) {
      sendWarning("FIRE", "CRITICAL", "Báo động có đám cháy!");
      isFireAlertSent = true;
    }
  } else {
    isFireAlertSent = false; // Reset trạng thái khi hết lửa
  }

  // 2. Kiểm tra cảm biến Gas
  int gasValue = analogRead(PIN_MQ2_AO);
  if(gasValue >= 1000){ // Ngưỡng nguy hiểm
    if(!isGasAlertSent){
      sendWarning("GAS", "WARNING", "Cảnh báo rò rỉ khí ga, hãy kiểm tra ngay!");
      isGasAlertSent = true;
    }
  } else {
    isGasAlertSent = false;
  }
}

/**
 * Đọc cảm biến SHT31 và gửi dữ liệu môi trường (Telemetry) định kỳ
 */
void handleTelemetry() {
  unsigned long currentMillis = millis();
  if (currentMillis - lastTelemetryTime >= telemetryInterval) {
    lastTelemetryTime = currentMillis;

    float t = sht31.readTemperature();
    float h = sht31.readHumidity();
    if (isnan(t)) t = 0.0;
    if (isnan(h)) h = 0.0;

    int gasValue = analogRead(PIN_MQ2_AO);

    StaticJsonDocument<256> doc;
    doc["temperature"] = ((int)(t * 100)) / 100.0; // Làm tròn 2 chữ số thập phân
    doc["humidity"] = ((int)(h * 100)) / 100.0;
    doc["gas"] = gasValue;
    doc["timestamp"] = getEpochTimeMs();

    char buffer[256];
    serializeJson(doc, buffer);
    if (client.connected()) client.publish(topic_telemetry, buffer);
  }
}

/**
 * Hàm xử lý khi nhận lệnh từ App qua MQTT (Topic: .../cmd)
 */
void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload, length)) return;

  const char* r_id = doc["device_id"];
  String receivedId = String(r_id);
  JsonObject params = doc["params"];
  DeviceState* target = nullptr;

  // Xác định thiết bị nào cần điều khiển
  if (receivedId == ID_LED_STRIP_1) target = &dev_led1;
  else if (receivedId == ID_LED_STRIP_2) target = &dev_led2;
  else if (receivedId == ID_FAN_1) target = &dev_fan1;
  else if (receivedId == ID_FAN_2) target = &dev_fan2;

  if (target) {
    // Xử lý bật/tắt
    if (params.containsKey("state")) {
      const char* st = params["state"];
      target->state = (strcmp(st, "ON") == 0);
      if (target->type == "fan" && !target->state) target->value = 0;
      if (target->type == "fan" && target->state && target->value == 0) target->value = 1;
    }
    // Xử lý thông số riêng cho Đèn
    if (target->type == "light") {
      if (params.containsKey("brightness")) target->value = params["brightness"];
      if (params.containsKey("color")) target->color = params["color"].as<String>();
    } 
    // Xử lý thông số riêng cho Quạt
    else {
      if (params.containsKey("speed")) {
          int s = params["speed"];
          if (s >= 0 && s <= 3) {
              target->value = s;
              target->state = (s > 0);
          }
      }
    }
    // Xử lý hẹn giờ tắt
    if (params.containsKey("timer_off")) {
      int minutes = params["timer_off"];
      target->timer_end_at = (minutes > 0) ? (millis() + minutes * 60000) : 0;
    }
    
    // Cập nhật phần cứng và báo cáo lại trạng thái mới
    updateLeds();
    updateFans();
    publishStatus(*target);
  }
}

void setup() {
  Serial.begin(115200);

  // Khởi tạo dải LED
  pixels.begin();
  pixels.show();

  // Khởi tạo PWM cho quạt
  ledcAttach(PIN_FAN1_A, PWM_FREQ, PWM_RES);
  ledcAttach(PIN_FAN2_A, PWM_FREQ, PWM_RES);
  pinMode(PIN_FAN1_B, OUTPUT);
  pinMode(PIN_FAN2_B, OUTPUT);
  digitalWrite(PIN_FAN1_B, LOW);
  digitalWrite(PIN_FAN2_B, LOW);
  
  // Khởi tạo cảm biến nhiệt độ I2C
  Wire.begin(PIN_SHT_SDA, PIN_SHT_SCL);
  if (!sht31.begin(0x44)) Serial.println("Couldn't find SHT31");

  // Khởi tạo cảm biến an toàn
  pinMode(PIN_MQ2_AO, INPUT);
  pinMode(PIN_MQ2_DO, INPUT);
  pinMode(PIN_FIRE_SENSOR, INPUT_PULLUP);

  // WiFiManager: Tự động tạo điểm phát WiFi nếu không kết nối được
  WiFiManager wm;
  if(!wm.autoConnect("SmartHome_Config", "12345678")) ESP.restart();
  
  // Đồng bộ thời gian thực từ Internet
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

/**
 * Hàm duy trì kết nối MQTT
 */
void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32_Controller_01";
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      client.subscribe("smart_home/controller-01/cmd");
      // Gửi trạng thái ban đầu khi vừa kết nối lại
      publishStatus(dev_led1);
      publishStatus(dev_led2);
      publishStatus(dev_fan1);
      publishStatus(dev_fan2);
    } else delay(5000);
  }
}

/**
 * Kiểm tra bộ đếm giờ (Timer) để tự động tắt thiết bị
 */
void checkTimer(DeviceState &dev) {
  if (dev.state && dev.timer_end_at > 0) {
    if (millis() > dev.timer_end_at) {
      dev.state = false;
      dev.value = 0;
      dev.timer_end_at = 0;
      if (dev.type == "light") updateLeds(); else updateFans();
      publishStatus(dev);
    }
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop(); // Duy trì giao tiếp MQTT

  // Kiểm tra hẹn giờ cho từng thiết bị
  checkTimer(dev_led1);
  checkTimer(dev_led2);
  checkTimer(dev_fan1);
  checkTimer(dev_fan2);

  handleTelemetry();      // Gửi dữ liệu nhiệt độ/độ ẩm/gas
  handleSafetySensors();  // Theo dõi báo cháy/rò rỉ gas
}