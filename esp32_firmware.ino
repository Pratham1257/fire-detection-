struct SensorPacket;

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <DHT.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// --------------------------------------------------
// WiFi and Backend Configuration
// --------------------------------------------------
const char* WIFI_SSID = "Renu";
const char* WIFI_PASSWORD = "12345678";
const char* API_HOST_PRIMARY = "10.202.252.180";
const char* API_HOST_ALT_LAN = "10.19.205.180";
const char* API_HOST_FALLBACK = "192.168.137.1";
const uint16_t API_PORT = 8000;
const char* API_URL_PRIMARY = "http://10.202.252.180:8000/api/v1/events/predict";
const char* API_URL_ALT_LAN = "http://10.19.205.180:8000/api/v1/events/predict";
const char* API_URL_FALLBACK = "http://192.168.137.1:8000/api/v1/events/predict";
const char* DEVICE_ID = "esp32-node-01";
const int WIFI_CONNECT_TIMEOUT_MS = 20000;
const int BACKEND_HTTP_TIMEOUT_MS = 7000;
const int BACKEND_POST_RETRY_COUNT = 2;
const int BACKEND_POST_RETRY_DELAY_MS = 350;
const int BACKEND_NEGATIVE_CODE_WIFI_RECOVERY_DELAY_MS = 700;
const unsigned long BACKEND_REACHABILITY_LOG_INTERVAL_MS = 10000;
const unsigned long NETWORK_DIAG_LOG_INTERVAL_MS = 15000;
const unsigned long SMOKE_WARMUP_MS = 20000;
const int SMOKE_BASELINE_SAMPLES = 80;
const float SMOKE_FILTER_ALPHA = 0.80f;
const float SMOKE_NOISE_FLOOR = 120.0f;
const float SMOKE_BASELINE_DRIFT_ALPHA = 0.999f;
const int FLAME_CALIBRATION_SAMPLES = 120;
const int FLAME_FAULT_CONFIRM_CYCLES = 6;
const float FLAME_FAULT_MAX_TEMP_C = 45.0f;
const float FLAME_FAULT_MIN_HUMIDITY = 55.0f;
const int FLAME_FAULT_MAX_SMOKE_LEVEL = 120;
const unsigned long FLAME_FAULT_LOG_INTERVAL_MS = 10000;

