#include <Wire.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_PN532.h>
#include <ESP32Servo.h>
#include "AESLib.h"
#include <WiFi.h>
#include <WebServer.h>

// --- WIFI CREDENTIALS ---
const char* ssid = "Charith DELL";
const char* password = "Charith@2842$";

// --- PIN MAPPING ---
#define OLED_SDA 21
#define OLED_SCL 22
#define PN532_SCK 18
#define PN532_MISO 19
#define PN532_MOSI 23
#define PN532_SS 5
#define SERVO_PIN 13
#define LED_RED 2
#define LED_GREEN 4
#define LED_BLUE 12
#define LED_YELLOW 14

Adafruit_SSD1306 display(128, 64, &Wire, -1);
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
Servo seatServo;
AESLib aes;
WebServer server(80);

// --- UNIQUE AES KEYS ---
byte key1[] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
byte key2[] = {0xA1, 0xB2, 0xC3, 0xD4, 0xE5, 0xF6, 0xA7, 0xB8, 0xC9, 0xD0, 0xE1, 0xF2, 0xA3, 0xB4, 0xC5, 0xD6};
byte key3[] = {0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA, 0x99, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00};
byte keyGuest[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99};
byte aes_iv[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

// --- HARDWARE STORAGE ---
uint8_t mifareKey[] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
const int DATA_BLOCK = 4;
uint8_t writeBuffer[16];

// --- USER UIDs ---
uint8_t user1_uid[] = {0x8F, 0x58, 0x49, 0x1F};
uint8_t user2_uid[] = {0xBF, 0xB5, 0x8E, 0x1F};
uint8_t user3_uid[] = {0xD3, 0x43, 0x3D, 0x15};
uint8_t guest_uid[4] = {0,0,0,0};

// --- RUNTIME STATE ---
String systemStatus = "ARMED & SECURE";
String lastUser = "NONE";
int activeSessionID = 0;
int previousUser = 0;
bool settingsSaved = false;
bool writePending = false;
bool guestEnrolled = false;
unsigned long sessionStartTime = 0;
const int SESSION_DURATION = 30000;

int currentAngle = 90;
String currentMusic = "Jazz";
String currentMode = "Sports";

const char* musicGenres[] = {"Melody", "Cool", "Pop", "Jazz", "Rock"};
const char* driveModes[] = {"Power", "Eco", "Sports", "Comfort"};

uint8_t getID(const char* arr[], int size, String val) {
  for(int i=0; i<size; i++) if(String(arr[i]) == val) return i;
  return 0;
}

void updateOLED() {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.setCursor(0,0);
  display.println("AURA B-M-V");
  display.drawFastHLine(0, 10, 128, WHITE);

  if (activeSessionID == 0 && previousUser != 0) {
    display.setCursor(0, 18); display.print("LAST USER: "); display.println(previousUser == 4 ? "GUEST" : String(previousUser));
    display.setCursor(0, 30); display.print("MUSIC: "); display.println(currentMusic);
    display.setCursor(0, 42); display.print("MODE:  "); display.println(currentMode);
    display.setCursor(0, 54); display.println("SYSTEM: LOCKED");
  } else if (writePending) {
    display.setCursor(0, 25); display.println("PENDING SYNC...");
    display.setCursor(0, 40); display.println("TAP CARD TO COMMIT");
  } else {
    display.setCursor(0, 18); display.print("STATUS: "); display.println(systemStatus);
    display.setCursor(0, 30); display.print("USER:   "); display.println(lastUser);
    display.setCursor(0, 42); display.print("WEB:    "); display.println(activeSessionID == 0 ? "LOCKED" : "ENABLED");
  }
  display.display();
}

void handleStatus() {
  server.sendHeader("Connection", "close");
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", "{\"session\":" + String(activeSessionID) + "}");
}

void handleDeregister() {
  if (activeSessionID == 4) {
    guestEnrolled = false;
    memset(guest_uid, 0, 4);
    display.clearDisplay(); display.setCursor(0, 25); display.println("GUEST DE-REGISTERED"); display.display();
    delay(1000);
    resetToArmed();
  }
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleRoot() {
  if (server.method() == HTTP_POST && activeSessionID != 0) {
    sessionStartTime = millis();
    currentMusic = server.arg("m");
    currentMode = server.arg("d");
    currentAngle = constrain(server.arg("a").toInt(), 0, 180);

    memset(writeBuffer, 0, 16);
    writeBuffer[0] = (uint8_t)currentAngle;
    writeBuffer[1] = getID(musicGenres, 5, currentMusic);
    writeBuffer[2] = getID(driveModes, 4, currentMode);

    writePending = true;
    settingsSaved = false;
    updateOLED();
  }

  // Optimized HTML with "Cache-Control" to ensure no delay upon unlock
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Expires", "-1");

  String h = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><script>";
  h += "let curr = " + String(activeSessionID) + ";";
  h += "function checkStatus(){ fetch('/status').then(r=>r.json()).then(d=>{ if(d.session != curr) location.replace('/'); }).catch(e=>console.log(e)); }";
  h += "setInterval(checkStatus, 150);"; // Aggressive polling
  h += "</script><style>";
  h += "body{font-family:sans-serif; background:#121212; color:white; text-align:center; padding-top:20px;}";
  h += ".card{border:2px solid #007bff; padding:20px; border-radius:15px; display:inline-block; background:#1e1e1e;}";
  h += "input, select{padding:8px; margin:5px; border-radius:5px;} .btn{background:#28a745; color:white; border:none; padding:10px 20px; cursor:pointer;}";
  h += ".btn-del{background:#dc3545; margin-top:10px;}";
  h += "</style></head><body><div class='card'>";

  if (activeSessionID == 0) {
    h += "<h2>SYSTEM LOCKED</h2><p>Awaiting Identity Verification...</p>";
  } else {
    h += "<h2>" + String(activeSessionID == 4 ? "GUEST" : "USER " + String(activeSessionID)) + "</h2><hr>";
    h += "<form action='/' method='POST'>";
    h += "Music: <select name='m'>";
    for(int i=0; i<5; i++) h += "<option value='"+String(musicGenres[i])+"'"+(currentMusic==musicGenres[i]?" selected":"")+">"+String(musicGenres[i])+"</option>";
    h += "</select><br>Mode: <select name='d'>";
    for(int i=0; i<4; i++) h += "<option value='"+String(driveModes[i])+"'"+(currentMode==driveModes[i]?" selected":"")+">"+String(driveModes[i])+"</option>";
    h += "<br>Angle: <input type='number' name='a' min='0' max='180' step='5' value='"+String(currentAngle)+"' style='width:60px;'><br><br>";
    h += "<input type='submit' class='btn' value='ENCRYPT & COMMIT'></form>";
    if (activeSessionID == 4) h += "<br><a href='/deregister'><button class='btn btn-del'>DEREGISTER</button></a>";
  }
  h += "</div></body></html>";
  server.send(200, "text/html", h);
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_RED, OUTPUT); pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_BLUE, OUTPUT); pinMode(LED_YELLOW, OUTPUT);
  seatServo.attach(SERVO_PIN);
  seatServo.write(0);
  Wire.begin(OLED_SDA, OLED_SCL);
  display.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  nfc.begin(); nfc.SAMConfig();

  WiFi.setSleep(false); // Maximize WiFi speed
  WiFi.begin(ssid, password);

  server.on("/", handleRoot);
  server.on("/status", handleStatus);
  server.on("/deregister", handleDeregister);
  server.begin();
  resetToArmed();
}

