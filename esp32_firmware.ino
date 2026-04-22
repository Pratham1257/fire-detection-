#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --------------------------------------------------
// WiFi and Backend Configuration
// --------------------------------------------------
const char* WIFI_SSID     = "onyx";
const char* WIFI_PASSWORD = "love162004";

const char* API_HOST_PRIMARY  = "10.220.51.180";
const char* API_HOST_ALT_LAN  = "10.19.205.180";
const char* API_HOST_FALLBACK = "192.168.137.1";
const uint16_t API_PORT       = 8000;

const char* API_URL_PRIMARY  = "http://10.220.51.180:8000/api/v1/events/predict";
const char* API_URL_ALT_LAN  = "http://10.19.205.180:8000/api/v1/events/predict";
const char* API_URL_FALLBACK = "http://192.168.137.1:8000/api/v1/events/predict";

const char* DEVICE_ID = "esp32-node-01";

const int           WIFI_CONNECT_TIMEOUT_MS              = 20000;
const int           WIFI_DHCP_WAIT_MS                    = 8000;
const int           BACKEND_HTTP_TIMEOUT_MS              = 7000;
const int           BACKEND_POST_RETRY_COUNT             = 2;
const int           BACKEND_POST_RETRY_DELAY_MS          = 350;
const unsigned long BACKEND_REACHABILITY_LOG_INTERVAL_MS = 10000;
const unsigned long NETWORK_DIAG_LOG_INTERVAL_MS         = 15000;

const unsigned long SMOKE_WARMUP_MS            = 20000;
const int           SMOKE_BASELINE_SAMPLES     = 80;
const float         SMOKE_FILTER_ALPHA         = 0.80f;
const float         SMOKE_NOISE_FLOOR          = 120.0f;
const float         SMOKE_BASELINE_DRIFT_ALPHA = 0.999f;

const int           FLAME_CALIBRATION_SAMPLES   = 120;
const int           FLAME_FAULT_CONFIRM_CYCLES  = 6;
const float         FLAME_FAULT_MAX_TEMP_C      = 45.0f;
const float         FLAME_FAULT_MIN_HUMIDITY    = 55.0f;
const int           FLAME_FAULT_MAX_SMOKE_LEVEL = 120;
const unsigned long FLAME_FAULT_LOG_INTERVAL_MS = 10000;

// --------------------------------------------------
// Pin Configuration
// --------------------------------------------------
#define DHT_PIN    4
#define DHT_TYPE   DHT22
#define MQ_PIN     34
#define FLAME_PIN  27
#define BUZZER_PIN 25
#define RELAY_PIN  33
#define LED_GREEN  12
#define LED_YELLOW 14
#define LED_RED    26

const bool FLAME_ACTIVE_LOW = true;

const unsigned long SAMPLE_INTERVAL_MS     = 5000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 15000;

const int SMOKE_ZERO_THRESHOLD      = 15;
const int SMOKE_SENSOR_FAULT_CYCLES = 3;

// --------------------------------------------------
// Global State
// --------------------------------------------------
unsigned long lastSampleMs                = 0;
unsigned long lastWifiRetryMs             = 0;
unsigned long lastBackendReachabilityLogMs= 0;
unsigned long lastFlameFaultLogMs         = 0;
unsigned long lastNetworkDiagLogMs        = 0;

float smokeBaselineRaw   = 0.0f;
float smokeFilteredLevel = 0.0f;
bool  smokeCalibrated    = false;

int  flameInactiveLevel           = HIGH;
int  flameSuspiciousCycles        = 0;
bool flameInactiveLevelCalibrated = false;

int smokeZeroCycles = 0;

LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHT_PIN, DHT_TYPE);

// --------------------------------------------------
// Sensor Packet
// --------------------------------------------------
struct SensorPacket {
  float temperature;
  float humidity;
  int   smokeRaw;
  int   smokeGasLevel;
  int   flameDetected;
  bool  smokeSensorFault;
  bool  flameSensorFault;
};

