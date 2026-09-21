# Motor Drive — Feature Reference

> **Covers:** Android app (`com.motordrive.esp32`) + Sender ESP8266 firmware v2 + Receiver ESP8266 firmware v2.

---

## Architecture overview

```
Android App
    │  HTTP/JSON over Wi-Fi (AP: MotorControl)
    ▼
Sender ESP8266          ─── ESP-NOW broadcast ───►  Receiver ESP8266
  • Wi-Fi AP                                           • Relay (motor)
  • REST API server                                    • Servo (physical button)
  • EEPROM (512 B)                                     • ACS712 current (EMA)
  • Timer / Scheduler                                  • Water sensor
  • Log ring-buffer (30 × 90 chars)                   • Stall detection
  • Optional DS3231 RTC (dead code by default)         • EEPROM (512 B)
                                                       • Optional DS3231 RTC (dead code)
```

---

## App — Navigation

The app has **three tabs** in a bottom navigation bar:

| Tab | Label | Icon | Purpose |
|-----|-------|------|---------| 
| 1 | **Control** | ⚙ dial | Live dashboard — motor ON/OFF, sensor readings |
| 2 | **Timer** | ⏳ hourglass | Stop-timer with countdown |
| 3 | **Schedule** | 📅 calendar | Time-based start/stop scheduler |

Settings (connection IP, poll interval, sensor toggles, notifications, serial monitor) remain accessible via the ⋮ gear icon in the Control tab toolbar.

---

## Tab 1 — Control (Dashboard)

- **Motor ON / OFF** buttons with visual banner and progress indicator.
- **Stall warning** — banner turns orange when motor is ON but current < 0.1 A for 5 s.
- **Connection chip** — green = connected; shows `(RF down)` if ESP-NOW link to receiver is lost.
- **Sensor modules** (each toggleable in Settings):
  - Module A — Three-phase voltage (R/Y/B)
  - Module B — ACS712 current reading (A)
  - Module C — Pipe-end water sensor (WATER / DRY / Checking…)
- **Notifications** (toggleable in Settings):
  - Persistent shade notification — ongoing motor state, no sound.
  - Alert on state change — sound + vibration when motor turns ON or OFF.
- **Serial Monitor** (in Settings) — fetches the Sender's 30-entry log ring-buffer via `GET /api/logs`.
- **App Log** (in Settings) — live in-app event log, exportable as plain text.

---

## Tab 2 — Timer (Stop-timer)

Set a countdown after which the motor is automatically switched **OFF**.

### App side

1. Enter duration as **HH : MM : SS** (any combination, e.g. 0h 30m 0s = 1800 s).
2. Toggle **Auto-restart after power cut** (see below).
3. Tap **START TIMER** → sends `POST /api/timer/set` with `{seconds, autoRestart}`.
4. Countdown is displayed in real time (polled from `GET /api/timer/status` each poll cycle).
5. Tap **CANCEL** → sends `POST /api/timer/cancel`; timer stops and EEPROM is cleared.

### Firmware side (Sender + Receiver)

- Both ESPs store the timer in **EEPROM** (`EE_TIMER_*` addresses) on every `set` / `cancel` call.
- The remaining time is checkpointed to EEPROM **every 10 seconds** while running.
- When the countdown reaches zero → `Motor OFF` is issued and EEPROM is updated.
- The Sender forwards the timer config to the Receiver via **ESP-NOW packet type `0x04`** (`TimerPacket`). The Receiver runs its own parallel countdown so that if the Sender reboots, the Receiver still cuts the motor on time.

### Auto-restart after power cut

When **Auto-restart is ON** and power is lost while the timer is active:

1. On reboot, both ESPs load EEPROM → see `timerActive=1`, `motorWasOn=1`, `autoRestart=1`.
2. The remaining seconds (last checkpoint, up to 10 s stale) are restored.
3. Motor is re-activated automatically, countdown resumes from the checkpointed value.

---

## Tab 3 — Scheduler

Create up to **8 start/stop schedule entries**, each tied to specific days of the week.

### RTC toggle

| Switch position | Behaviour |
|-----------------|-----------|
| **OFF** (default) | App sends phone's current time to ESP every **30 seconds** via `POST /api/time/sync`. Firmware advances the epoch internally between syncs. |
| **ON** | Firmware reads time from a **DS3231 RTC module** over I²C. The app still sends an initial sync so the DS3231 is set correctly; afterwards the firmware is fully autonomous. |

> **To enable RTC hardware:** uncomment `#define HAS_RTC_HARDWARE` at the top of both `.ino` files and wire the DS3231:
> - **Sender:** SDA=D2/GPIO4, SCL=D1/GPIO5 *(shared with buttons — remove buttons when RTC is installed)*
> - **Receiver:** SDA=D6/GPIO12, SCL=D5/GPIO14

### Adding a schedule entry

1. Tap **+ ADD SCHEDULE**.
2. Pick **Start time** and **Stop time** using the time picker.
3. Select **repeat days** (Su Mo Tu We Th Fr Sa checkboxes).
4. Toggle **Auto-restart** for this entry.
5. Tap **Add** → entry is pushed to both ESPs via `POST /api/schedule/push`.

### Firmware side

- Sender and Receiver each run their own **scheduler tick every 15 seconds**, comparing current minute-of-day against each enabled entry.
- On a **start minute match** → Motor ON.
- On a **stop minute match** → Motor OFF.
- Both devices deduplicate by tracking the last acted minute.

