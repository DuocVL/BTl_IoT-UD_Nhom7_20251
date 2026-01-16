// SMART HOME CONTROLLER - ESP32 (FreeRTOS Optimized)

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h> 
#include <vector>

// PHẦN CỨNG
#define SS_PIN          5
#define RST_PIN         22
#define PIN_LED_STRIP   26 
#define NUM_LEDS        16  
#define PIN_SERVO       4
#define PIN_PIR         34

// MQTT CONFIG
const char* mqtt_server = "68.183.188.187";
const int   mqtt_port   = 1885;
const char* mqtt_user   = "mqtt_admin";
const char* mqtt_pass   = "12345678@abc";

const char* topic_cmd        = "smart_home/controller-02/cmd";
const char* topic_rfid_event = "smart_home/controller-02/rfid/event";
const char* topic_rfid_list  = "smart_home/controller-02/rfid/list";
const char* topic_status     = "smart_home/controller-02/112/status";

// ĐỐI TƯỢNG TOÀN CỤC
WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel pixels(NUM_LEDS, PIN_LED_STRIP, NEO_GRB + NEO_KHZ800);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo myServo;
Preferences preferences; 

// QUẢN LÝ TRẠNG THÁI LED
// Cấu trúc dữ liệu dùng chung để đồng bộ giữa các Task
struct LedState {
  bool manual_on;              // Bật cưỡng bức từ App
  uint32_t color;              // Màu LED
  int brightness;              // Độ sáng
  bool pir_active;             // Trạng thái PIR
  unsigned long pir_timer;     // Timer tắt LED sau PIR
} sysLed;

// Mutex bảo vệ sysLed
SemaphoreHandle_t ledMutex; 

// QUẢN LÝ HỆ THỐNG
enum SystemMode { MODE_NORMAL, MODE_ADD, MODE_DELETE };
SystemMode currentMode = MODE_NORMAL;

bool servoOpen = false;
unsigned long servoTimer = 0;
std::vector<String> authorizedCards;

// Gói tin MQTT dùng cho Queue
struct MqttMessage {
  char topic[64];
  char payload[256];
};

QueueHandle_t mqttQueue;

// KHAI BÁO HÀM
void loadCards();
void saveCard(String uid);
void deleteCard(String uid);
bool checkCard(String uid);
String dumpByteArray(byte *buffer, byte bufferSize);
void sendMqttFromTask(const char* topic, String payload);
uint32_t hexToColor(String hex);
void publishCardList();
void publishLedStatus(bool isOn);