void loop() {
  server.handleClient();

  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;

  if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 20)) {
    unsigned long currentTime = millis();
    int userFound = 0;
    byte* selectedAESKey;

    if (memcmp(uid, user1_uid, 4) == 0) { userFound = 1; selectedAESKey = key1; }
    else if (memcmp(uid, user2_uid, 4) == 0) { userFound = 2; selectedAESKey = key2; }
    else if (memcmp(uid, user3_uid, 4) == 0) { userFound = 3; selectedAESKey = key3; }
    else if (guestEnrolled && memcmp(uid, guest_uid, 4) == 0) { userFound = 4; selectedAESKey = keyGuest; }

    if (userFound > 0) {
      if (runAES(selectedAESKey)) {
        if (nfc.mifareclassic_AuthenticateBlock(uid, uidLength, DATA_BLOCK, 0, mifareKey)) {
          if (activeSessionID == userFound) {
            if (writePending) {
              if (nfc.mifareclassic_WriteDataBlock(DATA_BLOCK, writeBuffer)) {
                writePending = false; settingsSaved = true; systemStatus = "SYNC SUCCESS";
              }
            } else {
              previousUser = activeSessionID;
              display.clearDisplay(); display.setTextSize(2); display.setCursor(10, 20); display.println("SAVED!"); display.display();
              delay(1000); resetToArmed();
              return;
            }
          } else {
            uint8_t data[16];
            if (nfc.mifareclassic_ReadDataBlock(DATA_BLOCK, data)) {
              currentAngle = data[0]; currentMusic = musicGenres[data[1]]; currentMode = driveModes[data[2]];
              if (activeSessionID == 0 && lastUser == (userFound == 4 ? "GUEST" : "USER " + String(userFound)) && (currentTime - sessionStartTime < 4000)) {
                  activeSessionID = userFound; systemStatus = "WEB ENABLED"; digitalWrite(LED_RED, LOW);
                  sessionStartTime = currentTime; // Ensure session starts fresh
              } else {
                  lastUser = (userFound == 4 ? "GUEST" : "USER " + String(userFound));
                  sessionStartTime = currentTime; systemStatus = "AUTH VERIFIED"; applyPhysicalState(userFound, currentAngle);
              }
            }
          }
        }
      }
    } else {
      if (lastUser == "UNKNOWN" && (currentTime - sessionStartTime < 4000)) {
          memcpy(guest_uid, uid, 4); guestEnrolled = true; activeSessionID = 4; sessionStartTime = currentTime;
          currentAngle = 90; currentMusic = "Melody"; currentMode = "Eco"; systemStatus = "GUEST ENROLLED"; digitalWrite(LED_RED, LOW);
      } else {
          lastUser = "UNKNOWN"; sessionStartTime = currentTime; systemStatus = "NEW CARD? TAP 2x";
      }
    }
    updateOLED();
  }

  if (activeSessionID != 0 && (millis() - sessionStartTime > SESSION_DURATION)) resetToArmed();
}

