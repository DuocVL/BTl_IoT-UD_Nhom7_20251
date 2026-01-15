/****************************************************
 * SMART HOME CONTROLLER - ESP32 (FreeRTOS Optimized)
 * Chức năng:
 * - 1 Dải LED (Pin 26): Manual (MQTT) + Auto (PIR)
 * - Quản lý thẻ RFID: Thêm/Xóa/Mở cửa qua MQTT
 * - Servo: Đóng mở cửa tự động
 ****************************************************/

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Preferences.h> // Lưu trữ thẻ vào Flash
#include <vector>

/*************** PHẦN CỨNG ****************/
#define SS_PIN          5
#define RST_PIN         22

// CHỈ DÙNG 1 DẢI LED
#define PIN_LED_STRIP   26 
#define NUM_LEDS        16  // Số lượng bóng LED (tùy chỉnh)

#define PIN_SERVO       4
#define PIN_PIR         34

/*************** MQTT CONFIG ****************/
const char* mqtt_server = "68.183.188.187";
const int   mqtt_port   = 1885;
const char* mqtt_user   = "mqtt_admin";
const char* mqtt_pass   = "12345678@abc";

// Các Topic MQTT
const char* topic_cmd        = "smart_home/controller-02/cmd";
const char* topic_rfid_event = "smart_home/controller-02/rfid/event";
const char* topic_rfid_list  = "smart_home/controller-02/rfid/list";
const char* topic_status     = "smart_home/controller-02/status";

/*************** ĐỐI TƯỢNG TOÀN CỤC ***************/
WiFiClient espClient;
PubSubClient client(espClient);

Adafruit_NeoPixel pixels(NUM_LEDS, PIN_LED_STRIP, NEO_GRB + NEO_KHZ800);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo myServo;
Preferences preferences; // Đối tượng lưu trữ Flash

/*************** QUẢN LÝ TRẠNG THÁI LED (SHARED) ***************/
// Cấu trúc này được chia sẻ giữa TaskPIR (ghi), TaskMQTT (ghi) và TaskLED (đọc)
struct LedState {
  bool manual_on;       // True: Bật thủ công qua MQTT (ghi đè PIR)
  uint32_t color;       // Màu sắc
  int brightness;       // Độ sáng
  bool pir_active;      // True: PIR đang phát hiện chuyển động
  unsigned long pir_timer; // Thời gian đếm lùi tắt PIR
} sysLed;
bool lastLedOn = false;


SemaphoreHandle_t ledMutex; // Khóa bảo vệ biến sysLed (tránh xung đột dữ liệu)

/*************** QUẢN LÝ HỆ THỐNG ***************/
enum SystemMode { MODE_NORMAL, MODE_ADD, MODE_DELETE };
SystemMode currentMode = MODE_NORMAL;

bool servoOpen = false;
unsigned long servoTimer = 0;
std::vector<String> authorizedCards; // Danh sách thẻ trong RAM

// Hàng đợi tin nhắn MQTT (Để các Task con gửi tin về Loop chính)
struct MqttMessage {
  char topic[64];
  char payload[256];
};
QueueHandle_t mqttQueue;

/*************** KHAI BÁO HÀM ***************/
void loadCards();
void saveCard(String uid);
void deleteCard(String uid);
bool checkCard(String uid);
String dumpByteArray(byte *buffer, byte bufferSize);
void sendMqttFromTask(const char* topic, String payload);
uint32_t hexToColor(String hex);
void publishCardList();

/****************************************************************
 * TASK 1: PIR SENSOR (Core 1)
 * Đọc cảm biến chuyển động, cập nhật trạng thái vào sysLed
 ****************************************************************/
void TaskPIR(void *pvParameters) {
  pinMode(PIN_PIR, INPUT);
  
  for (;;) {
    int val = digitalRead(PIN_PIR);
    
    // Dùng Mutex để ghi vào biến shared an toàn
    if (xSemaphoreTake(ledMutex, portMAX_DELAY)) {
      if (val == HIGH) {
        sysLed.pir_active = true;
        sysLed.pir_timer = millis() + 3000; // Có chuyển động -> Reset timer 3s
      }
      
      // Kiểm tra timeout: Nếu quá 3s không có chuyển động -> Tắt flag PIR
      if (sysLed.pir_active && millis() > sysLed.pir_timer) {
        sysLed.pir_active = false;
      }
      xSemaphoreGive(ledMutex);
    }
    
    vTaskDelay(100 / portTICK_PERIOD_MS); // Check mỗi 100ms
  }
}

/****************************************************************
 * TASK 2: LED CONTROLLER (Core 1)
 * Điều khiển thực tế NeoPixel dựa trên logic: Manual || PIR
 ****************************************************************/
/****************************************************************
 * TASK 2: LED CONTROLLER (Core 1)
 * Điều khiển NeoPixel + gửi status khi ON/OFF thay đổi
 ****************************************************************/
