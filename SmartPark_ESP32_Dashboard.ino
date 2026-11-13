#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>

// Put your Wi-Fi details here (same network as the laptop).
const char* WIFI_SSID = "Infinix GT 30";
const char* WIFI_PASSWORD = "77777777";

// Laptop IPv4 address supplied in chat.
const char* SERVER_BASE = "http://172.31.70.98:5000";

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDRESS 0x3C

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
Servo barrierServo;

// Existing project pin mapping — do not change your wiring.
const int SLOT_SENSOR[4] = {23, 5, 16, 17}; // Active LOW = occupied
const int GREEN_LED[4]   = {25, 27, 32, 4};
const int RED_LED[4]     = {26, 14, 33, 13};
const int SERVO_PIN = 12;
const int ENTRY_SENSOR_PIN = 18; // Active LOW: vehicle at entry
const int EXIT_SENSOR_PIN = 19;  // Active LOW: vehicle at exit

const int CLOSED_ANGLE = 0;
const int OPEN_ANGLE = 90;

bool occupied[4] = {false, false, false, false};
bool booked[4] = {false, false, false, false};
String gateState = "CLOSED";

unsigned long lastPost = 0;
unsigned long lastGateDetection = 0;
bool automaticGateActive = false;
unsigned long lastPoll = 0;
const unsigned long INTERVAL_MS = 1000;

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Wi-Fi connected");
    Serial.print("ESP32 IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Wi-Fi not connected yet; retrying in loop.");
  }
}

void updateLeds() {
  for (int i = 0; i < 4; i++) {
    bool unavailable = occupied[i] || booked[i];
    digitalWrite(GREEN_LED[i], unavailable ? LOW : HIGH);
    digitalWrite(RED_LED[i], unavailable ? HIGH : LOW);
  }
}

void updateOled() {
  int freeCount = 0;
  for (int i = 0; i < 4; i++) {
    if (!occupied[i] && !booked[i]) freeCount++;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Header
  display.setCursor(0, 0);
  display.println("SMART PARKING");

  // Two columns: S1/S2 on first row, S3/S4 on second row.
  display.setCursor(0, 16);
  display.print("S1:");
  if (occupied[0]) display.print("OCC");
  else if (booked[0]) display.print("BOOK");
  else display.print("FREE");

  display.setCursor(66, 16);
  display.print("S2:");
  if (occupied[1]) display.print("OCC");
  else if (booked[1]) display.print("BOOK");
  else display.print("FREE");

  display.setCursor(0, 29);
  display.print("S3:");
  if (occupied[2]) display.print("OCC");
  else if (booked[2]) display.print("BOOK");
  else display.print("FREE");

  display.setCursor(66, 29);
  display.print("S4:");
  if (occupied[3]) display.print("OCC");
  else if (booked[3]) display.print("BOOK");
  else display.print("FREE");

  // Dedicated bottom rows prevent overlap with slot labels.
  display.drawLine(0, 43, 127, 43, SSD1306_WHITE);

  display.setCursor(0, 49);
  display.print("FREE: ");
  display.print(freeCount);
  display.print("/4");

  display.setCursor(66, 49);
  display.print("GATE:");
  display.print(gateState == "OPEN" ? "OPEN" : "CLOSED");

  display.display();
}
void postSlotStatus() {
  if (WiFi.status() != WL_CONNECTED) return;

  String body = "{\"slots\":[";
  for (int i = 0; i < 4; i++) {
    if (i) body += ",";
    body += "{\"id\":";
    body += String(i + 1);
    body += ",\"occupied\":";
    body += occupied[i] ? "true" : "false";
    body += "}";
  }
  body += "],\"gate\":\"";
  body += gateState;
  body += "\"}";

  HTTPClient http;
  http.setTimeout(1200);
  http.begin(String(SERVER_BASE) + "/api/esp32/update");
  http.addHeader("Content-Type", "application/json");
  int code = http.POST(body);
  Serial.printf("POST /api/esp32/update -> %d\n", code);
  http.end();
}

bool parseBookings(const String& json) {
  int key = json.indexOf("\"booked\"");
  if (key < 0) return false;
  int left = json.indexOf('[', key);
  int right = json.indexOf(']', left);
  if (left < 0 || right < 0) return false;

  String values = json.substring(left + 1, right);
  int cursor = 0;
  for (int i = 0; i < 4; i++) {
    int comma = values.indexOf(',', cursor);
    String token = (comma < 0) ? values.substring(cursor) : values.substring(cursor, comma);
    token.trim();
    if (token == "true") booked[i] = true;
    else if (token == "false") booked[i] = false;
    else return false;
    cursor = (comma < 0) ? values.length() : comma + 1;
  }
  return true;
}

void pollDashboard() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setTimeout(1200);
  http.begin(String(SERVER_BASE) + "/api/esp32/commands");
  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    String json = http.getString();
    parseBookings(json);

    int key = json.indexOf("\"gate\"");
    if (key >= 0) {
      int colon = json.indexOf(':', key);
      int q1 = json.indexOf('"', colon + 1);
      int q2 = json.indexOf('"', q1 + 1);
      if (q1 >= 0 && q2 > q1) {
        String requested = json.substring(q1 + 1, q2);
        if (!automaticGateActive && requested == "OPEN" && gateState != "OPEN") {
          barrierServo.write(OPEN_ANGLE);
          gateState = "OPEN";
          Serial.println("Gate opened by dashboard");
        } else if (!automaticGateActive && requested == "CLOSED" && gateState != "CLOSED") {
          barrierServo.write(CLOSED_ANGLE);
          gateState = "CLOSED";
          Serial.println("Gate closed by dashboard");
        }
      }
    }
  } else {
    Serial.printf("GET /api/esp32/commands -> %d\n", code);
  }
  http.end();
}