// TASK 1: XỬ LÝ CẢM BIẾN PIR
void TaskPIR(void *pvParameters) {
  pinMode(PIN_PIR, INPUT);

  for (;;) {
    int val = digitalRead(PIN_PIR);

    // Chiếm quyền truy cập biến sysLed bằng Mutex
    if (xSemaphoreTake(ledMutex, portMAX_DELAY)) {
      if (val == HIGH) {
        sysLed.pir_active = true;
        sysLed.pir_timer = millis() + 3000;
      }
      // Kiểm tra nếu hết thời gian chờ thì tắt flag PIR
      if (sysLed.pir_active && millis() > sysLed.pir_timer) {
        sysLed.pir_active = false;
      }
      xSemaphoreGive(ledMutex);// Giải phóng khóa sau khi ghi xong
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// TASK 2: ĐIỀU KHIỂN LED NEOPIXEL
void TaskLED(void *pvParameters) {
  pixels.begin();
  pixels.clear();
  pixels.show();

  bool lastLedState = false;

  for (;;) {
    bool shouldBeOn;
    uint32_t targetColor;
    int targetBright;
    // Đọc thông số điều khiển từ biến chung sysLed
    if (xSemaphoreTake(ledMutex, portMAX_DELAY)) {
      shouldBeOn   = sysLed.manual_on || sysLed.pir_active;
      targetColor  = sysLed.color;
      targetBright = sysLed.brightness;
      xSemaphoreGive(ledMutex);
    }
    // Chỉ gửi tin nhắn MQTT khi trạng thái LED thực tế thay đổi
    if (shouldBeOn != lastLedState) {
      publishLedStatus(shouldBeOn);
      lastLedState = shouldBeOn;
    }

    // Cập nhật hiệu ứng lên dải LED
    pixels.setBrightness(targetBright);
    if (shouldBeOn) {
      for (int i = 0; i < NUM_LEDS; i++)
        pixels.setPixelColor(i, targetColor);
    } else {
      pixels.clear();
    }
    pixels.show();
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// TASK 3: ĐIỀU KHIỂN SERVO
void TaskServo(void *pvParameters) {
  myServo.write(0);

  for (;;) {
    if (servoOpen) {
      myServo.write(90);

      // Tự động đóng cửa sau khi hết thời gian timer
      if (millis() > servoTimer) {
        servoOpen = false;
        myServo.write(0);
        sendMqttFromTask(
          topic_status,
          "{\"device\":\"servo\",\"state\":\"LOCKED\"}"
        );
      }
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

// TASK 4: RFID
void TaskRFID(void *pvParameters) {
  SPI.begin();
  mfrc522.PCD_Init();

  for (;;) {
    // Kiểm tra xem có thẻ mới đưa vào vùng đọc không
    if (!mfrc522.PICC_IsNewCardPresent() ||
        !mfrc522.PICC_ReadCardSerial()) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    // Chuyển mã UID của thẻ sang dạng chuỗi HEX
    String uidStr = dumpByteArray(
      mfrc522.uid.uidByte,
      mfrc522.uid.size
    );

    StaticJsonDocument<300> doc;
    doc["uid"] = uidStr;
    //xử lý theo chế độ Mode hiện tại
    if (currentMode == MODE_ADD) {
      if (!checkCard(uidStr)) {
        saveCard(uidStr);// Lưu vào bộ nhớ Flash
        doc["status"] = "SUCCESS";
      } else {
        doc["status"] = "EXISTED";
      }
      doc["action"] = "ADDED";
      currentMode = MODE_NORMAL;
      sendMqttFromTask(topic_rfid_list, "UPDATE_REQUEST");
    }
    else if (currentMode == MODE_DELETE) {
      if (checkCard(uidStr)) {
        deleteCard(uidStr);// Xóa khỏi bộ nhớ Flash
        doc["status"] = "SUCCESS";
      } else {
        doc["status"] = "NOT_FOUND";
      }
      doc["action"] = "DELETED";
      currentMode = MODE_NORMAL;
      sendMqttFromTask(topic_rfid_list, "UPDATE_REQUEST");
    }
    else {
      // Chế độ bình thường: Kiểm tra quyền truy cập để mở cửa
      if (checkCard(uidStr)) {
        doc["access"] = "GRANTED";
        servoOpen = true;
        servoTimer = millis() + 5000;// Hẹn giờ đóng cửa sau 5s
      } else {
        doc["access"] = "DENIED";
      }
    }
    // Đẩy kết quả quẹt thẻ vào Queue để gửi lên MQTT
    String out;
    serializeJson(doc, out);
    sendMqttFromTask(topic_rfid_event, out);

    // Lệnh dừng giao tiếp với thẻ để tránh việc đọc lặp lại nhiều lần
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}


// Gửi tin nhắn vào hàng đợi an toàn cho đa luồng
void sendMqttFromTask(const char* topic, String payload) {
  MqttMessage msg;
  strncpy(msg.topic, topic, sizeof(msg.topic));
  strncpy(msg.payload, payload.c_str(), sizeof(msg.payload));
  xQueueSend(mqttQueue, &msg, 0); // Đẩy gói tin vào đuôi hàng đợi
}

// Chuyển mã màu Hex (vd: #FF0000) sang định dạng uint32 của NeoPixel
uint32_t hexToColor(String hex) {
  if (hex.startsWith("#")) hex.remove(0, 1);
  long number = strtol(hex.c_str(), NULL, 16);
  return (uint32_t)number;
}

// Chuyển mảng byte UID sang String để dễ so sánh
String dumpByteArray(byte *buffer, byte bufferSize) {
  String res = ""; 
  for (byte i = 0; i < bufferSize; i++) { 
    res += (buffer[i] < 0x10 ? "0" : ""); 
    res += String(buffer[i], HEX); 
  }
  res.toUpperCase(); 
  return res;
}

// Tải danh sách thẻ từ bộ nhớ Flash vào RAM khi khởi động
void loadCards() {
  preferences.begin("rfid_data", true); 
  String cardString = preferences.getString("cards", "");
  preferences.end();
  
  authorizedCards.clear();
  int start = 0;
  int end = cardString.indexOf(',');
  while (end != -1) {
    authorizedCards.push_back(cardString.substring(start, end));
    start = end + 1;
    end = cardString.indexOf(',', start);
  }
  if (start < cardString.length()) authorizedCards.push_back(cardString.substring(start));
}

// Gói trạng thái LED hiện tại thành JSON và gửi đi
void publishLedStatus(bool isOn) {
  StaticJsonDocument<256> doc;
  doc["device"] = "led";
  doc["state"]  = isOn ? "ON" : "OFF";
  doc["mode"]   = sysLed.manual_on ? "MANUAL" : "AUTO";
  doc["brightness"] = sysLed.brightness;

  char buffer[256];
  serializeJson(doc, buffer);
  sendMqttFromTask(topic_status, buffer);
}

// Lưu danh sách mảng RAM xuống bộ nhớ Flash bền vững
void saveCardsToFlash() {
  String cardString = "";
  for (size_t i = 0; i < authorizedCards.size(); i++) {
    cardString += authorizedCards[i];
    if (i < authorizedCards.size() - 1) cardString += ",";
  }
  preferences.begin("rfid_data", false); 
  preferences.putString("cards", cardString);
  preferences.end();
}

void saveCard(String uid) { authorizedCards.push_back(uid); saveCardsToFlash(); }

void deleteCard(String uid) { 
  for (auto it = authorizedCards.begin(); it != authorizedCards.end(); ++it) {
    if (*it == uid) { authorizedCards.erase(it); saveCardsToFlash(); break; }
  }
}

bool checkCard(String uid) { 
  for (String s : authorizedCards) if (s == uid) return true; 
  return false; 
}

// Gửi toàn bộ danh sách thẻ hiện có lên MQTT cho App cập nhật
void publishCardList() {
  StaticJsonDocument<1024> doc; 
  JsonArray list = doc.createNestedArray("cards");
  for (String s : authorizedCards) list.add(s);
  
  char buffer[1024]; 
  serializeJson(doc, buffer); 
  client.publish(topic_rfid_list, buffer, true);
}


// Hàm xử lý khi có dữ liệu từ Server gửi xuống (Topic: topic_cmd)
void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  deserializeJson(doc, payload, length);

  // Xử lý nhóm lệnh điều khiển LED
  if (doc.containsKey("led")) {
    JsonObject ledCmd = doc["led"];
    if (xSemaphoreTake(ledMutex, portMAX_DELAY)) {
      if (ledCmd.containsKey("state")) {
        const char* st = ledCmd["state"];
        sysLed.manual_on = (strcmp(st, "ON") == 0);
      }
      if (ledCmd.containsKey("color")) {
        sysLed.color = hexToColor(ledCmd["color"].as<String>());
      }
      if (ledCmd.containsKey("brightness")) {
        sysLed.brightness = ledCmd["brightness"];
      }
      xSemaphoreGive(ledMutex);
    }
    sendMqttFromTask(topic_status, "{\"msg\":\"LED Updated\"}");
  }

  // Xử lý nhóm lệnh thay đổi chế độ quản lý thẻ
  if (doc.containsKey("mode")) {
    const char* mode = doc["mode"];
    if (strcmp(mode, "ADD") == 0) currentMode = MODE_ADD;
    else if (strcmp(mode, "DELETE") == 0) currentMode = MODE_DELETE;
    else currentMode = MODE_NORMAL;
    
    sendMqttFromTask(topic_status, "{\"mode_changed\":\"" + String(mode) + "\"}");
  }
}

// Hàm duy trì kết nối MQTT Broker
void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32-Home-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      client.subscribe(topic_cmd);
      publishCardList(); 
    } else delay(5000);
  }
}

void setup() {
  Serial.begin(115200);

  // Khởi tạo các giá trị mặc định cho hệ thống LED
  sysLed.manual_on = false; 
  sysLed.color = pixels.Color(255, 255, 255); 
  sysLed.brightness = 100;
  sysLed.pir_active = false;

  ledMutex = xSemaphoreCreateMutex(); // Tạo Mutex để bảo vệ biến dùng chung
  mqttQueue = xQueueCreate(10, sizeof(MqttMessage)); // Khởi tạo hàng đợi 10 tin nhắn

  myServo.attach(PIN_SERVO);
  loadCards(); 

  // Cấu hình Wi-Fi SmartConfig
  WiFiManager wm;
  if (!wm.autoConnect("SmartHome_Ctrl","12345678")) ESP.restart();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // KHỞI TẠO CÁC TASK CHẠY SONG SONG TRÊN CORE 1
  // Tham số: Tên hàm, Tên Task, Stack size, Tham số, Độ ưu tiên, Handle, Core ID
  xTaskCreatePinnedToCore(TaskRFID, "TaskRFID", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskPIR,  "TaskPIR",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskServo,"TaskServo",2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskLED,  "TaskLED",  2048, NULL, 2, NULL, 1);
}

void loop() {
  // Loop chính chỉ tập trung quản lý kết nối và đẩy tin nhắn từ Queue lên Server
  if (!client.connected()) reconnect();
  client.loop();

  // Kiểm tra nếu có dữ liệu chờ trong hàng đợi
  MqttMessage msg;
  if (xQueueReceive(mqttQueue, &msg, 0) == pdTRUE) {
    if (String(msg.payload) == "UPDATE_REQUEST") publishCardList();
    else client.publish(msg.topic, msg.payload, true);
  }
}