void TaskLED(void *pvParameters) {
  pixels.begin();
  pixels.clear();
  pixels.show();

  bool lastLedOn = false; // Lưu trạng thái LED trước đó

  for (;;) {
    bool shouldBeOn;
    uint32_t targetColor;
    int targetBright;

    /* Đọc trạng thái dùng Mutex */
    if (xSemaphoreTake(ledMutex, portMAX_DELAY)) {
      shouldBeOn   = sysLed.manual_on || sysLed.pir_active;
      targetColor  = sysLed.color;
      targetBright = sysLed.brightness;
      xSemaphoreGive(ledMutex);
    }

    /* Nếu trạng thái LED thay đổi -> gửi status */
    if (shouldBeOn != lastLedOn) {
      publishLedStatus(shouldBeOn);
      lastLedOn = shouldBeOn;
    }

    /* Điều khiển LED */
    pixels.setBrightness(targetBright);

    if (shouldBeOn) {
      for (int i = 0; i < NUM_LEDS; i++) {
        pixels.setPixelColor(i, targetColor);
      }
    } else {
      pixels.clear();
    }

    pixels.show();
    vTaskDelay(100 / portTICK_PERIOD_MS); // 10Hz là đủ
  }
}


/****************************************************************
 * TASK 3: SERVO (Core 1)
 * Quản lý đóng/mở cửa không chặn chương trình
 ****************************************************************/
void TaskServo(void *pvParameters) {
  myServo.write(0); // Mặc định khóa
  
  for (;;) {
    if (servoOpen) {
      myServo.write(90); // Mở
      
      // Kiểm tra hết giờ mở cửa
      if (millis() > servoTimer) {
        servoOpen = false;
        myServo.write(0); // Đóng lại
        sendMqttFromTask(topic_status, "{\"device\":\"servo\", \"state\":\"LOCKED\"}");
      }
    } else {
      // Đảm bảo luôn đóng nếu không có cờ mở
      myServo.write(0); 
    }
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }
}

/****************************************************************
 * TASK 4: RFID READER (Core 0)
 * Quét thẻ và xử lý logic Thêm/Xóa/Mở
 ****************************************************************/
void TaskRFID(void *pvParameters) {
  SPI.begin();
  mfrc522.PCD_Init();

  for (;;) {
    // Nếu không có thẻ -> nghỉ 50ms rồi check lại
    if (!mfrc522.PICC_IsNewCardPresent() || !mfrc522.PICC_ReadCardSerial()) {
      vTaskDelay(50 / portTICK_PERIOD_MS);
      continue;
    }

    // Có thẻ -> Đọc UID
    String uidStr = dumpByteArray(mfrc522.uid.uidByte, mfrc522.uid.size);
    StaticJsonDocument<300> doc;
    doc["uid"] = uidStr;

    // XỬ LÝ THEO CHẾ ĐỘ HIỆN TẠI
    if (currentMode == MODE_ADD) {
      if (!checkCard(uidStr)) {
        saveCard(uidStr);
        doc["action"] = "ADDED";
        doc["status"] = "SUCCESS";
      } else {
        doc["action"] = "ADDED";
        doc["status"] = "EXISTED";
      }
      currentMode = MODE_NORMAL; // Xong việc quay về bình thường
      sendMqttFromTask(topic_rfid_list, "UPDATE_REQUEST"); // Yêu cầu gửi lại danh sách
    } 
    else if (currentMode == MODE_DELETE) {
      if (checkCard(uidStr)) {
        deleteCard(uidStr);
        doc["action"] = "DELETED";
        doc["status"] = "SUCCESS";
      } else {
        doc["action"] = "DELETED";
        doc["status"] = "NOT_FOUND";
      }
      currentMode = MODE_NORMAL;
      sendMqttFromTask(topic_rfid_list, "UPDATE_REQUEST");
    } 
    else { 
      // MODE NORMAL: Mở cửa
      if (checkCard(uidStr)) {
        doc["access"] = "GRANTED";
        servoOpen = true; // Kích hoạt TaskServo
        servoTimer = millis() + 5000; // Mở 5s
      } else {
        doc["access"] = "DENIED";
      }
    }

    // Gửi sự kiện quẹt thẻ lên MQTT
    String jsonOutput;
    serializeJson(doc, jsonOutput);
    sendMqttFromTask(topic_rfid_event, jsonOutput);

    // Dừng thẻ để không đọc lại liên tục
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
    vTaskDelay(1000 / portTICK_PERIOD_MS); // Chống spam thẻ
  }
}

/****************************************************************
 * HÀM HỖ TRỢ & LOGIC DỮ LIỆU
 ****************************************************************/

// Đẩy message vào hàng đợi để Main Loop gửi đi (Thread-safe)
void sendMqttFromTask(const char* topic, String payload) {
  MqttMessage msg;
  strncpy(msg.topic, topic, sizeof(msg.topic));
  strncpy(msg.payload, payload.c_str(), sizeof(msg.payload));
  xQueueSend(mqttQueue, &msg, 0);
}

// Chuyển đổi HEX color
uint32_t hexToColor(String hex) {
  if (hex.startsWith("#")) hex.remove(0, 1);
  long number = strtol(hex.c_str(), NULL, 16);
  return (uint32_t)number;
}

