# Smart Home IoT – ESP32 & MQTT

Hệ thống **Smart Home IoT** sử dụng **ESP32** và giao thức **MQTT** cho phép **điều khiển và giám sát thiết bị nhà thông minh theo thời gian thực** thông qua Web/Mobile App.

Dự án được thực hiện trong khuôn khổ học phần **IoT và Ứng dụng**.

---

## Chức năng chính

- Điều khiển **đèn LED RGB** (ON/OFF, độ sáng, màu sắc)
- **Khóa cửa thông minh** bằng RFID + Servo
- Phát hiện chuyển động (PIR)
- Giám sát **nhiệt độ – độ ẩm – khí gas**
- Điều khiển **quạt DC nhiều mức**
- Hẹn giờ tắt / auto-lock
- Giao tiếp thời gian thực qua **MQTT**

---

## Thành phần hệ thống

### living_room_controller.ino – Môi trường & thông gió
- ESP32
- LED RGB (NeoPixel)
- Quạt DC (PWM)
- Cảm biến SHT31 (Nhiệt độ, độ ẩm)
- Cảm biến MQ-2 (Khí gas)
- Gửi dữ liệu telemetry định kỳ

### 🔹 kitchen_controller.ino – An ninh & chiếu sáng
- ESP32
- LED RGB (NeoPixel)
- Servo khóa cửa
- RFID MFRC522
- Cảm biến PIR
- Điều khiển & phản hồi trạng thái thiết bị

---

## Cài đặt nhanh

### 1. Yêu cầu
- Arduino IDE
- ESP32 Board Package
- MQTT Broker (Mosquitto / EMQX / Cloud)
- Thư viện:
- WiFi
- PubSubClient
- ArduinoJson
- Adafruit NeoPixel
- WiFiManager
- MFRC522
- ESP32Servo
- Adafruit SHT31

### 2️. Nạp chương trình
- Chọn **Board: ESP32 Dev Module**
- Nạp code tương ứng cho từng controller

### 3. Cấu hình WiFi
ESP32 tự tạo WiFi cấu hình:
SSID: SmartHome_Config / SmartHome_Controller_02
Password: 12345678