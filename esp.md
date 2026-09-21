# ESP32 Firmware Documentation
## Wireless 3-Phase Induction Motor Drive System

---

## System Architecture

```
┌─────────────────────────────┐          ┌──────────────────────────────────────┐
│   TRANSMITTER  (ESP32 #1)   │          │    RECEIVER  (ESP32 #2)              │
│                             │          │                                      │
│  ┌──────────┐  ┌─────────┐  │  LoRa /  │  ┌─────────┐  ┌──────────────────┐  │
│  │ LCD 16x2 │  │ Buttons │  │◄────────►│  │LoRa/WiFi│  │ DOL Starter      │  │
│  └──────────┘  └─────────┘  │  WiFi /  │  └─────────┘  │ START solenoid   │  │
│                             │  ESP-NOW  │               │ STOP  solenoid   │  │
│  Acts as HTTP client:       │          │               └──────────────────┘  │
│  Sends ON/OFF commands ──────────────────────────────► Receives commands      │
│                             │          │                                      │
│  Displays sensor data  ◄────────────────────────────── Sends sensor readings  │
└─────────────────────────────┘          │                                      │
                                         │  Sensors fitted on receiver side:    │
       ┌─────────────────┐               │  • SW-420 Vibration (motor status)   │
       │   Mobile App    │    HTTP/WiFi  │  • ZMPT101B × 3 (phase voltages)     │
       │ (Android phone) │◄─────────────►│  • ACS712 (output current)           │
       └─────────────────┘               │  • Flow sensor at pipe end           │
                                         └──────────────────────────────────────┘
```

---

## Inter-ESP32 Communication Options

Three methods are supported — choose based on distance and infrastructure:

### Option 1 — LoRa (Recommended for rural / long range)
- **Modules**: SX1278 / Ra-02 on 433 MHz
- **Range**: 1–5 km line-of-sight
- **No infrastructure needed** — works without WiFi or mobile network
- **Library**: `LoRa` by Sandeep Mistry
- ESP32 #1 sends command packets; ESP32 #2 replies with sensor JSON

### Option 2 — ESP-NOW (Short range, ultra-low latency)
- **Built into ESP32** — no extra hardware
- **Range**: ~200 m (clear line of sight), ~50 m through walls
- **Library**: `esp_now.h` (built into Arduino ESP32 core)
- Peer-to-peer, encrypted, ~1 ms latency

### Option 3 — WiFi (Station mode, via router)
- Both ESP32s connect to the same WiFi network
- ESP32 #2 (receiver) runs the HTTP server
- ESP32 #1 (transmitter / app) connects as a client
- Range limited by router coverage; works anywhere on the LAN

> **Note**: For global internet access (server mode in the app), ESP32 #2 connects to home WiFi and posts sensor data to your server. The Android app then reads from that server regardless of its location.

---

## ESP32 #2 (Receiver) — HTTP Server Endpoints

The receiver runs a lightweight HTTP server on port **80**.

### `GET /api/status`
Returns the current motor and sensor state.

**Response:**
```json
{
  "motorOn":   true,
  "voltageR":  231.5,
  "voltageY":  229.8,
  "voltageB":  232.1,
  "current":   4.2,
  "vibration": true,
  "waterFlow": true
}
```

| Field       | Type    | Description                                    |
|-------------|---------|------------------------------------------------|
| `motorOn`   | bool    | Whether the DOL contactor is currently closed  |
| `voltageR`  | float   | Phase R voltage in Volts (from ZMPT101B)       |
| `voltageY`  | float   | Phase Y voltage in Volts                       |
| `voltageB`  | float   | Phase B voltage in Volts                       |
| `current`   | float   | Output current in Amperes (from ACS712)        |
| `vibration` | bool    | `true` = motor vibrating (running)             |
| `waterFlow` | bool    | `true` = flow detected at pipe end             |

> Omit any field if that sensor is not fitted — the app shows `—` for missing values.

---

### `POST /api/motor/on`
Triggers the START solenoid (energises DOL contactor).

**Response:**
```json
{ "success": true }
```

---

### `POST /api/motor/off`
Triggers the STOP solenoid (de-energises DOL contactor).

**Response:**
```json
{ "success": true }
```

---

### `GET /api/alerts`
Returns events that were stored in **non-volatile memory** (ESP32 Preferences / EEPROM)
while the app was offline — e.g. power cuts detected.

**Response:**
```json
{
  "alerts": [
    {
      "type":      "power_loss",
      "timestamp": 1718000000,
      "message":   "Power lost at 14:32"
    },
    {
      "type":      "phase_fault",
      "timestamp": 1718001234,
      "message":   "Phase R missing — undervoltage at 14:52"
    }
  ]
}
```

| `type` value  | Meaning                             |
|---------------|-------------------------------------|
| `power_loss`  | All 3 phases lost (power cut)       |
| `phase_fault` | One or two phases missing / low     |
| `overload`    | Current exceeded threshold          |
| `custom`      | Any user-defined event              |

> **Timestamp**: use `millis()` relative offset if no RTC is fitted. Unix epoch is preferred if you add a DS3231 RTC or sync via NTP.

---

### `POST /api/alerts/clear`
Erases all stored alerts from non-volatile memory after the app acknowledges them.

**Response:**
```json
{ "success": true }
```

---

## Alert Storage — Arduino Implementation