void applyPhysicalState(int user, int angle) {
  digitalWrite(LED_GREEN, user == 1 || user == 4 ? HIGH : LOW);
  digitalWrite(LED_BLUE, user == 2 || user == 4 ? HIGH : LOW);
  digitalWrite(LED_YELLOW, user == 3 ? HIGH : LOW);
  if (activeSessionID == 0) digitalWrite(LED_RED, HIGH); else digitalWrite(LED_RED, LOW);
  seatServo.write(constrain(angle, 0, 180));
}

void resetToArmed() {
  activeSessionID = 0; settingsSaved = false; writePending = false;
  systemStatus = "ARMED & SECURE"; lastUser = "NONE";
  digitalWrite(LED_RED, HIGH); digitalWrite(LED_GREEN, LOW); digitalWrite(LED_BLUE, LOW); digitalWrite(LED_YELLOW, LOW);
  updateOLED();
}

void denyAccess() {
  systemStatus = "DENIED"; updateOLED();
  for(int i=0; i<3; i++) { digitalWrite(LED_RED, LOW); delay(150); digitalWrite(LED_RED, HIGH); delay(150); }
  resetToArmed();
}

bool runAES(byte* key) {
  byte data[16] = "ECU_SECURE_AUTH";
  byte cipher[16], check[16];
  aes.encrypt(data, 16, cipher, key, 128, aes_iv);
  aes.decrypt(cipher, 16, check, key, 128, aes_iv);
  return true;
}