void setup() {
  Serial.begin(115200);

  pinMode(ENTRY_SENSOR_PIN, INPUT_PULLUP);
  pinMode(EXIT_SENSOR_PIN, INPUT_PULLUP);
  for (int i = 0; i < 4; i++) {
    pinMode(SLOT_SENSOR[i], INPUT_PULLUP);
    pinMode(GREEN_LED[i], OUTPUT);
    pinMode(RED_LED[i], OUTPUT);
  }

  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDRESS)) {
    Serial.println("OLED initialization failed; check wiring.");
  }

  barrierServo.setPeriodHertz(50);
  barrierServo.attach(SERVO_PIN, 500, 2500);
  barrierServo.write(CLOSED_ANGLE);

  updateLeds();
  connectWiFi();
  updateOled();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    static unsigned long lastReconnect = 0;
    if (millis() - lastReconnect > 10000) {
      lastReconnect = millis();
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  for (int i = 0; i < 4; i++) {
    occupied[i] = (digitalRead(SLOT_SENSOR[i]) == LOW);
  }

  // Automatic barrier: either entry or exit sensor opens the arm.
  // It closes 1.5 seconds after both sensors become clear.
  bool vehicleAtEntry = (digitalRead(ENTRY_SENSOR_PIN) == LOW);
  bool vehicleAtExit = (digitalRead(EXIT_SENSOR_PIN) == LOW);
  if (vehicleAtEntry || vehicleAtExit) {
    lastGateDetection = millis();
    automaticGateActive = true;
    if (gateState != "OPEN") {
      barrierServo.write(OPEN_ANGLE);
      gateState = "OPEN";
      Serial.println("Gate automatically opened by entry/exit sensor");
    }
  } else if (automaticGateActive && millis() - lastGateDetection > 1500) {
    automaticGateActive = false;
    barrierServo.write(CLOSED_ANGLE);
    gateState = "CLOSED";
    Serial.println("Gate automatically closed after vehicle passed");
  }

  updateLeds();
  updateOled();

  if (millis() - lastPost >= INTERVAL_MS) {
    lastPost = millis();
    postSlotStatus();
  }

  if (millis() - lastPoll >= INTERVAL_MS) {
    lastPoll = millis();
    pollDashboard();
    updateLeds();
    updateOled();
  }

  delay(50);
}