// --------------------------------------------------
// Hardware Helpers
// --------------------------------------------------
void setLeds(bool green, bool yellow, bool red) {
  digitalWrite(LED_GREEN,  green  ? HIGH : LOW);
  digitalWrite(LED_YELLOW, yellow ? HIGH : LOW);
  digitalWrite(LED_RED,    red    ? HIGH : LOW);
}

void relayOn()  { digitalWrite(RELAY_PIN, HIGH); }
void relayOff() { digitalWrite(RELAY_PIN, LOW);  }

void buzzerBeep(int onMs, int offMs, int count) {
  for (int i = 0; i < count; i++) {
    digitalWrite(BUZZER_PIN, HIGH); delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);  delay(offMs);
  }
}

// --------------------------------------------------
// WiFi Helpers
// --------------------------------------------------
bool usingPlaceholderCredentials() {
  return String(WIFI_SSID) == "YOUR_WIFI_SSID" ||
         String(WIFI_PASSWORD) == "YOUR_WIFI_PASSWORD";
}

const char* wifiStatusToText(wl_status_t s) {
  switch (s) {
    case WL_CONNECTED:       return "Connected";
    case WL_NO_SSID_AVAIL:   return "SSID Not Found";
    case WL_CONNECT_FAILED:  return "Connect Failed";
    case WL_CONNECTION_LOST: return "Connection Lost";
    case WL_DISCONNECTED:    return "Disconnected";
    case WL_IDLE_STATUS:     return "Idle";
    default:                 return "Unknown";
  }
}

void printWifiFailureHints(wl_status_t status) {
  if (status == WL_NO_SSID_AVAIL) {
    Serial.println("Hint: SSID not found. Check WiFi name spelling and router band.");
    return;
  }
  if (status == WL_CONNECT_FAILED || status == WL_DISCONNECTED) {
    Serial.println("Hint: Check WiFi password and security mode.");
    Serial.println("Hint: Many ESP32 boards cannot connect to WPA3-only networks.");
    Serial.println("Hint: Set router/hotspot to WPA2 or WPA2/WPA3 mixed mode.");
  }
}

void printTargetNetworkInfo() {
  int found = WiFi.scanNetworks();
  if (found <= 0) { Serial.println("WiFi scan: no networks found."); return; }

  bool targetFound = false;
  for (int i = 0; i < found; i++) {
    if (WiFi.SSID(i) == WIFI_SSID) {
      targetFound = true;
      Serial.print("WiFi scan target found. RSSI=");
      Serial.print(WiFi.RSSI(i));
      Serial.print(" dBm, Channel=");
      Serial.print(WiFi.channel(i));
      Serial.print(", EncryptionType=");
      Serial.println((int)WiFi.encryptionType(i));
      break;
    }
  }
  if (!targetFound) Serial.println("WiFi scan: target SSID not visible to ESP32.");
  WiFi.scanDelete();
}

bool isSameSubnet(IPAddress a, IPAddress b, IPAddress mask) {
  for (int i = 0; i < 4; i++)
    if ((a[i] & mask[i]) != (b[i] & mask[i])) return false;
  return true;
}

void logNetworkDiagnostics() {
  if (lastNetworkDiagLogMs != 0 &&
      millis() - lastNetworkDiagLogMs < NETWORK_DIAG_LOG_INTERVAL_MS) return;
  lastNetworkDiagLogMs = millis();

  IPAddress localIp    = WiFi.localIP();
  IPAddress gatewayIp  = WiFi.gatewayIP();
  IPAddress subnetMask = WiFi.subnetMask();

  Serial.print("Network diag - Local/Gateway/Mask: ");
  Serial.print(localIp); Serial.print(" / ");
  Serial.print(gatewayIp); Serial.print(" / ");
  Serial.println(subnetMask);

  IPAddress h;
  if (h.fromString(API_HOST_PRIMARY)) {
    Serial.print("Primary host same subnet: ");
    Serial.println(isSameSubnet(localIp, h, subnetMask) ? "YES" : "NO");
  }
  if (h.fromString(API_HOST_ALT_LAN)) {
    Serial.print("Alt host same subnet: ");
    Serial.println(isSameSubnet(localIp, h, subnetMask) ? "YES" : "NO");
  }
  if (h.fromString(API_HOST_FALLBACK)) {
    Serial.print("Hotspot host same subnet: ");
    Serial.println(isSameSubnet(localIp, h, subnetMask) ? "YES" : "NO");
  }
}

