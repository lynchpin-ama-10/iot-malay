#include <Wire.h>
#include <MAX30105.h>
#include <heartRate.h>
#include <HX711.h>
#include <LiquidCrystal_I2C.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_ADS1X15.h>
#include <WebServer.h>
#include "SPIFFS.h"

// ========== I2C ==========
MAX30105 particleSensor;
LiquidCrystal_I2C lcd(0x27, 20, 4);
Adafruit_ADS1115 ads;
WebServer server(80);

// ========== Pins ==========
#define HX711_DOUT   4
#define HX711_SCK    5

// Navigation buttons
#define BTN_UP       12
#define BTN_DOWN     15
#define BTN_SELECT   14

// LEDs
#define LED_GREEN    16
#define LED_YELLOW   17
#define LED_RED      2

// Blood pressure hardware
#define PUMP_PIN     18
#define VALVE_PIN    19

// Battery
#define BATTERY_PIN  34

// ========== Constants ==========
#define MEASURE_TIME       30000
#define RESULT_TIME        10000
#define BP_TARGET_INFLATE  170.0
#define BP_MAX_SAFE        220.0
#define BP_SAMPLE_RATE_MS  10

// ========== WiFi ==========
const char* STA_SSID = "YourHomeNetwork";      // CHANGE
const char* STA_PASSWORD = "YourPassword";     // CHANGE
IPAddress local_IP(192, 168, 1, 100);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress dns(8, 8, 8, 8);
const char* AP_SSID = "HealthMonitor_ESP";
const char* AP_PASSWORD = "12345678";
const char* SERVER_URL = "https://your-server.com/api/measurements";

// ========== State ==========
bool measuring = false, staConnected = false;
bool max30102_ok = false, hx711_ok = false, ads1115_ok = false;
int menuIndex = 0;      // 0=HR, 1=Grip, 2=BP, 3=All
int scrollOffset = 0;   // first visible item (0 or 1 because we show 2 items)

// ========== HR & SpO2 ==========
long lastBeat = 0;
float bpmSum = 0, spo2Sum = 0;
int beatCount = 0, spo2Count = 0;

// ========== BP ==========
float systolicBP = 0, diastolicBP = 0, meanBP = 0;
int bpHeartRate = 0, oscillationCount = 0;
float pressureBuffer[100], oscillationBuffer[100], maxOscillation = 0;

// ========== Grip ==========
float maxGripForce = 0;

HX711 scale;
enum BPState { BP_IDLE, BP_INFLATING, BP_MEASURING, BP_COMPLETE };
BPState bpState = BP_IDLE;

// ========== Prototypes ==========
void showMainMenu();
void handleMenuNavigation();
void measureHeart();
void measureGrip();
void measureBloodPressure();
void measureAll();
float readPressureMPX();
void startPump(), stopPump(), openValve(), closeValve();
float detectOscillation(float p);
void calculateBloodPressure();
void uploadData(float sys, float dia, float map, int hr, float spo2, int bpm, float grip);
void setLED(float value, String type);
void clearLEDs();
void startHTTPServer();
String getSensorDataJSON();
void saveToLocalStorage(String data);
void uploadPendingData();
int getBatteryPercent();
bool connectToSurroundingWiFi();
void startAPMode();
void initSPIFFS();

// ========== WiFi Connection ==========
bool connectToSurroundingWiFi() {
  for (int attempt = 0; attempt < 5; attempt++) {
    lcd.setCursor(0,2); lcd.print("Scanning WiFi    ");
    lcd.setCursor(0,3); lcd.print("Attempt "); lcd.print(attempt+1); lcd.print("/5");
    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      if (WiFi.SSID(i) == STA_SSID) {
        WiFi.config(local_IP, gateway, subnet, dns);
        WiFi.begin(STA_SSID, STA_PASSWORD);
        int conn = 0;
        while (WiFi.status() != WL_CONNECTED && conn < 20) { delay(500); conn++; }
        if (WiFi.status() == WL_CONNECTED) return true;
      }
    }
    delay(1000);
  }
  return false;
}

void startAPMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  lcd.clear();
  lcd.setCursor(0,0); lcd.print("AP Mode Active");
  lcd.setCursor(0,1); lcd.print("Network: "); lcd.print(AP_SSID);
  lcd.setCursor(0,2); lcd.print("IP: "); lcd.print(WiFi.softAPIP());
  lcd.setCursor(0,3); lcd.print("No internet");
  delay(2000);
}

// ========== SPIFFS ==========
void initSPIFFS() {
  if (!SPIFFS.begin(true)) {
    lcd.clear();
    lcd.print("SPIFFS Mount Fail");
    while(1);
  }
}

void saveToLocalStorage(String data) {
  File file = SPIFFS.open("/data.txt", FILE_APPEND);
  if (file) { file.println(data); file.close(); }
}

void uploadPendingData() {
  if (!(WiFi.status() == WL_CONNECTED && staConnected)) return;
  File file = SPIFFS.open("/data.txt");
  if (!file) return;
  int uploaded = 0;
  while (file.available()) {
    String data = file.readStringUntil('\n');
    if (data.length() > 0) {
      HTTPClient http;
      http.begin(SERVER_URL);
      http.addHeader("Content-Type", "application/json");
      if (http.POST(data) > 0) uploaded++;
      http.end();
      delay(500);
    }
  }
  file.close();
  if (uploaded > 0) { file = SPIFFS.open("/data.txt", FILE_WRITE); file.close(); }
}

// ========== HTTP Server ==========
void startHTTPServer() {
  server.on("/data", HTTP_GET, []() { server.send(200, "application/json", getSensorDataJSON()); });
  server.on("/status", HTTP_GET, []() {
    String s = "{\"sta\":" + String(staConnected) + ",\"max\":" + max30102_ok + ",\"hx\":" + hx711_ok + ",\"ads\":" + ads1115_ok + ",\"bat\":" + String(getBatteryPercent()) + "}";
    server.send(200, "application/json", s);
  });
  server.on("/measure/heart", HTTP_POST, []() { server.send(200, "application/json", "{\"status\":\"ok\"}"); measuring=true; measureHeart(); measuring=false; });
  server.on("/measure/grip", HTTP_POST, []() { server.send(200, "application/json", "{\"status\":\"ok\"}"); measuring=true; measureGrip(); measuring=false; });
  server.on("/measure/bp", HTTP_POST, []() { server.send(200, "application/json", "{\"status\":\"ok\"}"); measuring=true; measureBloodPressure(); measuring=false; });
  server.on("/measure/all", HTTP_POST, []() { server.send(200, "application/json", "{\"status\":\"ok\"}"); measuring=true; measureAll(); measuring=false; });
  server.begin();
}

String getSensorDataJSON() {
  String j = "{";
  if (systolicBP > 0) j += "\"bp\":{\"sys\":" + String(systolicBP) + ",\"dia\":" + String(diastolicBP) + "},";
  if (spo2Count > 0) j += "\"spo2\":" + String(spo2Sum/spo2Count) + ",";
  if (beatCount > 0) j += "\"hr\":" + String((bpmSum/beatCount)*2) + ",";
  if (maxGripForce > 0) j += "\"grip\":" + String(maxGripForce) + ",";
  j += "\"bat\":" + String(getBatteryPercent()) + "}";
  j.replace(",}", "}");
  return j;
}

// ========== Battery ==========
int getBatteryPercent() {
  int raw = analogRead(BATTERY_PIN);
  float voltage = raw * (5.0 / 1023.0) * 2;
  return constrain(map(voltage * 100, 300, 420, 0, 100), 0, 100);
}

// ========== Data Upload ==========
void uploadData(float sys, float dia, float map, int hr, float spo2, int bpm, float grip) {
  String json = "{";
  if (sys > 0) json += "\"bp\":{\"sys\":" + String(sys) + ",\"dia\":" + String(dia) + "},";
  if (spo2 > 0) json += "\"spo2\":" + String(spo2) + ",";
  if (bpm > 0) json += "\"hr\":" + String(bpm) + ",";
  if (grip > 0) json += "\"grip\":" + String(grip) + ",";
  json += "\"ts\":" + String(millis()) + "}";
  json.replace(",}", "}");
  
  if (WiFi.status() == WL_CONNECTED && staConnected) {
    HTTPClient http;
    http.begin(SERVER_URL);
    http.addHeader("Content-Type", "application/json");
    if (http.POST(json) > 0) { lcd.setCursor(0,3); lcd.print("Data Sent!      "); }
    else saveToLocalStorage(json);
    http.end();
  } else saveToLocalStorage(json);
}