unsigned long lastBackendReachabilityLogMs = 0;
unsigned long lastFlameFaultLogMs = 0;
unsigned long lastNetworkDiagLogMs = 0;
float smokeBaselineRaw = 0.0f;
float smokeFilteredLevel = 0.0f;
bool smokeCalibrated = false;
int flameInactiveLevel = HIGH;
int flameSuspiciousCycles = 0;
bool flameInactiveLevelCalibrated = false;

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
  int foundNetworks = WiFi.scanNetworks();
  if (foundNetworks <= 0) {
    Serial.println("WiFi scan: no networks found.");
    return;
  }

  bool targetFound = false;
  for (int i = 0; i < foundNetworks; i++) {
    String detectedSsid = WiFi.SSID(i);
    if (detectedSsid == WIFI_SSID) {
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

  if (!targetFound) {
    Serial.println("WiFi scan: target SSID not visible to ESP32.");
  }

  WiFi.scanDelete();
}

bool usingPlaceholderCredentials() {
  return String(WIFI_SSID) == "YOUR_WIFI_SSID" || String(WIFI_PASSWORD) == "YOUR_WIFI_PASSWORD";
}

const char* wifiStatusToText(wl_status_t status) {
  switch (status) {
    case WL_CONNECTED:
      return "Connected";
    case WL_NO_SSID_AVAIL:
      return "SSID Not Found";
    case WL_CONNECT_FAILED:
      return "Connect Failed";
    case WL_CONNECTION_LOST:
      return "Connection Lost";
    case WL_DISCONNECTED:
      return "Disconnected";
    case WL_IDLE_STATUS:
      return "Idle";
    default:
      return "Unknown";
  }
}

// --------------------------------------------------
// Pin Configuration
// --------------------------------------------------
#define DHT_PIN 4
#define DHT_TYPE DHT22

#define MQ_PIN 34
#define FLAME_PIN 27
#define BUZZER_PIN 25
#define RELAY_PIN 33

#define LED_GREEN 12
#define LED_YELLOW 14
#define LED_RED 26

// Flame sensor usually gives LOW when flame is detected.
const bool FLAME_ACTIVE_LOW = true;

// Sampling and reconnect intervals.
const unsigned long SAMPLE_INTERVAL_MS = 5000;
const unsigned long WIFI_RETRY_INTERVAL_MS = 15000;
unsigned long lastSampleMs = 0;
unsigned long lastWifiRetryMs = 0;

// Treat repeated near-zero smoke readings as a likely wiring/power fault.
const int SMOKE_ZERO_THRESHOLD = 15;
const int SMOKE_SENSOR_FAULT_CYCLES = 3;
int smokeZeroCycles = 0;

LiquidCrystal_I2C lcd(0x27, 16, 2);
DHT dht(DHT_PIN, DHT_TYPE);

struct SensorPacket {
  float temperature;
  float humidity;
  int smokeRaw;
  int smokeGasLevel;
  int flameDetected;
  bool smokeSensorFault;
  bool flameSensorFault;
};

void setLeds(bool green, bool yellow, bool red) {
  digitalWrite(LED_GREEN, green ? HIGH : LOW);
  digitalWrite(LED_YELLOW, yellow ? HIGH : LOW);
  digitalWrite(LED_RED, red ? HIGH : LOW);
}

void relayOn() {
  digitalWrite(RELAY_PIN, HIGH);
}

void relayOff() {
  digitalWrite(RELAY_PIN, LOW);
}

void buzzerBeep(int onMs, int offMs, int repeatCount) {
  for (int i = 0; i < repeatCount; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(onMs);
    digitalWrite(BUZZER_PIN, LOW);
    delay(offMs);
  }
}

int readSmokeAverage(int samples) {
  long total = 0;

  for (int i = 0; i < samples; i++) {
    total += analogRead(MQ_PIN);
    delay(10);
  }

  return (int)(total / samples);
}

void calibrateSmokeBaseline() {
  Serial.println("Warming up smoke sensor for baseline calibration...");
  unsigned long start = millis();
  while (millis() - start < SMOKE_WARMUP_MS) {
    delay(100);
  }

  long total = 0;
  for (int i = 0; i < SMOKE_BASELINE_SAMPLES; i++) {
    total += analogRead(MQ_PIN);
    delay(15);
  }

  smokeBaselineRaw = (float)total / (float)SMOKE_BASELINE_SAMPLES;
  if (smokeBaselineRaw < 0.0f) {
    smokeBaselineRaw = 0.0f;
  }
  smokeFilteredLevel = 0.0f;
  smokeCalibrated = true;

  Serial.print("Smoke baseline calibrated (raw): ");
  Serial.println((int)smokeBaselineRaw);
}

int normalizeSmokeLevel(int rawSmoke) {
  if (!smokeCalibrated) {
    smokeBaselineRaw = (float)rawSmoke;
    smokeFilteredLevel = 0.0f;
    smokeCalibrated = true;
  }

  float adjusted = (float)rawSmoke - smokeBaselineRaw;
  if (adjusted < 0.0f) {
    adjusted = 0.0f;
  }

  // Ignore low-level jitter near baseline.
  adjusted -= SMOKE_NOISE_FLOOR;
  if (adjusted < 0.0f) {
    adjusted = 0.0f;
  }

  // Allow slow baseline drift compensation in clean-air conditions.
  if (adjusted < (SMOKE_NOISE_FLOOR * 0.5f)) {
    smokeBaselineRaw = (SMOKE_BASELINE_DRIFT_ALPHA * smokeBaselineRaw)
                     + ((1.0f - SMOKE_BASELINE_DRIFT_ALPHA) * (float)rawSmoke);
  }

  smokeFilteredLevel = (SMOKE_FILTER_ALPHA * smokeFilteredLevel) + ((1.0f - SMOKE_FILTER_ALPHA) * adjusted);

  if (smokeFilteredLevel < 0.0f) {
    smokeFilteredLevel = 0.0f;
  }

  return (int)(smokeFilteredLevel + 0.5f);
}

int toPercent(float value, float maxValue) {
  if (maxValue <= 0.0f) {
    return 0;
  }

  float ratio = value / maxValue;
  if (ratio < 0.0f) {
    ratio = 0.0f;
  }
  if (ratio > 1.0f) {
    ratio = 1.0f;
  }

  return (int)(ratio * 100.0f + 0.5f);
}

bool isSameSubnet(IPAddress a, IPAddress b, IPAddress mask) {
  for (int i = 0; i < 4; i++) {
    if ((a[i] & mask[i]) != (b[i] & mask[i])) {
      return false;
    }
  }
  return true;
}

void logNetworkDiagnostics() {
  if (lastNetworkDiagLogMs != 0
      && millis() - lastNetworkDiagLogMs < NETWORK_DIAG_LOG_INTERVAL_MS) {
    return;
  }
  lastNetworkDiagLogMs = millis();

  IPAddress localIp = WiFi.localIP();
  IPAddress gatewayIp = WiFi.gatewayIP();
  IPAddress subnetMask = WiFi.subnetMask();

  Serial.print("Network diag - Local/Gateway/Mask: ");
  Serial.print(localIp);
  Serial.print(" / ");
  Serial.print(gatewayIp);
  Serial.print(" / ");
  Serial.println(subnetMask);

  IPAddress primaryHost;
  if (primaryHost.fromString(API_HOST_PRIMARY)) {
    Serial.print("Network diag - Primary host ");
    Serial.print(primaryHost);
    Serial.print(" same subnet: ");
    Serial.println(isSameSubnet(localIp, primaryHost, subnetMask) ? "YES" : "NO");
  }

  IPAddress altHost;
  if (altHost.fromString(API_HOST_ALT_LAN)) {
    Serial.print("Network diag - Alt host ");
    Serial.print(altHost);
    Serial.print(" same subnet: ");
    Serial.println(isSameSubnet(localIp, altHost, subnetMask) ? "YES" : "NO");
  }

  IPAddress hotspotHost;
  if (hotspotHost.fromString(API_HOST_FALLBACK)) {
    Serial.print("Network diag - Hotspot host ");
    Serial.print(hotspotHost);
    Serial.print(" same subnet: ");
    Serial.println(isSameSubnet(localIp, hotspotHost, subnetMask) ? "YES" : "NO");
  }
}

void calibrateFlameInactiveLevel() {
  int lowCount = 0;
  int highCount = 0;

  Serial.println("Calibrating flame sensor baseline...");
  for (int i = 0; i < FLAME_CALIBRATION_SAMPLES; i++) {
    int raw = digitalRead(FLAME_PIN);
    if (raw == LOW) {
      lowCount++;
    } else {
      highCount++;
    }
    delay(5);
  }

  if (highCount == lowCount) {
    flameInactiveLevel = FLAME_ACTIVE_LOW ? HIGH : LOW;
  } else {
    flameInactiveLevel = highCount > lowCount ? HIGH : LOW;
  }

  flameInactiveLevelCalibrated = true;

  Serial.print("Flame baseline LOW/HIGH samples: ");
  Serial.print(lowCount);
  Serial.print("/");
  Serial.println(highCount);
  Serial.print("Flame inactive level inferred as: ");
  Serial.println(flameInactiveLevel == LOW ? "LOW" : "HIGH");
}

int readStableFlameState(int samples) {
  int activeCount = 0;

  if (!flameInactiveLevelCalibrated) {
    flameInactiveLevel = FLAME_ACTIVE_LOW ? HIGH : LOW;
  }

  for (int i = 0; i < samples; i++) {
    int flameRaw = digitalRead(FLAME_PIN);
    bool active = flameRaw != flameInactiveLevel;
    if (active) {
      activeCount++;
    }
    delay(2);
  }

  return activeCount >= ((samples / 2) + 1) ? 1 : 0;
}

String localInitialWarning(const SensorPacket& packet) {
  if (packet.temperature == 0 && packet.humidity == 0 && packet.smokeGasLevel == 0 && packet.flameDetected == 0) {
    return "Sensor Fault";
  }

  if (packet.smokeRaw > 4000) {
    return "Sensor Malfunction";
  }

  if (packet.smokeSensorFault && packet.flameDetected == 0) {
    return "Smoke Sensor Fault";
  }

  if (packet.flameSensorFault) {
    return "Flame Sensor Fault";
  }

  // Safety-first: any stable flame trigger is treated as immediate risk.
  if (packet.flameDetected == 1) {
    return "Immediate Fire Alert";
  }

  if (packet.humidity < 30 && packet.temperature > 45 && packet.smokeGasLevel > 700) {
    return "Immediate Fire Alert";
  }

  if (packet.smokeGasLevel > 1200 && packet.temperature > 50) {
    return "Immediate Fire Alert";
  }

  if (packet.flameDetected == 0 && packet.temperature > 55 && packet.smokeGasLevel > 900 && packet.humidity < 40) {
    return "Electrical Fire Risk";
  }

  if (packet.flameDetected == 0 && packet.humidity > 85 && packet.smokeGasLevel >= 300 && packet.smokeGasLevel < 800 && packet.temperature < 50) {
    return "Steam Detected";
  }

  if (packet.flameDetected == 0 && packet.temperature > 50 && packet.smokeGasLevel < 300) {
    return "Heat Alert";
  }

  if (packet.smokeGasLevel >= 300 && packet.smokeGasLevel < 500) {
    return "Mild Smoke Alert";
  }

  if (packet.smokeGasLevel >= 500 && packet.smokeGasLevel < 800) {
    return "Smoke Warning Alert";
  }

  if (packet.smokeGasLevel >= 800 && packet.smokeGasLevel <= 1200) {
    return "Gas Leakage Alert";
  }

  return "No Immediate Alert";
}

void updateLCD(const SensorPacket& packet, const String& status) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("T:");
  lcd.print(packet.temperature, 1);
  lcd.print("C S:");
  lcd.print(packet.smokeGasLevel);

  lcd.setCursor(0, 1);
  if (status.length() > 16) {
    lcd.print(status.substring(0, 16));
  } else {
    lcd.print(status);
  }
}