// --------------------------------------------------
// connectWifi
// FIX 1: Wait for both WL_CONNECTED AND a valid DHCP IP (not just WL_CONNECTED).
// Previously the loop exited as soon as WL_CONNECTED was true, but at that
// point the IP could still be 0.0.0.0 causing every POST to fail immediately.
// --------------------------------------------------
void connectWifi() {
  if (usingPlaceholderCredentials()) {
    Serial.println("WiFi credentials are placeholders. Update WIFI_SSID and WIFI_PASSWORD.");
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(false);

  printTargetNetworkInfo();
  WiFi.disconnect(true);
  delay(200);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi SSID: ");
  Serial.println(WIFI_SSID);

  unsigned long startMs = millis();
  while (millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
    if (WiFi.status() == WL_CONNECTED &&
        WiFi.localIP() != IPAddress(0, 0, 0, 0)) break;   // FIX 1
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED &&
      WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    Serial.println();
    Serial.print("Connected. IP Address: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.print("WiFi connection failed. Status: ");
    wl_status_t status = (wl_status_t)WiFi.status();
    Serial.println(wifiStatusToText(status));
    printWifiFailureHints(status);
    Serial.println("Running in local mode.");
  }
}

// --------------------------------------------------
// ensureWifiConnected
// FIX 2: Added DHCP polling loop after WiFi.begin().
// Previously the function returned immediately after WiFi.begin() with no wait,
// so the next loop() iteration called postToBackend() before DHCP was done,
// resulting in IP 0.0.0.0 and "connection refused" on every endpoint.
// --------------------------------------------------
void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED &&
      WiFi.localIP() != IPAddress(0, 0, 0, 0)) return;   // FIX 2

  if (millis() - lastWifiRetryMs < WIFI_RETRY_INTERVAL_MS) return;
  lastWifiRetryMs = millis();

  if (usingPlaceholderCredentials()) {
    Serial.println("WiFi retry skipped: placeholder credentials.");
    return;
  }

  Serial.print("WiFi disconnected. Retrying. Last status: ");
  wl_status_t status = (wl_status_t)WiFi.status();
  Serial.println(wifiStatusToText(status));
  printWifiFailureHints(status);

  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  // FIX 2: Wait up to WIFI_DHCP_WAIT_MS for association + DHCP lease.
  unsigned long t = millis();
  while (millis() - t < WIFI_DHCP_WAIT_MS) {
    if (WiFi.status() == WL_CONNECTED &&
        WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
      Serial.print("Reconnected. IP: ");
      Serial.println(WiFi.localIP());
      return;
    }
    delay(300);
  }
  Serial.println("WiFi reconnect timed out. Will retry next cycle.");
}

// --------------------------------------------------
// Smoke Sensor
// --------------------------------------------------
int readSmokeAverage(int samples) {
  long total = 0;
  for (int i = 0; i < samples; i++) { total += analogRead(MQ_PIN); delay(10); }
  return (int)(total / samples);
}

void calibrateSmokeBaseline() {
  Serial.println("Warming up smoke sensor...");
  unsigned long start = millis();
  while (millis() - start < SMOKE_WARMUP_MS) delay(100);

  long total = 0;
  for (int i = 0; i < SMOKE_BASELINE_SAMPLES; i++) { total += analogRead(MQ_PIN); delay(15); }

  smokeBaselineRaw = (float)total / (float)SMOKE_BASELINE_SAMPLES;
  if (smokeBaselineRaw < 0.0f) smokeBaselineRaw = 0.0f;
  smokeFilteredLevel = 0.0f;
  smokeCalibrated    = true;

  Serial.print("Smoke baseline calibrated (raw): ");
  Serial.println((int)smokeBaselineRaw);
}

