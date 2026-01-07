#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Adafruit_NeoPixel.h>
#include <WiFiManager.h>
#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>

#define SS_PIN  5
#define RST_PIN 22
#define PIN_LED_MAIN    26  
#define NUM_LED_MAIN    15 
#define PIN_LED_LOCAL   27
#define NUM_LED_LOCAL   3
#define PIN_SERVO       4
#define PIN_PIR         34 
const char* ID_LED_STRIP_1 = "ced94f1e-7f22-48d1-a78d-87fccc6107a6"; // 8 bóng đầu (0-7)
const char* ID_LED_STRIP_2 = "f3310c9f-3299-4f0d-b2e6-8baf494dc0a5"; // 7 bóng sau (8-14)
const char* ID_SERVO       = "bac49fc9-cb7d-439e-9959-f037061b54d3";
byte MASTER_KEY_UID[] = {0xDE, 0xAD, 0xBE, 0xEF}; 

const char* mqtt_server = "68.183.188.187";
const int   mqtt_port   = 1885;
const char* mqtt_user   = "mqtt_admin";
const char* mqtt_pass   = "12345678@abc";

const char* topic_cmd    = "smart_home/controller-02/cmd";

WiFiClient espClient;
PubSubClient client(espClient);
Adafruit_NeoPixel pixelsMain(NUM_LED_MAIN, PIN_LED_MAIN, NEO_GRB + NEO_KHZ800);
Adafruit_NeoPixel pixelsLocal(NUM_LED_LOCAL, PIN_LED_LOCAL, NEO_GRB + NEO_KHZ800);
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo myServo;

struct DeviceState {
  String id;
  String type; // "light" or "servo"
  bool state;  // Light: ON/OFF | Servo: LOCKED(false)/OPEN(true)
  int value;   // Light: Brightness | Servo: Angle (optional)
  String color;
  unsigned long timer_end_at; 
};

DeviceState dev_led1  = {ID_LED_STRIP_1, "light", false, 100, "#FFFFFF", 0};
DeviceState dev_led2  = {ID_LED_STRIP_2, "light", false, 100, "#FFFFFF", 0};
DeviceState dev_servo = {ID_SERVO,       "servo", false, 0,   "",        0};

bool pirState = false;
unsigned long pirLastTrigger = 0;
const int PIR_TIMEOUT = 5000; // Đèn local sáng 5s sau khi hết chuyển động


uint32_t hexToColor(Adafruit_NeoPixel &p, String hex) {
  if (hex.startsWith("#")) hex.remove(0, 1);
  long number = strtol(hex.c_str(), NULL, 16);
  return p.Color((number >> 16) & 0xFF, (number >> 8) & 0xFF, number & 0xFF);
}

void updateMainLeds() {
  uint32_t c1 = dev_led1.state ? hexToColor(pixelsMain, dev_led1.color) : 0;
  for(int i=0; i<8; i++) {
    if(dev_led1.state) {
      // Simple brightness scaling
      uint8_t r = (uint8_t)(c1 >> 16) * dev_led1.value / 100;
      uint8_t g = (uint8_t)(c1 >> 8)  * dev_led1.value / 100;
      uint8_t b = (uint8_t)c1         * dev_led1.value / 100;
      pixelsMain.setPixelColor(i, pixelsMain.Color(r,g,b));
    } else {
      pixelsMain.setPixelColor(i, 0);
    }
  }

  uint32_t c2 = dev_led2.state ? hexToColor(pixelsMain, dev_led2.color) : 0;
  for(int i=8; i<15; i++) {
     if(dev_led2.state) {
      uint8_t r = (uint8_t)(c2 >> 16) * dev_led2.value / 100;
      uint8_t g = (uint8_t)(c2 >> 8)  * dev_led2.value / 100;
      uint8_t b = (uint8_t)c2         * dev_led2.value / 100;
      pixelsMain.setPixelColor(i, pixelsMain.Color(r,g,b));
    } else {
      pixelsMain.setPixelColor(i, 0);
    }
  }
  pixelsMain.show();
}

void controlServoHW() {
  if (dev_servo.state) {
    myServo.write(90); // Mở
  } else {
    myServo.write(0);  // Đóng
  }
}

