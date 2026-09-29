#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <RTClib.h>
#include <FreeRTOSConfig.h>

#define SS_PIN 5
#define RST_PIN 22
#define BUZZER_PIN 21
#define RED_LED_PIN 2
#define GREEN_LED_PIN 15
#define PIR_PIN 35
#define SERVO_PIN 27
#define BUTTON_DAY 4
#define BUTTON_NIGHT 16
#define BUTTON_AUTO 17 
#define SDA_PIN 25
#define SCL_PIN 26

// Khai báo LCD, MFRC522, Servo
LiquidCrystal_I2C lcd(0x27, 16, 2);
MFRC522 rfid(SS_PIN, RST_PIN);
Servo myServo;
// User UIDs với mỗi mã số thẻ có 4byte
byte allowedUIDs[][4] = {
  {0x61, 0xBC, 0x8C, 0x02},  // Thẻ user 1
  {0x03, 0x81, 0xd8, 0x0A}   // Thẻ user 2
};
// MasterCard UID
byte masterUID[4] = {0x61, 0xBC, 0x8C, 0x02};

// Biến toàn cục
TaskHandle_t RFIDTaskHandle = NULL;
TaskHandle_t PIRTaskHandle = NULL;
TaskHandle_t RTCModeTaskHandle = NULL; 
bool isAutoMode = true;   // Biến bật Automode
bool isNightMode;  // Biến bật Nightmode
// Tạo biến góc đóng/mở của Servo
const int unlockAngle = 100;
const int lockAngle = 0;
// Biến thời gian mở cửa PIR
const int RFID_OPEN_TIME = 4000;
// Biến chờ thẻ Master
bool waitingForMaster = false;
// Biến cập nhật LCD
bool lcdNeedsUpdate = true;
// Biến cập nhật cho PIR
bool motionDetected = true;
unsigned long motionEndTime = 0;
// Khai báo module DS1307
RTC_DS1307 rtc;

void setup() {
  // Khởi tạo LCD tích hợp I2C và Serial
  Serial.begin(115200);
  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  // khởi tạo và kiểm tra khởi tạo RTC
  if (!rtc.begin()) {
    Serial.println("Không thể khởi tạo RTC!");
    while (1);
  }
  if (!rtc.isrunning()) {
    Serial.println("RTC chưa chạy, cài đặt lại giờ!");
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }
  // Khởi tạo Servo với trạng thái khóa  
  myServo.attach(SERVO_PIN);
  myServo.write(lockAngle);
  // Khởi tạo RFID và các nút bấm
  SPI.begin();
  rfid.PCD_Init();
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(PIR_PIN, INPUT);
  pinMode(BUTTON_DAY, INPUT_PULLUP);
  pinMode(BUTTON_NIGHT, INPUT_PULLUP);
  pinMode(BUTTON_AUTO, INPUT_PULLUP); 
  // Tạo các Task FreeRTOS
  xTaskCreate(TaskPIR, "PIR Task", 4096, NULL, 1, &PIRTaskHandle);
  xTaskCreate(TaskRFID, "RFID Task", 4096, NULL, 1, &RFIDTaskHandle);
  xTaskCreate(TaskModeSwitch, "Mode Switch", 4096, NULL, 2, NULL);
  xTaskCreate(TaskLCDDisplay, "LCD Display Task", 2048, NULL, 1, NULL);
  xTaskCreate(TaskRTCModeSwitch, "RTC Mode Switch Task", 2048, NULL, 1, &RTCModeTaskHandle); 
}
void loop() {
  // hàm loop trống
}