int normalizeSmokeLevel(int rawSmoke) {
  if (!smokeCalibrated) {
    smokeBaselineRaw   = (float)rawSmoke;
    smokeFilteredLevel = 0.0f;
    smokeCalibrated    = true;
  }

  float adjusted = (float)rawSmoke - smokeBaselineRaw;
  if (adjusted < 0.0f) adjusted = 0.0f;

  adjusted -= SMOKE_NOISE_FLOOR;
  if (adjusted < 0.0f) adjusted = 0.0f;

  if (adjusted < (SMOKE_NOISE_FLOOR * 0.5f)) {
    smokeBaselineRaw = (SMOKE_BASELINE_DRIFT_ALPHA * smokeBaselineRaw)
                     + ((1.0f - SMOKE_BASELINE_DRIFT_ALPHA) * (float)rawSmoke);
  }

  smokeFilteredLevel = (SMOKE_FILTER_ALPHA * smokeFilteredLevel)
                     + ((1.0f - SMOKE_FILTER_ALPHA) * adjusted);
  if (smokeFilteredLevel < 0.0f) smokeFilteredLevel = 0.0f;

  return (int)(smokeFilteredLevel + 0.5f);
}

// --------------------------------------------------
// Flame Sensor
// --------------------------------------------------
void calibrateFlameInactiveLevel() {
  int lowCount = 0, highCount = 0;
  Serial.println("Calibrating flame sensor baseline...");

  for (int i = 0; i < FLAME_CALIBRATION_SAMPLES; i++) {
    if (digitalRead(FLAME_PIN) == LOW) lowCount++;
    else highCount++;
    delay(5);
  }

  flameInactiveLevel = (highCount == lowCount)
    ? (FLAME_ACTIVE_LOW ? HIGH : LOW)
    : (highCount > lowCount ? HIGH : LOW);

  flameInactiveLevelCalibrated = true;

  Serial.print("Flame baseline LOW/HIGH samples: ");
  Serial.print(lowCount); Serial.print("/"); Serial.println(highCount);
  Serial.print("Flame inactive level: ");
  Serial.println(flameInactiveLevel == LOW ? "LOW" : "HIGH");
}

int readStableFlameState(int samples) {
  if (!flameInactiveLevelCalibrated)
    flameInactiveLevel = FLAME_ACTIVE_LOW ? HIGH : LOW;

  int activeCount = 0;
  for (int i = 0; i < samples; i++) {
    if (digitalRead(FLAME_PIN) != flameInactiveLevel) activeCount++;
    delay(2);
  }
  return activeCount >= ((samples / 2) + 1) ? 1 : 0;
}

// --------------------------------------------------
// readSensors
// --------------------------------------------------
SensorPacket readSensors() {
  SensorPacket packet;

  packet.temperature   = dht.readTemperature();
  packet.humidity      = dht.readHumidity();
  packet.smokeRaw      = readSmokeAverage(10);
  packet.smokeGasLevel = normalizeSmokeLevel(packet.smokeRaw);

  // FIX 5: Decrement instead of hard-reset to 0.
  // A single non-zero ADC reading previously cleared the entire fault counter,
  // allowing a real wiring/power fault to be hidden by one brief spike.
  if (packet.smokeRaw <= SMOKE_ZERO_THRESHOLD) {
    smokeZeroCycles++;
  } else if (smokeZeroCycles > 0) {
    smokeZeroCycles--;   // FIX 5
  }
  packet.smokeSensorFault = smokeZeroCycles >= SMOKE_SENSOR_FAULT_CYCLES;

  packet.flameDetected = readStableFlameState(7);

  if (isnan(packet.temperature)) packet.temperature = 0.0;
  if (isnan(packet.humidity))    packet.humidity    = 0.0;

  bool likelyFalseFlame = packet.flameDetected == 1
    && packet.smokeGasLevel <= FLAME_FAULT_MAX_SMOKE_LEVEL
    && packet.temperature   <  FLAME_FAULT_MAX_TEMP_C
    && packet.humidity      >  FLAME_FAULT_MIN_HUMIDITY;

  if (likelyFalseFlame) flameSuspiciousCycles++;
  else if (flameSuspiciousCycles > 0) flameSuspiciousCycles--;

  packet.flameSensorFault = flameSuspiciousCycles >= FLAME_FAULT_CONFIRM_CYCLES;

  if (packet.flameSensorFault) {
    packet.flameDetected = 0;
    if (millis() - lastFlameFaultLogMs > FLAME_FAULT_LOG_INTERVAL_MS) {
      lastFlameFaultLogMs = millis();
      Serial.println("Flame sensor stuck active under safe conditions — Flame Sensor Fault.");
    }
  }

  return packet;
}

