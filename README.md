# Macro Project 1 - AI Fire and Gas Monitoring Platform

## Project Topic

AI-Enabled IoT-Based Intelligent Fire Detection and Early Warning System

This is a full-stack prototype with ESP32 telemetry, FastAPI inference, SQLite event storage, and a Streamlit command dashboard.

## Current System Capabilities

- Hybrid detection: Decision Tree prediction plus deterministic safety rules.
- Priority fire logic:
  - flame_detected == 1 and smoke_gas_level > 500 -> Fire Emergency
  - humidity < 30 and temperature > 45 and smoke_gas_level > 700 -> Fire Emergency
  - smoke_gas_level > 1200 and temperature > 50 -> Fire Emergency
- Progressive smoke ladder:
  - 300-499 -> Mild Smoke Alert
  - 500-799 -> Smoke Warning Alert
  - 800-1200 -> Gas Leakage Alert
- Industrial context labels:
  - Heat Alert
  - Steam Detected
  - Electrical Fire Risk
  - Sensor Fault
  - Sensor Malfunction
- Confidence score support from model prediction paths.
- Severity score and severity band per event.
- Duplicate event prevention to avoid DB flooding.
- Backend degraded mode if model loading fails.

## Architecture

1. ESP32 reads sensors and generates local warning logic.
2. ESP32 posts telemetry payload to backend API.
3. Backend validates sensor values and checks model availability.
4. Backend computes prediction, confidence, severity score, and severity band.
5. Backend skips duplicate events based on device, prediction, smoke bucket, and recent window.
6. Backend stores event and notification queue records in SQLite.
7. Streamlit dashboard renders KPIs, colorful charts, high-priority feed, and styled table.

## Main Files

- app.py: Streamlit dashboard (charts + styled event table)
- backend_api.py: FastAPI endpoints, validation, logging, duplicate prevention, severity scoring
- database.py: SQLite schema, migrations, queries, duplicate lookup helper
- fire_detection.py: training, evaluation, decision logic, confidence output, exported rules
- test_model.py: regression tests for rule logic and prediction behavior
- esp32_firmware.ino: ESP32 firmware logic and fallback behaviors
- esp32_decision_rules.txt: exported rule text for firmware reference
- esp32_payload_example.json: sample ESP32 payload
- requirements.txt: Python dependencies

## Rule-Enriched Labels

Core model classes:

- False Alarm
- Smoke Warning
- Gas Leakage
- Fire Emergency

Additional operational labels:

- Heat Alert
- Steam Detected
- Electrical Fire Risk
- Sensor Fault
- Sensor Malfunction

## Sensor Integrity Logic

- Sensor Fault when all primary signals are zero:
  - temperature == 0 and humidity == 0 and smoke_gas_level == 0 and flame_detected == 0
- Sensor Malfunction when smoke is abnormally high:
  - smoke_gas_level > 4000

## Severity Score

Score formula:

- +30 if smoke_gas_level > 500
- +30 if temperature > 45
- +40 if flame_detected == 1

Severity bands:

- 0-20: Safe
- 21-50: Warning
- 51-80: High Risk
- 81-100: Critical

## Duplicate Event Prevention

Duplicate insert is skipped when all conditions match:

- Same device_id
- Same prediction
- Same smoke bucket (bucket size = 100)
- Within last 30 seconds

API response includes:

- notification_status = skipped_duplicate
- duplicate_skipped = true

## Model Versioning

- Saved model naming pattern: fire_alert_model_<version>.pkl
- Default version: v1
- Backend model version loaded via MODEL_VERSION environment variable
- Legacy fallback still supports fire_alert_model.pkl when versioned file is absent

## Training and Evaluation

Training includes:

- Stratified train/test split
- Sample weighting for high-risk fire patterns

Evaluation output includes:

- Accuracy
- Classification report
- Confusion matrix
- Depth comparison for max_depth values: 5, 7, 10

## API Endpoints

- GET /health
- POST /api/v1/events/predict
- GET /api/v1/events?limit=200
- GET /api/v1/stats

## Example Predict Payload