void publishStatus(DeviceState &dev) {
  StaticJsonDocument<256> doc;
  doc["device_id"] = dev.id;
  doc["type"] = dev.type;
  
  JsonObject params = doc.createNestedObject("params");
  
  if (dev.type == "servo") {
    params["state"] = dev.state ? "OPEN" : "LOCKED";
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
  snprintf(topicBuffer, sizeof(topicBuffer), "smart_home/controller-02/%s/status", dev.id.c_str());

  client.publish(topicBuffer, payloadBuffer, true);
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
  else if (receivedId == ID_SERVO) target = &dev_servo;

  if (target) {
    if (params.containsKey("state")) {
      const char* st = params["state"];
      if (target->type == "servo") {
         bool reqOpen = (strcmp(st, "OPEN") == 0);
         target->state = reqOpen;
      } else {
         target->state = (strcmp(st, "ON") == 0);
      }
    }

    if (target->type == "light") {
      if (params.containsKey("brightness")) target->value = params["brightness"];
      if (params.containsKey("color")) target->color = params["color"].as<String>();
    }
    
    if (params.containsKey("timer_off")) {
       int minutes = params["timer_off"];
       target->timer_end_at = (minutes > 0) ? (millis() + minutes * 60000) : 0;
    }
    if (params.containsKey("auto_lock_delay")) {
       int seconds = params["auto_lock_delay"];
       target->timer_end_at = (seconds > 0) ? (millis() + seconds * 1000) : 0;
    }

    if (target->type == "light") updateMainLeds();
    else controlServoHW();
    
    publishStatus(*target);
  }
}

void handleRFID() {
  if (!mfrc522.PICC_IsNewCardPresent()) return;
  if (!mfrc522.PICC_ReadCardSerial()) return;

  Serial.print("RFID UID Tag :");
  String content= "";
  bool match = true;

  // Kiểm tra độ dài UID (thường là 4 byte)
  if (mfrc522.uid.size != sizeof(MASTER_KEY_UID)) {
     match = false;
  } else {
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      Serial.print(mfrc522.uid.uidByte[i] < 0x10 ? " 0" : " ");
      Serial.print(mfrc522.uid.uidByte[i], HEX);
      
      if (mfrc522.uid.uidByte[i] != MASTER_KEY_UID[i]) match = false;
    }
  }
  Serial.println();

  if (match) {
    Serial.println("Access Granted!");
    dev_servo.state = true;
    controlServoHW();
    
    dev_servo.timer_end_at = millis() + 5000; 
    
    publishStatus(dev_servo);
  } else {
    Serial.println("Access Denied!");
  }
  
  mfrc522.PICC_HaltA(); 
  mfrc522.PCD_StopCrypto1();
}

void handlePIR() {
  int pirVal = digitalRead(PIN_PIR);
  
  if (pirVal == HIGH) {
    if (!pirState) {
      Serial.println("Motion Detected!");
      pirState = true;
      
      for(int i=0; i<NUM_LED_LOCAL; i++) {
        pixelsLocal.setPixelColor(i, pixelsLocal.Color(50, 50, 0)); // Yellow dim
      }
      pixelsLocal.show();
    }
    pirLastTrigger = millis(); // Reset timeout
  } 
  
  if (pirState && (millis() - pirLastTrigger > PIR_TIMEOUT)) {
    Serial.println("Motion Ended");
    pirState = false;
    pixelsLocal.clear(); // Tắt hết
    pixelsLocal.show();
  }
}

void setup() {
  Serial.begin(115200);

  // Init LEDs
  pixelsMain.begin(); pixelsMain.show();
  pixelsLocal.begin(); pixelsLocal.show();

  // Init PIR
  pinMode(PIN_PIR, INPUT);

  // Init Servo
  myServo.attach(PIN_SERVO);
  myServo.write(0); // Mặc định đóng

  // Init SPI & RFID
  SPI.begin(); 
  mfrc522.PCD_Init(); 
  delay(4);
  mfrc522.PCD_DumpVersionToSerial(); // Debug info RFID module

  // WiFi Manager
  WiFiManager wm;
  if(!wm.autoConnect("SmartHome_Controller_02", "12345678")) {
    ESP.restart();
  } 

  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(callback);
}

void reconnect() {
  while (!client.connected()) {
    String clientId = "ESP32_Controller_02";
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      client.subscribe(topic_cmd);
      // Gửi status ban đầu
      publishStatus(dev_led1);
      publishStatus(dev_led2);
      publishStatus(dev_servo);
    } else {
      delay(5000);
    }
  }
}

void checkTimer(DeviceState &dev) {
  if (dev.state && dev.timer_end_at > 0) {
    if (millis() > dev.timer_end_at) {
      if (dev.type == "servo") {
        Serial.println("Auto Lock Servo");
        dev.state = false; // Close
        controlServoHW();
      } else {
        Serial.println("Timer Off Light");
        dev.state = false; // Off
        updateMainLeds();
      }
      dev.timer_end_at = 0;
      publishStatus(dev);
    }
  }
}

void loop() {
  if (!client.connected()) reconnect();
  client.loop();

  handleRFID();

  handlePIR();

  checkTimer(dev_led1);
  checkTimer(dev_led2);
  checkTimer(dev_servo);
}