void applyLocalFallback(const String& warning, const SensorPacket& packet) {
  if (warning == "Flame Sensor Fault") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(100, 140, 2);
    updateLCD(packet, "Flame SensorFlt");
    return;
  }

  if (warning == "Smoke Sensor Fault") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(120, 120, 2);
    updateLCD(packet, "Smoke SensorFlt");
    return;
  }

  if (warning == "Sensor Malfunction") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(110, 90, 2);
    updateLCD(packet, "Sensor Malfunc");
    return;
  }

  if (warning == "Sensor Fault") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(90, 90, 2);
    updateLCD(packet, "Sensor Fault");
    return;
  }

  if (warning == "No Immediate Alert") {
    setLeds(true, false, false);
    relayOff();
    digitalWrite(BUZZER_PIN, LOW);
    updateLCD(packet, "Safe");
    return;
  }

  if (warning == "Mild Smoke Alert") {
    setLeds(true, true, false);
    relayOff();
    buzzerBeep(80, 120, 1);
    updateLCD(packet, "Mild Smoke");
    return;
  }

  if (warning == "Smoke Warning Alert") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(120, 140, 2);
    updateLCD(packet, "Smoke Warning");
    return;
  }

  if (warning == "Gas Leakage Alert") {
    setLeds(false, true, true);
    relayOn();
    buzzerBeep(220, 130, 3);
    updateLCD(packet, "Gas Leakage");
    return;
  }

  if (warning == "Heat Alert") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(160, 140, 2);
    updateLCD(packet, "Heat Alert");
    return;
  }

  if (warning == "Steam Detected") {
    setLeds(true, true, false);
    relayOff();
    buzzerBeep(70, 130, 1);
    updateLCD(packet, "Steam Detected");
    return;
  }

  if (warning == "Electrical Fire Risk") {
    setLeds(false, true, true);
    relayOn();
    buzzerBeep(260, 120, 3);
    updateLCD(packet, "Elec Fire Risk");
    return;
  }

  if (warning == "Immediate Fire Alert") {
    setLeds(false, false, true);
    relayOn();
    buzzerBeep(450, 100, 3);
    updateLCD(packet, "Fire Emergency");
    return;
  }

  setLeds(false, true, false);
  relayOff();
  buzzerBeep(100, 80, 1);
  updateLCD(packet, "Unknown Status");
}