// --------------------------------------------------
// Local Warning Logic
// --------------------------------------------------
String localInitialWarning(const SensorPacket& p) {
  if (p.temperature == 0 && p.humidity == 0 &&
      p.smokeGasLevel == 0 && p.flameDetected == 0)     return "Sensor Fault";
  if (p.smokeRaw > 4000)                                 return "Sensor Malfunction";
  if (p.smokeSensorFault && p.flameDetected == 0)        return "Smoke Sensor Fault";
  if (p.flameSensorFault)                                return "Flame Sensor Fault";
  if (p.flameDetected == 1)                              return "Immediate Fire Alert";
  if (p.humidity < 30 && p.temperature > 45 &&
      p.smokeGasLevel > 700)                             return "Immediate Fire Alert";
  if (p.smokeGasLevel > 1200 && p.temperature > 50)      return "Immediate Fire Alert";
  if (p.flameDetected == 0 && p.temperature > 55 &&
      p.smokeGasLevel > 900 && p.humidity < 40)          return "Electrical Fire Risk";
  if (p.flameDetected == 0 && p.humidity > 85 &&
      p.smokeGasLevel >= 300 && p.smokeGasLevel < 800 &&
      p.temperature < 50)                                return "Steam Detected";
  if (p.flameDetected == 0 && p.temperature > 50 &&
      p.smokeGasLevel < 300)                             return "Heat Alert";
  if (p.smokeGasLevel >= 300 && p.smokeGasLevel < 500)   return "Mild Smoke Alert";
  if (p.smokeGasLevel >= 500 && p.smokeGasLevel < 800)   return "Smoke Warning Alert";
  if (p.smokeGasLevel >= 800 && p.smokeGasLevel <= 1200) return "Gas Leakage Alert";
  return "No Immediate Alert";
}

// --------------------------------------------------
// LCD
// --------------------------------------------------
void updateLCD(const SensorPacket& p, const String& status) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("T:"); lcd.print(p.temperature, 1);
  lcd.print("C S:"); lcd.print(p.smokeGasLevel);
  lcd.setCursor(0, 1);
  lcd.print(status.length() > 16 ? status.substring(0, 16) : status);
}

// --------------------------------------------------
// Local Fallback Actions
// --------------------------------------------------
void applyLocalFallback(const String& w, const SensorPacket& p) {
  if (w == "Flame Sensor Fault")   { setLeds(0,1,0); relayOff(); buzzerBeep(100,140,2); updateLCD(p,"Flame SensorFlt"); return; }
  if (w == "Smoke Sensor Fault")   { setLeds(0,1,0); relayOff(); buzzerBeep(120,120,2); updateLCD(p,"Smoke SensorFlt"); return; }
  if (w == "Sensor Malfunction")   { setLeds(0,1,0); relayOff(); buzzerBeep(110,90,2);  updateLCD(p,"Sensor Malfunc");  return; }
  if (w == "Sensor Fault")         { setLeds(0,1,0); relayOff(); buzzerBeep(90,90,2);   updateLCD(p,"Sensor Fault");    return; }
  if (w == "No Immediate Alert")   { setLeds(1,0,0); relayOff(); digitalWrite(BUZZER_PIN,LOW); updateLCD(p,"Safe"); return; }
  if (w == "Mild Smoke Alert")     { setLeds(1,1,0); relayOff(); buzzerBeep(80,120,1);  updateLCD(p,"Mild Smoke");      return; }
  if (w == "Smoke Warning Alert")  { setLeds(0,1,0); relayOff(); buzzerBeep(120,140,2); updateLCD(p,"Smoke Warning");   return; }
  if (w == "Gas Leakage Alert")    { setLeds(0,1,1); relayOn();  buzzerBeep(220,130,3); updateLCD(p,"Gas Leakage");     return; }
  if (w == "Heat Alert")           { setLeds(0,1,0); relayOff(); buzzerBeep(160,140,2); updateLCD(p,"Heat Alert");      return; }
  if (w == "Steam Detected")       { setLeds(1,1,0); relayOff(); buzzerBeep(70,130,1);  updateLCD(p,"Steam Detected");  return; }
  if (w == "Electrical Fire Risk") { setLeds(0,1,1); relayOn();  buzzerBeep(260,120,3); updateLCD(p,"Elec Fire Risk");  return; }
  if (w == "Immediate Fire Alert") { setLeds(0,0,1); relayOn();  buzzerBeep(450,100,3); updateLCD(p,"Fire Emergency");  return; }
  setLeds(0,1,0); relayOff(); buzzerBeep(100,80,1); updateLCD(p,"Unknown Status");
}