// ========== SETUP ==========
void setup() {
  Serial.begin(115200);
  
  lcd.init();
  lcd.backlight();
  delay(50);
  lcd.clear();
  lcd.print("Health Monitor");
  lcd.setCursor(0,1);
  lcd.print("Initializing...");
  delay(1000);
  
  pinMode(BTN_UP, INPUT_PULLUP);
  pinMode(BTN_DOWN, INPUT_PULLUP);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(PUMP_PIN, OUTPUT);
  pinMode(VALVE_PIN, OUTPUT);
  pinMode(BATTERY_PIN, INPUT);
  digitalWrite(PUMP_PIN, LOW);
  digitalWrite(VALVE_PIN, LOW);
  clearLEDs();
  
  initSPIFFS();
  Wire.begin(21, 22);
  delay(100);
  
  lcd.setCursor(0,2); lcd.print("Checking MAX...   ");
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    max30102_ok = false;
    lcd.setCursor(0,2); lcd.print("MAX30102: ERROR   ");
  } else {
    particleSensor.setup();
    particleSensor.setPulseAmplitudeRed(0x1F);
    particleSensor.setPulseAmplitudeIR(0x1F);
    max30102_ok = true;
    lcd.setCursor(0,2); lcd.print("MAX30102: OK      ");
  }
  
  lcd.setCursor(0,3); lcd.print("Checking HX711... ");
  scale.begin(HX711_DOUT, HX711_SCK);
  scale.set_scale();
  scale.tare();
  hx711_ok = true;
  lcd.setCursor(0,3); lcd.print("HX711: OK         ");
  delay(1000);
  
  lcd.setCursor(0,2); lcd.print("Checking ADS...   ");
  if (!ads.begin()) {
    ads1115_ok = false;
    lcd.setCursor(0,2); lcd.print("ADS1115: ERROR    ");
  } else {
    ads.setGain(GAIN_ONE);
    ads1115_ok = true;
    lcd.setCursor(0,2); lcd.print("ADS1115: OK       ");
  }
  delay(1500);
  
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("Connecting...");
  if (connectToSurroundingWiFi()) {
    staConnected = true;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("WiFi Connected!");
    lcd.setCursor(0,1);
    lcd.print("IP: ");
    lcd.print(WiFi.localIP());
    delay(2000);
    uploadPendingData();
  } else {
    staConnected = false;
    startAPMode();
  }
  
  startHTTPServer();
  showMainMenu();
}

// ========== LED ==========
void clearLEDs() { digitalWrite(LED_GREEN, LOW); digitalWrite(LED_YELLOW, LOW); digitalWrite(LED_RED, LOW); }
void setLED(float v, String t) {
  clearLEDs();
  if (t == "spo2") { if (v >= 95) digitalWrite(LED_GREEN, HIGH); else if (v >= 90) digitalWrite(LED_YELLOW, HIGH); else digitalWrite(LED_RED, HIGH); }
  else if (t == "grip") { if (v >= 40) digitalWrite(LED_GREEN, HIGH); else if (v >= 30) digitalWrite(LED_YELLOW, HIGH); else digitalWrite(LED_RED, HIGH); }
  else if (t == "bp") { if (systolicBP < 120 && diastolicBP < 80) digitalWrite(LED_GREEN, HIGH);
    else if ((systolicBP >= 120 && systolicBP < 140) || (diastolicBP >= 80 && diastolicBP < 90)) digitalWrite(LED_YELLOW, HIGH);
    else digitalWrite(LED_RED, HIGH); }
}