```cpp
#include <Preferences.h>

Preferences prefs;

// Call this when a power-loss event is detected
void saveAlert(const char* type, const char* message) {
    prefs.begin("alerts", false);

    int count = prefs.getInt("count", 0);
    if (count >= 10) count = 0; // ring buffer, max 10 alerts

    String key = "msg" + String(count);
    String val = String(type) + "|" + String(millis() / 1000) + "|" + message;
    prefs.putString(key.c_str(), val.c_str());
    prefs.putInt("count", count + 1);

    prefs.end();
}

// Detect power loss: monitor voltage sensors; if all 3 drop to 0 → save alert
void checkPowerLoss() {
    float vR = readVoltageR();
    float vY = readVoltageY();
    float vB = readVoltageB();

    if (vR < 10.0 && vY < 10.0 && vB < 10.0) {
        saveAlert("power_loss", "All phases lost — possible power cut");
    } else if (vR < 10.0) {
        saveAlert("phase_fault", "Phase R missing — undervoltage");
    }
    // Add Y and B checks similarly
}

// Serve GET /api/alerts — build JSON from stored prefs
String buildAlertsJson() {
    prefs.begin("alerts", true);
    int count = prefs.getInt("count", 0);

    String json = "{\"alerts\":[";
    for (int i = 0; i < count; i++) {
        String key = "msg" + String(i);
        String val = prefs.getString(key.c_str(), "");
        if (val.length() == 0) continue;

        // Parse "type|timestamp|message"
        int p1 = val.indexOf('|');
        int p2 = val.indexOf('|', p1 + 1);
        String type  = val.substring(0, p1);
        String ts    = val.substring(p1 + 1, p2);
        String msg   = val.substring(p2 + 1);

        if (i > 0) json += ",";
        json += "{\"type\":\"" + type + "\","
                "\"timestamp\":" + ts + ","
                "\"message\":\"" + msg + "\"}";
    }
    json += "]}";
    prefs.end();
    return json;
}

// Handle POST /api/alerts/clear
void clearAlerts() {
    prefs.begin("alerts", false);
    prefs.clear();
    prefs.end();
}
```

---

## Solenoid Wiring (Receiver Side)

```
ESP32 GPIO → MOSFET Gate (IRF520)
MOSFET Drain → Solenoid (–)
Solenoid (+) → 12V supply (+)
12V supply (–) → MOSFET Source → GND (common with ESP32 GND)
Flyback diode (1N4007) across solenoid terminals
```

| GPIO | Function           | Solenoid   |
|------|--------------------|------------|
| 25   | START pulse (100 ms) | DOL START button |
| 26   | STOP  pulse (100 ms) | DOL STOP  button |

```cpp
#define PIN_START 25
#define PIN_STOP  26

void motorOn() {
    digitalWrite(PIN_START, HIGH);
    delay(120);               // 120 ms pulse — enough to latch contactor
    digitalWrite(PIN_START, LOW);
}

void motorOff() {
    digitalWrite(PIN_STOP, HIGH);
    delay(120);
    digitalWrite(PIN_STOP, LOW);
}
```

---

## Sensor Pin Reference

| Sensor              | Model     | ESP32 Pin | Notes                         |
|---------------------|-----------|-----------|-------------------------------|
| Phase R voltage     | ZMPT101B  | GPIO 34   | Analog, use voltage divider   |
| Phase Y voltage     | ZMPT101B  | GPIO 35   | Analog                        |
| Phase B voltage     | ZMPT101B  | GPIO 32   | Analog                        |
| Output current      | ACS712    | GPIO 33   | Analog, 2.5V midpoint         |
| Vibration / status  | SW-420    | GPIO 27   | Digital, INPUT_PULLUP         |
| Water flow (pipe)   | Flow/reed | GPIO 14   | Digital, long wire OK (twisted pair) |

---

## Minimal Arduino Sketch Outline (Receiver)

```cpp
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
// Add: #include <LoRa.h>   if using LoRa instead of WiFi

WebServer server(80);

void setup() {
    // --- WiFi soft-AP mode (phone connects directly) ---
    WiFi.softAP("MotorDrive", "12345678");

    // --- OR station mode (connect to home router) ---
    // WiFi.begin("YourSSID", "YourPassword");

    server.on("/api/status",       HTTP_GET,  handleStatus);
    server.on("/api/motor/on",     HTTP_POST, handleMotorOn);
    server.on("/api/motor/off",    HTTP_POST, handleMotorOff);
    server.on("/api/alerts",       HTTP_GET,  handleAlerts);
    server.on("/api/alerts/clear", HTTP_POST, handleClearAlerts);
    server.begin();

    pinMode(PIN_START, OUTPUT);
    pinMode(PIN_STOP,  OUTPUT);
    pinMode(27, INPUT_PULLUP); // SW-420 vibration
    pinMode(14, INPUT_PULLUP); // water flow
}

void loop() {
    server.handleClient();
    checkPowerLoss();   // call every loop iteration
}

void handleStatus() {
    String json = "{";
    json += "\"motorOn\":"   + String(motorState ? "true" : "false") + ",";
    json += "\"voltageR\":"  + String(readVoltage(34), 1) + ",";
    json += "\"voltageY\":"  + String(readVoltage(35), 1) + ",";
    json += "\"voltageB\":"  + String(readVoltage(32), 1) + ",";
    json += "\"current\":"   + String(readCurrent(33), 2) + ",";
    json += "\"vibration\":" + String(!digitalRead(27) ? "true" : "false") + ",";
    json += "\"waterFlow\":" + String(!digitalRead(14) ? "true" : "false");
    json += "}";
    server.send(200, "application/json", json);
}
```

---

## Default App Settings

| Setting          | Default          |
|------------------|------------------|
| ESP32 IP         | `192.168.4.1`    |
| Port             | `80`             |
| WiFi SSID        | `MotorDrive`     |
| WiFi Password    | `12345678`       |
| Poll interval    | `3` seconds      |
| Connection mode  | Direct Wi-Fi     |