// Chuyển UID bytes sang String HEX
String dumpByteArray(byte *buffer, byte bufferSize) {
  String res = ""; 
  for (byte i = 0; i < bufferSize; i++) { 
    res += (buffer[i] < 0x10 ? "0" : ""); 
    res += String(buffer[i], HEX); 
  }
  res.toUpperCase(); 
  return res;
}

// --- QUẢN LÝ FLASH (PREFERENCES) ---
void loadCards() {
  preferences.begin("rfid_data", true); // Read mode
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

/* Gửi trạng thái LED hiện tại lên MQTT */
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


void saveCardsToFlash() {
  String cardString = "";
  for (size_t i = 0; i < authorizedCards.size(); i++) {
    cardString += authorizedCards[i];
    if (i < authorizedCards.size() - 1) cardString += ",";
  }
  preferences.begin("rfid_data", false); // Write mode
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

// Gửi danh sách thẻ hiện có lên MQTT
void publishCardList() {
  StaticJsonDocument<1024> doc; 
  JsonArray list = doc.createNestedArray("cards");
  for (String s : authorizedCards) list.add(s);
  
  char buffer[1024]; 
  serializeJson(doc, buffer); 
  client.publish(topic_rfid_list, buffer);
}

/****************************************************************
 * MQTT CALLBACK & MAIN SETUP
 ****************************************************************/

// Xử lý lệnh nhận được từ MQTT
void callback(char* topic, byte* payload, unsigned int length) {
  StaticJsonDocument<512> doc;
  deserializeJson(doc, payload, length);

  // 1. LỆNH ĐIỀU KHIỂN LED
  if (doc.containsKey("led")) {
    JsonObject ledCmd = doc["led"];
    
    // Lock Mutex để cập nhật biến sysLed an toàn
    if (xSemaphoreTake(ledMutex, portMAX_DELAY)) {
      if (ledCmd.containsKey("state")) {
        const char* st = ledCmd["state"];
        // Nếu OFF -> Tắt chế độ Manual, chuyển về Auto PIR
        // Nếu ON -> Bật chế độ Manual, đèn sáng mãi
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
    // Gửi phản hồi
    sendMqttFromTask(topic_status, "{\"msg\":\"LED Updated\"}");
  }

  // 2. LỆNH HỆ THỐNG (MODE THẺ)
  if (doc.containsKey("mode")) {
    const char* mode = doc["mode"];
    if (strcmp(mode, "ADD") == 0) {
        currentMode = MODE_ADD;
        sendMqttFromTask(topic_status, "{\"mode\":\"WAITING_FOR_CARD_TO_ADD\"}");
    }
    else if (strcmp(mode, "DELETE") == 0) {
        currentMode = MODE_DELETE;
        sendMqttFromTask(topic_status, "{\"mode\":\"WAITING_FOR_CARD_TO_DELETE\"}");
    }
    else {
        currentMode = MODE_NORMAL;
        sendMqttFromTask(topic_status, "{\"mode\":\"NORMAL\"}");
    }
  }
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32-Home-" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      client.subscribe(topic_cmd);
      publishCardList(); // Gửi danh sách thẻ ngay khi kết nối
    } else delay(5000);
  }
}

void setup() {
  Serial.begin(115200);

  // Cấu hình LED mặc định
  sysLed.manual_on = false; // Mặc định chạy theo PIR
  sysLed.color = pixels.Color(255, 255, 255); // Trắng
  sysLed.brightness = 100;
  sysLed.pir_active = false;
  sysLed.pir_timer = 0;

  ledMutex = xSemaphoreCreateMutex(); // Tạo khóa Mutex

  myServo.attach(PIN_SERVO);
  loadCards(); // Tải thẻ từ Flash

  WiFiManager wm;
  if (!wm.autoConnect("SmartHome_Ctrl","12345678")) ESP.restart();

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);

  // Tạo hàng đợi tin nhắn
  mqttQueue = xQueueCreate(10, sizeof(MqttMessage));

  // KHỞI TẠO CÁC TASKS (FreeRTOS)
  // TaskRFID, LED chạy Core 1; WiFi chạy Core 0 (mặc định của Arduino ESP32)
  xTaskCreatePinnedToCore(TaskRFID, "TaskRFID", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(TaskPIR,  "TaskPIR",  2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskServo,"TaskServo",2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(TaskLED,  "TaskLED",  2048, NULL, 2, NULL, 1);
}

void loop() {
  // Loop này đóng vai trò duy trì kết nối mạng và gửi tin
  if (!client.connected()) reconnect();
  client.loop();

  // Kiểm tra hàng đợi xem có Task nào gửi tin không
  MqttMessage msg;
  if (xQueueReceive(mqttQueue, &msg, 0) == pdTRUE) {
    // Nếu là lệnh nội bộ yêu cầu update list
    if (String(msg.payload) == "UPDATE_REQUEST") publishCardList();
    // Nếu là tin nhắn thường
    else client.publish(msg.topic, msg.payload);
  }
}