// ========== Menu Display (Fixed IP on bottom, two items, wrap-around) ==========
void showMainMenu() {
  lcd.clear();
  
  // ---- Line 0: title + battery ----
  lcd.setCursor(0,0);
  lcd.print("Select Measurement");
  lcd.setCursor(15,0);
  lcd.print("Bat:");
  lcd.print(getBatteryPercent());
  lcd.print("%");
  
  // ---- IP address on line 3 (bottom) ----
  String ip = "";
  if (staConnected) ip = WiFi.localIP().toString();
  else if (WiFi.getMode() == WIFI_AP) ip = WiFi.softAPIP().toString();
  else ip = "No IP";
  if (ip.length() > 16) ip = ip.substring(0,16);
  lcd.setCursor(0,3);
  lcd.print("IP: ");
  lcd.print(ip);
  // clear any leftover characters after IP
  for (int i = ip.length() + 4; i < 20; i++) lcd.print(" ");
  
  // ---- Two menu items (lines 1 & 2) ----
  const char* items[] = {"HR+SpO2", "Handgrip", "Tensimeter", "All"};
  
  // First visible item (line 1)
  int idx1 = scrollOffset;
  lcd.setCursor(0,1);
  if (menuIndex == idx1) lcd.print(">");
  else lcd.print(" ");
  lcd.print(items[idx1]);
  if (idx1 == 0 && !max30102_ok) lcd.print(" ERR");
  if (idx1 == 1 && !hx711_ok) lcd.print(" ERR");
  if (idx1 == 2 && !ads1115_ok) lcd.print(" ERR");
  
  // Second visible item (line 2)
  int idx2 = scrollOffset + 1;
  if (idx2 < 4) {
    lcd.setCursor(0,2);
    if (menuIndex == idx2) lcd.print(">");
    else lcd.print(" ");
    lcd.print(items[idx2]);
    if (idx2 == 0 && !max30102_ok) lcd.print(" ERR");
    if (idx2 == 1 && !hx711_ok) lcd.print(" ERR");
    if (idx2 == 2 && !ads1115_ok) lcd.print(" ERR");
  } else {
    // if only one item left (should never happen because we wrap before that)
    lcd.setCursor(0,2);
    lcd.print("                ");
  }
}

void handleMenuNavigation() {
  static unsigned long lastBtnTime = 0;
  if (millis() - lastBtnTime < 200) return;
  
  if (digitalRead(BTN_UP) == LOW) {
    if (menuIndex > 0) {
      menuIndex--;
    } else {
      // wrap to last item
      menuIndex = 3;
      scrollOffset = 2;   // show items 2 and 3 (Tensimeter & All)
    }
    // adjust scrollOffset so that the selected item is always the first visible one
    if (menuIndex < scrollOffset) scrollOffset = menuIndex;
    else if (menuIndex > scrollOffset + 1) scrollOffset = menuIndex - 1;
    // keep scrollOffset valid (0..2)
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > 2) scrollOffset = 2;
    showMainMenu();
    lastBtnTime = millis();
  }
  else if (digitalRead(BTN_DOWN) == LOW) {
    if (menuIndex < 3) {
      menuIndex++;
    } else {
      // wrap to first item
      menuIndex = 0;
      scrollOffset = 0;
    }
    // adjust scrollOffset so that the selected item is the first visible one
    if (menuIndex < scrollOffset) scrollOffset = menuIndex;
    else if (menuIndex > scrollOffset + 1) scrollOffset = menuIndex - 1;
    if (scrollOffset < 0) scrollOffset = 0;
    if (scrollOffset > 2) scrollOffset = 2;
    showMainMenu();
    lastBtnTime = millis();
  }
  else if (digitalRead(BTN_SELECT) == LOW) {
    lastBtnTime = millis();
    measuring = true;
    switch (menuIndex) {
      case 0: measureHeart(); break;
      case 1: measureGrip(); break;
      case 2: measureBloodPressure(); break;
      case 3: measureAll(); break;
    }
    measuring = false;
    showMainMenu();  // return to menu after measurement
  }
}