void applyServerDecision(const String& prediction, const SensorPacket& packet) {
  if (prediction == "Sensor Malfunction") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(110, 90, 2);
    updateLCD(packet, "Sensor Malfunc");
    return;
  }

  if (prediction == "Sensor Fault") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(90, 90, 2);
    updateLCD(packet, "Sensor Fault");
    return;
  }

  if (prediction == "False Alarm") {
    setLeds(true, false, false);
    relayOff();
    digitalWrite(BUZZER_PIN, LOW);
    updateLCD(packet, "False Alarm");
    return;
  }

  if (prediction == "Smoke Warning") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(120, 140, 2);
    updateLCD(packet, "Smoke Warning");
    return;
  }

  if (prediction == "Gas Leakage") {
    setLeds(false, true, true);
    relayOn();
    buzzerBeep(220, 130, 3);
    updateLCD(packet, "Gas Leakage");
    return;
  }

  if (prediction == "Heat Alert") {
    setLeds(false, true, false);
    relayOff();
    buzzerBeep(160, 140, 2);
    updateLCD(packet, "Heat Alert");
    return;
  }

  if (prediction == "Steam Detected") {
    setLeds(true, true, false);
    relayOff();
    buzzerBeep(70, 130, 1);
    updateLCD(packet, "Steam Detected");
    return;
  }

  if (prediction == "Electrical Fire Risk") {
    setLeds(false, true, true);
    relayOn();
    buzzerBeep(260, 120, 3);
    updateLCD(packet, "Elec Fire Risk");
    return;
  }

  if (prediction == "Fire Emergency") {
    setLeds(false, false, true);
    relayOn();
    buzzerBeep(450, 100, 3);
    updateLCD(packet, "Fire Emergency");
    return;
  }

  setLeds(false, true, false);
  relayOff();
  buzzerBeep(100, 80, 1);
  updateLCD(packet, "Unknown Status");
}