// --------------------------------------------------
// Server Decision Actions
// --------------------------------------------------
void applyServerDecision(const String& pred, const SensorPacket& p) {
  if (pred == "Sensor Malfunction")   { setLeds(0,1,0); relayOff(); buzzerBeep(110,90,2);  updateLCD(p,"Sensor Malfunc");  return; }
  if (pred == "Sensor Fault")         { setLeds(0,1,0); relayOff(); buzzerBeep(90,90,2);   updateLCD(p,"Sensor Fault");    return; }
  if (pred == "False Alarm")          { setLeds(1,0,0); relayOff(); digitalWrite(BUZZER_PIN,LOW); updateLCD(p,"False Alarm"); return; }
  if (pred == "Smoke Warning")        { setLeds(0,1,0); relayOff(); buzzerBeep(120,140,2); updateLCD(p,"Smoke Warning");   return; }
  if (pred == "Gas Leakage")          { setLeds(0,1,1); relayOn();  buzzerBeep(220,130,3); updateLCD(p,"Gas Leakage");     return; }
  if (pred == "Heat Alert")           { setLeds(0,1,0); relayOff(); buzzerBeep(160,140,2); updateLCD(p,"Heat Alert");      return; }
  if (pred == "Steam Detected")       { setLeds(1,1,0); relayOff(); buzzerBeep(70,130,1);  updateLCD(p,"Steam Detected");  return; }
  if (pred == "Electrical Fire Risk") { setLeds(0,1,1); relayOn();  buzzerBeep(260,120,3); updateLCD(p,"Elec Fire Risk");  return; }
  if (pred == "Fire Emergency")       { setLeds(0,0,1); relayOn();  buzzerBeep(450,100,3); updateLCD(p,"Fire Emergency");  return; }
  setLeds(0,1,0); relayOff(); buzzerBeep(100,80,1); updateLCD(p,"Unknown Status");
}