URL: http://<your-computer-ip>:8000/api/v1/events/predict

Method: POST

Content-Type: application/json

```json
{
  "device_id": "esp32-room-a",
  "temperature": 38.4,
  "humidity": 44.1,
  "smoke_gas_level": 610,
  "flame_detected": 0
}
```

## Run Steps (Windows PowerShell)

1. Install dependencies

```powershell
python -m venv .venv
.\.venv\Scripts\activate
python -m pip install --upgrade pip
pip install -r requirements.txt
```

2. Train model and export rules

```powershell
python fire_detection.py
```

This saves a versioned model file, for example fire_alert_model_v1.pkl.

3. Start backend API

```powershell
$env:MODEL_VERSION = "v1"
uvicorn backend_api:app --host 0.0.0.0 --port 8000 --reload
```

4. Start dashboard

```powershell
streamlit run app.py
```

Dashboard live updates:

- Use sidebar `Auto Refresh` (enabled by default).
- Set `Refresh Interval (seconds)` to match ESP32 sampling (default 5 seconds).

5. Configure and flash ESP32 firmware

- Open `esp32_firmware.ino` in Arduino IDE.
- Set `WIFI_SSID`, `WIFI_PASSWORD`, and `API_URL` to your local network values.
- Install Arduino libraries: `ArduinoJson`, `DHT sensor library`, and `LiquidCrystal I2C`.
- Select your ESP32 board and COM port, then upload.
- Open Serial Monitor at 115200 baud to confirm telemetry and fallback logs.

## Backend Logging and Reliability

- Startup info logs for DB initialization and model load.
- Warning logs for high smoke levels and sensor validation issues.
- Error logs for pipeline failures.
- Graceful degraded mode if model fails to load.

## ESP32 Firmware Notes

- Local fallback logic mirrors backend safety priorities and sensor fault/malfunction logic.
- Firmware maps server prediction labels to LED/buzzer/relay behavior.
- If backend is unreachable, firmware continues local warning responses.
- Dashboard now also supports local database fallback if backend is offline.

## Hardware Wiring (ESP32)

Use the default pin mapping from `esp32_firmware.ino`:

- DHT22 data pin -> GPIO4
- MQ analog output -> GPIO34
- Flame digital output -> GPIO27
- Buzzer -> GPIO25
- Relay IN -> GPIO33
- Green LED -> GPIO12
- Yellow LED -> GPIO14
- Red LED -> GPIO26

Power notes:

- Use 3.3V-compatible sensor outputs for ESP32 GPIO pins.
- Use a transistor or relay driver for high-current loads (siren/fan).
- Keep grounds common between ESP32, sensors, and relay module.

## Offline Operation Behavior

- Backend reachable: ESP32 posts data and follows backend prediction response.
- Backend unreachable: ESP32 logs `Backend unavailable. Using local logic.` and drives relay, buzzer, LEDs, and LCD from on-device rules.
- Dashboard offline mode: if API is down, Streamlit reads local `macro_alerts.db` history instead of stopping.

## Connection Verification Checklist

Use Serial Monitor to verify wiring and software path:

- `WiFi: Connected` confirms ESP32 network connection.
- `Backend: Connected` confirms API reachability from ESP32.
- `Smoke Sensor Fault: YES` means repeated near-zero MQ readings and likely wiring/power issue.
- `Flame Detected: 1` now triggers `Initial Warning: Immediate Fire Alert` as safety-first behavior.

## Recommended Hardware Components

Required:

- ESP32 dev board
- MQ smoke/gas sensor (for example MQ-2 or MQ-135)
- DHT11 or DHT22
- Flame sensor module
- Buzzer
- LED indicators (red/yellow/green)

Strongly recommended:

- Relay module for siren/exhaust fan
- 5V active siren
- ADS1115 ADC module
- GSM module (for SMS fallback)
- Battery backup and proper regulator/isolation
- Enclosure with ventilation

## Next Production Upgrades

- Auth and role-based access for dashboard/backend
- Real notification integrations (SMS, email, incident APIs)
- Camera confirmation module and visual model fusion
- Scheduled retraining pipeline with production telemetry