SensorPacket readSensors() {
  SensorPacket packet;

  packet.temperature = dht.readTemperature();
  packet.humidity = dht.readHumidity();
  packet.smokeRaw = readSmokeAverage(10);
  packet.smokeGasLevel = normalizeSmokeLevel(packet.smokeRaw);

  if (packet.smokeRaw <= SMOKE_ZERO_THRESHOLD) {
    smokeZeroCycles++;
  } else {
    smokeZeroCycles = 0;
  }

  packet.smokeSensorFault = smokeZeroCycles >= SMOKE_SENSOR_FAULT_CYCLES;

  packet.flameDetected = readStableFlameState(7);

  if (isnan(packet.temperature)) {
    packet.temperature = 0.0;
  }

  if (isnan(packet.humidity)) {
    packet.humidity = 0.0;
  }

  bool likelyFalseFlame = packet.flameDetected == 1
    && packet.smokeGasLevel <= FLAME_FAULT_MAX_SMOKE_LEVEL
    && packet.temperature < FLAME_FAULT_MAX_TEMP_C
    && packet.humidity > FLAME_FAULT_MIN_HUMIDITY;

  if (likelyFalseFlame) {
    flameSuspiciousCycles++;
  } else if (flameSuspiciousCycles > 0) {
    flameSuspiciousCycles--;
  }

  packet.flameSensorFault = flameSuspiciousCycles >= FLAME_FAULT_CONFIRM_CYCLES;

  if (packet.flameSensorFault) {
    packet.flameDetected = 0;
    if (millis() - lastFlameFaultLogMs > FLAME_FAULT_LOG_INTERVAL_MS) {
      lastFlameFaultLogMs = millis();
      Serial.println("Flame sensor appears stuck active under safe conditions. Marking as Flame Sensor Fault.");
    }
  }

  return packet;
}