// --------------------------------------------------
// postToBackend
// --------------------------------------------------
bool postToBackend(const SensorPacket& packet, String& predictionOut) {
  // FIX 1: Block HTTP attempts when DHCP hasn't assigned an IP yet.
  // WL_CONNECTED can be true while localIP() is still 0.0.0.0 — every TCP
  // connect in that state fails with "connection refused" immediately.
  if (WiFi.status() != WL_CONNECTED) return false;
  if (WiFi.localIP() == IPAddress(0, 0, 0, 0)) {
    Serial.println("WiFi connected but no IP yet (DHCP pending). Skipping POST.");
    return false;
  }

  logNetworkDiagnostics();

  IPAddress localIp    = WiFi.localIP();
  IPAddress subnetMask = WiFi.subnetMask();
  IPAddress primaryHost;
  bool primarySameSubnet = false;
  if (primaryHost.fromString(API_HOST_PRIMARY))
    primarySameSubnet = isSameSubnet(localIp, primaryHost, subnetMask);

  // FIX 4a: Request document size 256 -> 384 to fit all fields without overflow.
  // FIX 4b: Added smoke_sensor_fault — it was never sent to the server before.
  StaticJsonDocument<384> payload;
  payload["device_id"]          = DEVICE_ID;
  payload["temperature"]        = packet.temperature;
  payload["humidity"]           = packet.humidity;
  payload["smoke_gas_level"]    = packet.smokeGasLevel;
  payload["flame_detected"]     = packet.flameDetected;
  payload["flame_sensor_fault"] = packet.flameSensorFault;
  payload["smoke_sensor_fault"] = packet.smokeSensorFault; // FIX 4b

  String requestBody;
  serializeJson(payload, requestBody);

  // FIX 6: Array was declared size 5 but only 4 entries are ever added.
  String endpoints[4];
  int endpointCount = 0;
  endpoints[endpointCount++] = String(API_URL_PRIMARY);
  if (String(API_URL_ALT_LAN)  != endpoints[0]) endpoints[endpointCount++] = String(API_URL_ALT_LAN);
  if (String(API_URL_FALLBACK) != endpoints[0]) endpoints[endpointCount++] = String(API_URL_FALLBACK);

  // FIX 6: Guard against gateway being 0.0.0.0 — that would add a broken URL.
  String gatewayHost = WiFi.gatewayIP().toString();
  if (gatewayHost != "0.0.0.0" && gatewayHost.length() > 0) {
    String gatewayUrl = "http://" + gatewayHost + ":8000/api/v1/events/predict";
    bool alreadyPresent = false;
    for (int i = 0; i < endpointCount; i++)
      if (endpoints[i] == gatewayUrl) { alreadyPresent = true; break; }
    if (!alreadyPresent && endpointCount < 4)
      endpoints[endpointCount++] = gatewayUrl;
  }

  int    lastHttpCode      = -999;
  String lastTriedEndpoint = "";

  for (int ei = 0; ei < endpointCount; ei++) {
    const char* url = endpoints[ei].c_str();
    Serial.print("Trying backend endpoint: "); Serial.println(url);

    for (int attempt = 1; attempt <= BACKEND_POST_RETRY_COUNT; attempt++) {
      HTTPClient http;
      http.begin(url);
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(BACKEND_HTTP_TIMEOUT_MS);

      int httpCode = http.POST(requestBody);
      lastHttpCode      = httpCode;
      lastTriedEndpoint = String(url);

      if (httpCode >= 200 && httpCode < 300) {
        String response = http.getString();
        http.end();

        // FIX 4c: Response document size 1024 -> 1536 to prevent silent truncation.
        StaticJsonDocument<1536> responseJson;
        DeserializationError err = deserializeJson(responseJson, response);
        if (err) { Serial.println("Backend response parse failed."); return false; }

        // FIX 4d: Log which key was matched so response-format mismatches are visible.
        const char* prediction = responseJson["event"]["prediction"];
        if (prediction == nullptr) {
          prediction = responseJson["prediction"];
          if (prediction != nullptr)
            Serial.println("Note: using top-level 'prediction' key (event.prediction absent).");
        }
        if (prediction == nullptr) {
          Serial.println("Backend response missing prediction field.");
          return false;
        }

        predictionOut = String(prediction);
        return true;
      }

      Serial.print("Backend POST failed (endpoint ");
      Serial.print(ei + 1); Serial.print("/"); Serial.print(endpointCount);
      Serial.print(", attempt ");
      Serial.print(attempt); Serial.print("/"); Serial.print(BACKEND_POST_RETRY_COUNT);
      Serial.print("). HTTP code: "); Serial.println(httpCode);

      if (httpCode > 0) {
        String body = http.getString();
        if (body.length() > 0) { Serial.print("Error body: "); Serial.println(body); }
      } else {
        Serial.print("Transport detail: "); Serial.println(HTTPClient::errorToString(httpCode));
        Serial.print("WiFi IP/Gateway: ");
        Serial.print(WiFi.localIP()); Serial.print(" / "); Serial.println(WiFi.gatewayIP());
        if (ei == 0 && primarySameSubnet) {
          Serial.println("Likely cause: host firewall or AP client isolation.");
          Serial.println("Action: allow inbound TCP 8000 on Windows, use Private WiFi profile.");
        }
        // FIX 3: Removed WiFi.disconnect()/reconnect() from here.
        // That caused a 700ms+ stall per retry attempt and tore down the
        // interface while other endpoints were still waiting to be tried.
        Serial.println("Transport error. Moving to next endpoint.");
      }

      http.end();
      if (attempt < BACKEND_POST_RETRY_COUNT) delay(BACKEND_POST_RETRY_DELAY_MS);
    }
  }

  if (millis() - lastBackendReachabilityLogMs > BACKEND_REACHABILITY_LOG_INTERVAL_MS) {
    lastBackendReachabilityLogMs = millis();
    Serial.println("Backend unreachable after trying all endpoints.");
    Serial.print("Last endpoint: "); Serial.println(lastTriedEndpoint);
    Serial.print("Last HTTP code: "); Serial.println(lastHttpCode);
    Serial.println("Hint: ensure backend is running and firewall allows TCP 8000.");
    Serial.println("Hint: ensure ESP32 and backend are on the same subnet.");
  }

  return false;
}