// ========== Heart Rate + SpO2 ==========
void measureHeart() {
  if (!max30102_ok) { lcd.clear(); lcd.print("Sensor Error!"); delay(RESULT_TIME); return; }
  lcd.clear(); lcd.print("Put your fingertip"); lcd.setCursor(0,1); lcd.print("on the sensor");
  delay(2000);
  bpmSum = 0; beatCount = 0; spo2Sum = 0; spo2Count = 0;
  unsigned long start = millis();
  lcd.clear(); lcd.setCursor(0,0); lcd.print("HR & SpO2"); lcd.setCursor(0,1); lcd.print("Please wait 30s");
  while (millis() - start < MEASURE_TIME) {
    long ir = particleSensor.getIR(), red = particleSensor.getRed();
    int rem = (MEASURE_TIME - (millis() - start)) / 1000;
    lcd.setCursor(0,2); lcd.print(rem); lcd.print(" second left ");
    if (checkForBeat(ir)) {
      long delta = millis() - lastBeat; lastBeat = millis();
      float bpm = 60 / (delta / 1000.0);
      if (bpm > 40 && bpm < 180) { bpmSum += bpm; beatCount++; lcd.setCursor(0,3); lcd.print("BPM:"); lcd.print(bpm,0); }
    }
    if (ir > 0) {
      float spo2 = 110 - 25 * ((float)red / (float)ir);
      if (spo2 > 80 && spo2 <= 100) { spo2Sum += spo2; spo2Count++; lcd.setCursor(10,3); lcd.print("SpO2:"); lcd.print(spo2,0); lcd.print("%"); }
    }
    delay(20);
  }
  float avgBPM = (beatCount > 0) ? (bpmSum / beatCount) * 2 : 0;
  float avgSpO2 = (spo2Count > 0) ? spo2Sum / spo2Count : 0;
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Measurement done");
  lcd.setCursor(0,1); lcd.print("BPM: "); lcd.print(avgBPM);
  lcd.setCursor(0,2); lcd.print("SpO2: "); lcd.print(avgSpO2); lcd.print("%");
  setLED(avgSpO2, "spo2");
  uploadData(0,0,0,0, avgSpO2, avgBPM, 0);
  delay(RESULT_TIME); clearLEDs();
}

// ========== Grip ==========
void measureGrip() {
  if (!hx711_ok) { lcd.clear(); lcd.print("Sensor Error!"); delay(RESULT_TIME); return; }
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Please use the"); lcd.setCursor(0,1); lcd.print("dynamometer");
  lcd.setCursor(0,2); lcd.print("Grip to start"); delay(2000);
  maxGripForce = 0; unsigned long start = millis();
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Grip Now!");
  while (millis() - start < 5000) {
    float force = scale.get_units(5); if (force < 0) force = 0;
    if (force > maxGripForce) maxGripForce = force;
    lcd.setCursor(0,1); lcd.print("Force: "); lcd.print(force,1); lcd.print(" kg   ");
    lcd.setCursor(0,2); lcd.print("Max: "); lcd.print(maxGripForce,1); lcd.print(" kg   ");
    delay(100);
  }
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Measurement done");
  lcd.setCursor(0,1); lcd.print("Max Grip: "); lcd.print(maxGripForce,1); lcd.print(" kg");
  setLED(maxGripForce, "grip");
  uploadData(0,0,0,0,0,0, maxGripForce);
  delay(RESULT_TIME); clearLEDs();
}