void connectWifi() {
  if (usingPlaceholderCredentials()) {
    Serial.println("WiFi credentials are placeholders. Update WIFI_SSID and WIFI_PASSWORD in firmware.");
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
  while (WiFi.status() != WL_CONNECTED && millis() - startMs < WIFI_CONNECT_TIMEOUT_MS) {
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
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

void ensureWifiConnected() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  if (millis() - lastWifiRetryMs < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastWifiRetryMs = millis();
  if (usingPlaceholderCredentials()) {
    Serial.println("WiFi retry skipped: placeholder credentials.");
    return;
  }

  Serial.print("WiFi disconnected. Retrying connection. Last status: ");
  wl_status_t status = (wl_status_t)WiFi.status();
  Serial.println(wifiStatusToText(status));
  printWifiFailureHints(status);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool postToBackend(const SensorPacket& packet, String& predictionOut) {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }

  logNetworkDiagnostics();

  IPAddress localIp = WiFi.localIP();
  IPAddress subnetMask = WiFi.subnetMask();
  IPAddress primaryHost;
  bool primarySameSubnet = false;
  if (primaryHost.fromString(API_HOST_PRIMARY)) {
    primarySameSubnet = isSameSubnet(localIp, primaryHost, subnetMask);
  }

  StaticJsonDocument<256> payload;
  payload["device_id"] = DEVICE_ID;
  payload["temperature"] = packet.temperature;
  payload["humidity"] = packet.humidity;
  payload["smoke_gas_level"] = packet.smokeGasLevel;
  payload["flame_detected"] = packet.flameDetected;
  payload["flame_sensor_fault"] = packet.flameSensorFault;

  String requestBody;
  serializeJson(payload, requestBody);

  String endpoints[5];
  int endpointCount = 0;
  endpoints[endpointCount++] = String(API_URL_PRIMARY);
  if (String(API_URL_ALT_LAN) != endpoints[0]) {
    endpoints[endpointCount++] = String(API_URL_ALT_LAN);
  }
  if (endpoints[0] != String(API_URL_FALLBACK)) {
    endpoints[endpointCount++] = String(API_URL_FALLBACK);
  }

  String gatewayHost = WiFi.gatewayIP().toString();
  if (gatewayHost != "0.0.0.0") {
    String gatewayUrl = "http://" + gatewayHost + ":8000/api/v1/events/predict";
    bool alreadyPresent = false;
    for (int i = 0; i < endpointCount; i++) {
      if (endpoints[i] == gatewayUrl) {
        alreadyPresent = true;
        break;
      }
    }
    if (!alreadyPresent) {
      endpoints[endpointCount++] = gatewayUrl;
    }
  }

  int lastHttpCode = -999;
  String lastTriedEndpoint = "";

  for (int endpointIndex = 0; endpointIndex < endpointCount; endpointIndex++) {
    const char* activeApiUrl = endpoints[endpointIndex].c_str();
    Serial.print("Trying backend endpoint: ");
    Serial.println(activeApiUrl);

    for (int attempt = 1; attempt <= BACKEND_POST_RETRY_COUNT; attempt++) {
      HTTPClient http;
      http.begin(activeApiUrl);
      http.addHeader("Content-Type", "application/json");
      http.setTimeout(BACKEND_HTTP_TIMEOUT_MS);

      int httpCode = http.POST(requestBody);
      lastHttpCode = httpCode;
      lastTriedEndpoint = String(activeApiUrl);
      if (httpCode >= 200 && httpCode < 300) {
        String response = http.getString();
        http.end();

        StaticJsonDocument<1024> responseJson;
        DeserializationError err = deserializeJson(responseJson, response);
        if (err) {
          Serial.println("Backend response parse failed.");
          return false;
        }

        const char* prediction = responseJson["event"]["prediction"];
        if (prediction == nullptr) {
          prediction = responseJson["prediction"];
        }
        if (prediction == nullptr) {
          return false;
        }

        predictionOut = String(prediction);
        return true;
      }

      Serial.print("Backend POST failed (endpoint ");
      Serial.print(endpointIndex + 1);
      Serial.print("/");
      Serial.print(endpointCount);
      Serial.print(", attempt ");
      Serial.print(attempt);
      Serial.print("/");
      Serial.print(BACKEND_POST_RETRY_COUNT);
      Serial.print("). HTTP code: ");
      Serial.println(httpCode);
      if (httpCode > 0) {
        String errorBody = http.getString();
        if (errorBody.length() > 0) {
          Serial.print("Backend error body: ");
          Serial.println(errorBody);
        }
      } else {
        String transportError = HTTPClient::errorToString(httpCode);
        Serial.print("Transport detail: ");
        Serial.println(transportError);
        Serial.print("WiFi IP/Gateway: ");
        Serial.print(WiFi.localIP());
        Serial.print(" / ");
        Serial.println(WiFi.gatewayIP());

        if (endpointIndex == 0 && primarySameSubnet) {
          Serial.println("Likely root cause: host firewall block or AP client isolation (same subnet but TCP connect fails).");
          Serial.println("Action: allow inbound TCP 8000 on Windows and use Private WiFi profile.");
        }

        // Transport-level failure (e.g. -1). Reconnect WiFi before next retry.
        Serial.println("Transport error. Attempting WiFi recovery before retry...");
        WiFi.disconnect(false, false);
        delay(120);
        WiFi.reconnect();
        delay(BACKEND_NEGATIVE_CODE_WIFI_RECOVERY_DELAY_MS);
      }
      http.end();

      if (attempt < BACKEND_POST_RETRY_COUNT) {
        delay(BACKEND_POST_RETRY_DELAY_MS);
      }
    }
  }

  if (millis() - lastBackendReachabilityLogMs > BACKEND_REACHABILITY_LOG_INTERVAL_MS) {
    lastBackendReachabilityLogMs = millis();
    Serial.println("Backend host unreachable from ESP32 after trying all endpoints.");
    Serial.print("Last tried endpoint: ");
    Serial.println(lastTriedEndpoint);
    Serial.print("Last HTTP code: ");
    Serial.println(lastHttpCode);
    Serial.println("Hint: Ensure backend is running and Windows firewall allows TCP 8000 on Private network.");
    Serial.println("Hint: Ensure ESP32 and backend host are on the same subnet, or use hotspot with 192.168.137.1.");
  }

  return false;
}

void setup() {
  Serial.begin(115200);
  Serial.println("System booting...");
  Serial.print("Device ID: ");
  Serial.println(DEVICE_ID);
  Serial.print("Primary API URL: ");
  Serial.println(API_URL_PRIMARY);
  Serial.print("Alt LAN API URL: ");
  Serial.println(API_URL_ALT_LAN);
  Serial.print("Fallback API URL: ");
  Serial.println(API_URL_FALLBACK);

  pinMode(FLAME_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);

  analogReadResolution(12);
  analogSetPinAttenuation(MQ_PIN, ADC_11db);

  digitalWrite(BUZZER_PIN, LOW);
  relayOff();
  setLeds(true, false, false);

  dht.begin();

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Fire System");
  lcd.setCursor(0, 1);
  lcd.print("Starting...");

  connectWifi();
  calibrateSmokeBaseline();
  calibrateFlameInactiveLevel();
}

void loop() {
  if (millis() - lastSampleMs < SAMPLE_INTERVAL_MS) {
    delay(50);
    return;
  }

  lastSampleMs = millis();
  ensureWifiConnected();

  SensorPacket packet = readSensors();

  Serial.println("----------------------------");
  Serial.print("Temperature: ");
  Serial.println(packet.temperature);
  Serial.print("Humidity: ");
  Serial.println(packet.humidity);
  Serial.print("Smoke/Gas Level: ");
  Serial.println(packet.smokeGasLevel);
  Serial.print("Smoke Raw: ");
  Serial.println(packet.smokeRaw);
  Serial.print("Smoke Raw (%ADC): ");
  Serial.println(toPercent((float)packet.smokeRaw, 4095.0f));
  Serial.print("Smoke Baseline Raw: ");
  Serial.println((int)smokeBaselineRaw);
  Serial.print("Smoke Baseline (%ADC): ");
  Serial.println(toPercent(smokeBaselineRaw, 4095.0f));
  Serial.print("Flame Detected: ");
  Serial.println(packet.flameDetected);
  Serial.print("Smoke Sensor Fault: ");
  Serial.println(packet.smokeSensorFault ? "YES" : "NO");
  Serial.print("Flame Sensor Fault: ");
  Serial.println(packet.flameSensorFault ? "YES" : "NO");
  Serial.print("WiFi: ");
  Serial.println(wifiStatusToText((wl_status_t)WiFi.status()));

  String warning = localInitialWarning(packet);
  Serial.print("Initial Warning: ");
  Serial.println(warning);

  String prediction;
  bool sent = postToBackend(packet, prediction);

  if (sent) {
    Serial.println("Backend: Connected");
    Serial.print("Server Prediction: ");
    Serial.println(prediction);
    applyServerDecision(prediction, packet);
  } else {
    Serial.println("Backend: Unavailable");
    Serial.println("Backend unavailable. Using local logic.");
    applyLocalFallback(warning, packet);
  }
}