// --------------------------------------------------
// setup
// --------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("System booting...");
  Serial.print("Device ID: ");        Serial.println(DEVICE_ID);
  Serial.print("Primary API URL: ");  Serial.println(API_URL_PRIMARY);
  Serial.print("Alt LAN API URL: ");  Serial.println(API_URL_ALT_LAN);
  Serial.print("Fallback API URL: "); Serial.println(API_URL_FALLBACK);

  pinMode(FLAME_PIN,  INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN,  OUTPUT);
  pinMode(LED_GREEN,  OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED,    OUTPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(MQ_PIN, ADC_11db);

  digitalWrite(BUZZER_PIN, LOW);
  relayOff();
  setLeds(true, false, false);

  dht.begin();
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0); lcd.print("Fire System");
  lcd.setCursor(0, 1); lcd.print("Starting...");

  connectWifi();
  calibrateSmokeBaseline();
  calibrateFlameInactiveLevel();
}

// --------------------------------------------------
// loop
// --------------------------------------------------
void loop() {
  if (millis() - lastSampleMs < SAMPLE_INTERVAL_MS) {
    delay(50);
    return;
  }
  lastSampleMs = millis();

  ensureWifiConnected();

  SensorPacket packet = readSensors();

  Serial.println("----------------------------");
  Serial.print("Temperature: ");           Serial.println(packet.temperature);
  Serial.print("Humidity: ");              Serial.println(packet.humidity);
  Serial.print("Smoke/Gas Level: ");       Serial.println(packet.smokeGasLevel);
  Serial.print("Smoke Raw: ");             Serial.println(packet.smokeRaw);
  Serial.print("Smoke Raw (%ADC): ");
  Serial.println((int)((float)packet.smokeRaw / 4095.0f * 100.0f + 0.5f));
  Serial.print("Smoke Baseline Raw: ");    Serial.println((int)smokeBaselineRaw);
  Serial.print("Smoke Baseline (%ADC): ");
  Serial.println((int)(smokeBaselineRaw / 4095.0f * 100.0f + 0.5f));
  Serial.print("Flame Detected: ");        Serial.println(packet.flameDetected);
  Serial.print("Smoke Sensor Fault: ");    Serial.println(packet.smokeSensorFault ? "YES" : "NO");
  Serial.print("Flame Sensor Fault: ");    Serial.println(packet.flameSensorFault ? "YES" : "NO");
  Serial.print("WiFi: ");                  Serial.println(wifiStatusToText((wl_status_t)WiFi.status()));
  Serial.print("IP: ");                    Serial.println(WiFi.localIP());

  String warning = localInitialWarning(packet);
  Serial.print("Initial Warning: "); Serial.println(warning);

  String prediction;
  bool sent = postToBackend(packet, prediction);

  if (sent) {
    Serial.println("Backend: Connected");
    Serial.print("Server Prediction: "); Serial.println(prediction);
    applyServerDecision(prediction, packet);
  } else {
    Serial.println("Backend: Unavailable — using local logic.");
    applyLocalFallback(warning, packet);
  }
}