// Task cho AutoMode (dựa trên thời gian của RTC)
void TaskRTCModeSwitch(void *parameters) {
  while (1) {
    if (isAutoMode) {
      TimeSwitch(); // Gọi hàm TimeSwitch()
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}
// Hàm thay đổi Day/Night theo thời gian
void TimeSwitch() {
  DateTime now = rtc.now();
  int hour = now.hour();
  int minute = now.minute();
  if (hour == 2 && minute >= 2 && minute <= 5) {  // Điều kiện thời gian Daymode
    isNightMode = false;
    Serial.println("Chế độ AutoMode đang là Daymode.");
  } else { // Khoảng thời gian còn lại là của Nightmode
    isNightMode = true;
    Serial.println("Chế độ AutoMode đang là Nightmode.");
  }
}

// Task PIR cho daymode
void TaskPIR(void *parameters) {
  motionDetected = false;
  motionEndTime = 0;
  while (1) {
    if ((!isNightMode && !isAutoMode) || (!isNightMode && isAutoMode)) {
      vTaskDelay(pdMS_TO_TICKS(1000));
      if (digitalRead(PIR_PIN) == HIGH) {
        if (motionDetected) {
          Serial.println("Phát hiện chuyển động! Mở cửa...");
          openDoorPIR();
          motionDetected = false;
        }
        // Cập nhật thời gian khi phát hiện chuyển động
        motionEndTime = millis();
      } else if (!motionDetected && (millis() - motionEndTime > 1000)) {
        // Đóng cửa sau khi không có chuyển động trong 1 giây
        Serial.println("Không có chuyển động! Đóng cửa...");
        closeDoor();
        motionDetected = true;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Task cho NightMode (RFID)
void TaskRFID(void *parameters) {
  while (1) {
    if ((isNightMode && !isAutoMode) || (isNightMode && isAutoMode)) {
      myServo.write(lockAngle);
      digitalWrite(GREEN_LED_PIN, LOW);
      vTaskDelay(pdMS_TO_TICKS(1000));
      if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
        if (isCardAllowed()) {
          openDoorRFID();
          vTaskDelay(pdMS_TO_TICKS(RFID_OPEN_TIME));
          closeDoor();
        } else {
          signalInvalidCard();
        }
        rfid.PICC_HaltA();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

// Biến kiểm tra thẻ hợp lệ trong danh sách
bool isCardAllowed() {
  for (int j = 0; j < sizeof(allowedUIDs) / sizeof(allowedUIDs[0]); j++) {
    bool match = true;
    for (byte i = 0; i < 4; i++) {
      if (rfid.uid.uidByte[i] != allowedUIDs[j][i]) {
        match = false;
        break;
      }
    }
    if (match) {
      return true;
    }
  }
  return false;
}

// Hàm này được sử dụng để cảnh báo nếu có thẻ sai bằng các thiết bị
void signalInvalidCard() {
  digitalWrite(RED_LED_PIN, HIGH);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(3000);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
}

// Hàm mở cửa của PIR
void openDoorPIR() {
  digitalWrite(GREEN_LED_PIN, HIGH);
  myServo.write(unlockAngle);
  digitalWrite(BUZZER_PIN, HIGH);
  delay(1000);
  digitalWrite(BUZZER_PIN, LOW);
}

// Hàm mở cửa của RFID
void openDoorRFID() {
  digitalWrite(BUZZER_PIN, HIGH);
  myServo.write(unlockAngle);
  for (int i = 0; i < 3; i++) {
    digitalWrite(GREEN_LED_PIN, HIGH);
    delay(200);
    digitalWrite(GREEN_LED_PIN, LOW);
    delay(200);
  }
  delay(1000);
  digitalWrite(BUZZER_PIN, LOW);
}

// Hàm đóng cửa dùng chung
void closeDoor() {
  myServo.write(lockAngle);
  digitalWrite(GREEN_LED_PIN, LOW);
  for (int i = 0; i < 3; i++) {
    digitalWrite(RED_LED_PIN, HIGH);
    delay(200);
    digitalWrite(RED_LED_PIN, LOW);
    delay(200);
  }
}

// tạo biến yêu cầu quyền Master
bool YeuCauMasterCard() {
  // Tạm ngừng các task và các thiết bị
  myServo.write(lockAngle);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);
  vTaskSuspend(PIRTaskHandle);
  vTaskSuspend(RFIDTaskHandle);
  vTaskSuspend(RTCModeTaskHandle);
  waitingForMaster = true;  // Kích hoạt trạng thái chờ master
  lcdNeedsUpdate = true;    // Yêu cầu cập nhật LCD
  Serial.println("Yêu cầu quét thẻ Master");
  if (waitForMasterCard()) {
    Serial.println("Cấp quyền Master thành công!");
    motionDetected = true;
    motionEndTime = 0;
    waitingForMaster = false;  // Tắt trạng thái chờ master
    lcdNeedsUpdate = true;    // Yêu cầu cập nhật LCD
    vTaskResume(PIRTaskHandle);
    vTaskResume(RFIDTaskHandle);
    vTaskResume(RTCModeTaskHandle);
    return true;
  } else {
    Serial.println("Thẻ Master không hợp lệ");
    waitingForMaster = false;  // Tắt trạng thái chờ master
    lcdNeedsUpdate = true;    // Yêu cầu cập nhật LCD
    delay(1500);
    // Tiếp tục các task sau khi quét thẻ Master thất bại
    vTaskResume(PIRTaskHandle);
    motionDetected = true;
    motionEndTime = 0;
    vTaskResume(RFIDTaskHandle);
    vTaskResume(RTCModeTaskHandle);
    return false;
  }
}

// Biến chờ thẻ Master, nếu quá 10s (10000 Đơn vị TG) thì sẽ timeout về lại trạng thái hiện tại
bool waitForMasterCard() {
  unsigned long startTime = millis();
  const unsigned long timeout = 10000;  // 10 giây
  while (millis() - startTime < timeout) {
    if (rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial()) {
      if (isMasterCard()) {
        waitingForMaster = false;  // Tắt trạng thái chờ master
        return true;
      } else {
        waitingForMaster = false;  // Tắt trạng thái chờ master
        return false;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
  waitingForMaster = false;  // Hết thời gian chờ, tắt trạng thái chờ master
  Serial.println("Hết thời gian quét thẻ Master!");
  Serial.println("Quay về chế độ hiện tại");
  return false;
}

// Tạo biến để kiểm tra thẻ Master
bool isMasterCard() {
  for (byte i = 0; i < 4; i++) { //so sánh từ Byte0 đến Byte3
    if (rfid.uid.uidByte[i] != masterUID[i]) {
      return false;
    }
  }
  return true;
}

// Task for switching modes with master card authentication
void TaskModeSwitch(void *parameters) {
  while (1) {
    // Kiểm tra và xử lý nút DayMode
    if (digitalRead(BUTTON_DAY) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(25));  // Debounce 25ms
      if (digitalRead(BUTTON_DAY) == LOW) {  // Kiểm tra lại trạng thái
        if (YeuCauMasterCard()) {
          if (!isAutoMode && !isNightMode) {
            Serial.println("Hệ thống đã ở chế độ Daymode thủ công.");
          } else {
            isNightMode = false;
            isAutoMode = false;
            lcdNeedsUpdate = true;
            Serial.println("Chuyển sang chế độ Daymode thủ công.");
          }
        }
        while (digitalRead(BUTTON_DAY) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));  // Chờ nút được nhả
        }
      }
    }
    // Kiểm tra và xử lý nút NightMode
    if (digitalRead(BUTTON_NIGHT) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(50));  // Debounce 50ms
      if (digitalRead(BUTTON_NIGHT) == LOW) {  // Kiểm tra lại trạng thái
        if (YeuCauMasterCard()) {
          if (!isAutoMode && isNightMode) {
            Serial.println("Hệ thống đã ở chế độ Nightmode thủ công.");
          } else {
            isNightMode = true;
            isAutoMode = false;
            lcdNeedsUpdate = true;
            Serial.println("Chuyển sang chế độ Nightmode thủ công.");
          }
        }
        while (digitalRead(BUTTON_NIGHT) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));  // Chờ nút được nhả
        }
      }
    }
    // Kiểm tra và xử lý nút AutoMode
    if (digitalRead(BUTTON_AUTO) == LOW) {
      vTaskDelay(pdMS_TO_TICKS(50));  // Debounce 50ms
      if (digitalRead(BUTTON_AUTO) == LOW) {  // Kiểm tra lại trạng thái
        if (YeuCauMasterCard()) {
          if (isAutoMode) {
            Serial.println("Hệ thống đã ở chế độ AutoMode.");
          } else {
            isAutoMode = true;
            lcdNeedsUpdate = true;
            TimeSwitch();
          }
        }
        while (digitalRead(BUTTON_AUTO) == LOW) {
          vTaskDelay(pdMS_TO_TICKS(10));  // Chờ nút được nhả
        }
      }
    }

    vTaskDelay(pdMS_TO_TICKS(10));  // Giảm tải CPU
  }
}

// Task for LCD display
void TaskLCDDisplay(void *parameters) {
  // Biến lưu trạng thái hiển thị trước đó để tránh cập nhật lại không cần thiết
  String previousModeDisplay = "";
  String previousTimeDisplay = "";
  bool isDisplayingMasterRequest = false;

  while (1) {
    // Lấy thời gian hiện tại từ RTC
    DateTime now = rtc.now();
    char timeBuffer[16];
    sprintf(timeBuffer, "Time: %02d-%02d-%02d", now.hour(), now.minute(), now.second());
    String currentTimeDisplay = String(timeBuffer);
    // Kiểm tra trạng thái hiển thị Master yêu cầu
    if (waitingForMaster) {
      if (!isDisplayingMasterRequest) {
        lcd.setCursor(0, 0);
        lcd.print("  Master pls? ");
        isDisplayingMasterRequest = true;  // Đánh dấu đang hiển thị yêu cầu master
      }
    } else {
      if (isDisplayingMasterRequest) {
        isDisplayingMasterRequest = false;  // Quay về hiển thị chế độ
        lcdNeedsUpdate = true;  // Yêu cầu cập nhật chế độ sau khi hoàn tất quét master
      }
    } 

    // Hiển thị mode nếu không phải ở trạng thái yêu cầu master
    if (!isDisplayingMasterRequest) {
      String currentModeDisplay;
      if (isAutoMode) {
        currentModeDisplay = "  AutoMode :v  ";
      } else if (isNightMode) {
        currentModeDisplay = "  NightMode ^^ ";
      } else {
        currentModeDisplay = "  DayMode :)   ";
      }
      // Cập nhật dòng 0
      if (lcdNeedsUpdate || currentModeDisplay != previousModeDisplay) {
        lcd.setCursor(0, 0);
        lcd.print(currentModeDisplay);
        previousModeDisplay = currentModeDisplay;
        lcdNeedsUpdate = false;  // Reset yêu cầu cập nhật
      }
    } 
    // Cập nhật dòng 1 (thời gian thực)
    if (currentTimeDisplay != previousTimeDisplay) {
      lcd.setCursor(0, 1);
      lcd.print(currentTimeDisplay);
      previousTimeDisplay = currentTimeDisplay;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}