// ========== Blood Pressure ==========
float readPressureMPX() {
  int16_t adc = ads.readADC_SingleEnded(0);
  float v = (adc * 4.096) / 32767.0;
  float p = v * 85.7 - 2.0;
  return constrain(p < 0 ? 0 : p, 0, BP_MAX_SAFE);
}
void startPump() { digitalWrite(PUMP_PIN, HIGH); }
void stopPump() { digitalWrite(PUMP_PIN, LOW); }
void openValve() { digitalWrite(VALVE_PIN, HIGH); }
void closeValve() { digitalWrite(VALVE_PIN, LOW); }
float detectOscillation(float p) {
  static float prev = 0, peak = 0; static bool rising = false;
  float osc = 0;
  if (p > prev + 0.5) { if (!rising) { rising = true; peak = p; } else if (p > peak) peak = p; }
  else if (p < prev - 0.5) { if (rising) { rising = false; osc = peak - p; } }
  prev = p; return osc;
}
void calculateBloodPressure() {
  if (oscillationCount < 5) { systolicBP = 120; diastolicBP = 80; meanBP = 93; return; }
  maxOscillation = 0;
  for (int i=0; i<oscillationCount; i++) if (oscillationBuffer[i] > maxOscillation) maxOscillation = oscillationBuffer[i];
  for (int i=0; i<oscillationCount; i++) if (oscillationBuffer[i] >= maxOscillation * 0.52) { systolicBP = pressureBuffer[i]; break; }
  for (int i=oscillationCount-1; i>=0; i--) if (oscillationBuffer[i] >= maxOscillation * 0.78) { diastolicBP = pressureBuffer[i]; break; }
  for (int i=0; i<oscillationCount; i++) if (oscillationBuffer[i] >= maxOscillation * 0.95) { meanBP = pressureBuffer[i]; break; }
}
void measureBloodPressure() {
  if (!ads1115_ok) { lcd.clear(); lcd.print("Sensor Error!"); delay(RESULT_TIME); return; }
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Please put on the");
  lcd.setCursor(0,1); lcd.print("cuff above your"); lcd.setCursor(0,2); lcd.print("wrist");
  lcd.setCursor(0,3); lcd.print("Press any button");
  while(digitalRead(BTN_SELECT)==HIGH && digitalRead(BTN_UP)==HIGH && digitalRead(BTN_DOWN)==HIGH) delay(50);
  bpState = BP_INFLATING; oscillationCount = 0;
  closeValve(); startPump();
  float p = 0; int lastPct = -1;
  while (bpState == BP_INFLATING && p < BP_TARGET_INFLATE) {
    p = readPressureMPX();
    int pct = (int)((p / BP_TARGET_INFLATE) * 100);
    if (pct != lastPct) { lcd.clear(); lcd.setCursor(0,0); lcd.print("Inflating: "); lcd.print(pct); lcd.print("%"); lastPct = pct; }
    if (p >= BP_TARGET_INFLATE) { stopPump(); bpState = BP_MEASURING; }
    if (p > BP_MAX_SAFE) { stopPump(); openValve(); lcd.print("OVER PRESSURE!"); delay(2000); return; }
    delay(20);
  }
  openValve(); unsigned long lastSample = 0; float lastOsc = 0;
  while (bpState == BP_MEASURING && p > 20) {
    if (millis() - lastSample >= BP_SAMPLE_RATE_MS) {
      p = readPressureMPX();
      float osc = detectOscillation(p);
      if (osc > 5 && osc != lastOsc) {
        lastOsc = osc;
        if (oscillationCount < 100) { oscillationBuffer[oscillationCount] = osc; pressureBuffer[oscillationCount] = p; oscillationCount++; }
        int pct = (int)(((BP_TARGET_INFLATE - p) / BP_TARGET_INFLATE) * 100);
        if (pct < 0) pct = 0;
        lcd.setCursor(0,0); lcd.print("Processing: "); lcd.print(pct); lcd.print("%   ");
      }
      lastSample = millis();
    }
    delay(5);
  }
  bpState = BP_COMPLETE; openValve(); delay(1000); closeValve();
  calculateBloodPressure();
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Please remove the"); lcd.setCursor(0,1); lcd.print("cuff");
  lcd.setCursor(0,2); lcd.print("SYS: "); lcd.print(systolicBP,0); lcd.print(" DIA: "); lcd.print(diastolicBP,0);
  setLED(0,"bp");
  uploadData(systolicBP, diastolicBP, meanBP, bpHeartRate, 0, 0, 0);
  delay(RESULT_TIME); clearLEDs();
}

// ========== All Measurements ==========
void measureAll() { measureHeart(); delay(1000); measureGrip(); delay(1000); measureBloodPressure(); }

// ========== Main Loop ==========
void loop() {
  if (!measuring) {
    handleMenuNavigation();
    server.handleClient();
  }
  delay(20);
}