---

## EEPROM layout (both ESPs, 512 B)

| Address | Size | Field |
|---------|------|-------|
| 0 | 1 B | Magic byte (`0xBE`). If absent, all defaults are used. |
| 1–4 | 4 B | Timer total seconds (uint32) |
| 5 | 1 B | Timer `autoRestart` flag |
| 6 | 1 B | Timer `wasActive` flag |
| 7–10 | 4 B | Timer remaining seconds (checkpointed every 10 s) |
| 11 | 1 B | RTC enabled flag |
| 12–15 | 4 B | Last known Unix epoch |
| 16 | 1 B | Schedule count (0–8) |
| 17–80 | 64 B | Schedule entries × 8 (8 B each: startH, startM, stopH, stopM, days, autoRestart, enabled, pad) |
| 81 | 1 B | `motorWasOn` — updated every time motor state changes |

---

## REST API — complete endpoint list

All endpoints are served by the **Sender ESP8266** at `http://192.168.4.1:80` (default).

| Method | Path | Body / Response | Description |
|--------|------|-----------------|-------------|
| GET | `/api/status` | `{motorOn, current, isRunning, waterOk, stall, linkOk}` | Live status |
| POST | `/api/motor/on` | `{success:true}` | Turn motor ON |
| POST | `/api/motor/off` | `{success:true}` | Turn motor OFF |
| GET | `/api/logs` | `{logs:[…]}` | Sender log ring-buffer (30 entries) |
| GET | `/api/alerts` | `{alerts:[]}` | Pending alert list (stub) |
| POST | `/api/alerts/clear` | `{success:true}` | Clear alert list |
| POST | `/api/timer/set` | `{seconds:N, autoRestart:0\|1}` → `{success:true}` | Set stop-timer |
| POST | `/api/timer/cancel` | — → `{success:true}` | Cancel active timer |
| GET | `/api/timer/status` | `{active, remaining, total, autoRestart}` | Timer state |
| POST | `/api/schedule/push` | `{count:N, entries:[{slot,startH,startM,stopH,stopM,days,autoRestart,enabled}]}` → `{success:true}` | Push full schedule |
| POST | `/api/schedule/clear` | — → `{success:true}` | Clear all schedules |
| POST | `/api/time/sync` | `{epoch:N, rtcEnabled:0\|1}` → `{success:true}` | Sync phone time |

### Status response field reference

| Field | Type | Meaning |
|-------|------|---------|
| `motorOn` | bool | Relay state (commanded by sender or receiver countdown) |
| `current` | float | ACS712 EMA reading in Amps |
| `isRunning` | bool | `motorOn` mirrored (same value) |
| `waterOk` | bool | `true` = water confirmed at pipe end, OR motor is off (no active check). `false` = motor ON but waiting for flow (30 s auto cut-off in progress) |
| `stall` | bool | `true` = relay is ON but current < 0.1 A for 5 s (pump stalled or dry-running) |
| `linkOk` | bool | `true` = a status packet from the Receiver was received within the last 10 s |

---

## ESP-NOW packet types (Sender ↔ Receiver)

| Type | Direction | Struct | Purpose |
|------|-----------|--------|---------| 
| `0x01` | Sender → Receiver | `CmdPacket` | Motor ON/OFF command |
| `0x02` | Receiver → Sender | `StatusPacket` | current, motorOn, waterOk, stall |
| `0x03` | Receiver → Sender | `LogPacket` | Forwarded serial log line |
| `0x04` | Sender → Receiver | `TimerPacket` | Timer config (seconds, remaining, autoRestart, active) |
| `0x05` | Sender → Receiver | `SchedPacket` | Full schedule sync (count + 8 entries) |
| `0x06` | Sender → Receiver | `TimeSyncPacket` | Unix epoch + RTC-enabled flag |

---

## RTC dead code

`#define HAS_RTC_HARDWARE` is **commented out** by default in both firmware files.

When commented out: all `#ifdef HAS_RTC_HARDWARE` blocks compile to zero bytes. The firmware falls back to the phone-time sync path automatically.

When uncommented: Wire.h is included, DS3231 is read for time, and phone-synced epoch is written to DS3231 for persistence across reboots.

---

## Pin summary

### Sender NodeMCU

| Pin | GPIO | Function |
|-----|------|----------|
| D1 | GPIO5 | Button ON (INPUT_PULLUP) / **I²C SCL if RTC wired** |
| D2 | GPIO4 | Button OFF (INPUT_PULLUP) / **I²C SDA if RTC wired** |
| D3 | GPIO0 | Green LED (motor ON) |
| RX/GPIO3 | 3 | Red LED (motor OFF) — TX-only serial |

### Receiver NodeMCU

| Pin | GPIO | Function |
|-----|------|----------|
| D1 | GPIO5 | Relay IN (active-HIGH via level shifter) |
| D2 | GPIO4 | Water sensor (INPUT_PULLUP, LOW=water) |
| D3 | GPIO0 | Servo signal |
| D4 | GPIO2 | Built-in LED blink (active-LOW) |
| D5 | GPIO14 | **I²C SCL if RTC wired** (else LoRa dead code) |
| D6 | GPIO12 | **I²C SDA if RTC wired** (else LoRa dead code) |
| A0 | ADC | ACS712 current sensor |
