# Motor Drive — Feature Reference

> **Covers:** Android app (`com.motordrive.esp32`) + Sender ESP8266 firmware v2 + Receiver ESP8266 firmware v2.

> **Current sensor (Module B) is switched off for now.** "Motor ON" is the receiver's relay GPIO (D0) and nothing else. Stall detection is off, `current` is always 0, and the dashboard card is hidden (`ENABLE_CURRENT_SENSOR = false` in the app, `CURRENT_SENSOR_ENABLED 0` in the receiver). The reasons for the phantom readings are listed beside that flag in the receiver sketch.

---

## Architecture overview

```
Android App
    │  HTTP/JSON over Wi-Fi (sender access point "MotorControl", 192.168.4.1)
    ▼
Sender ESP8266          ─── Wi-Fi (UDP :4210) ───►  Receiver ESP8266
  • Wi-Fi AP (receiver joins it)                       • Relay on D0 (the motor)
  • REST API server + /update (OTA)                    • Servo presses START / STOP
  • EEPROM (200 B)                                     • Water sensor + dry-run cut-off
  • Timer / scheduler (phone time)                     • Own copy of timer / scheduler
  • Log ring-buffer (30 × 90 chars)                    • EEPROM (128 B)
  • Optional router / hotspot client                   • OTA window (5 min, on request)
```

---

## App — Navigation

The app has **four tabs** in a bottom navigation bar:

| Tab | Label | Icon | Purpose |
|-----|-------|------|---------| 
| 1 | **Control** | ⚙ dial | Live dashboard — motor ON/OFF, sensor readings |
| 2 | **Timer** | ⏳ hourglass | Stop-timer with countdown |
| 3 | **Schedule** | 📅 calendar | Time-based start/stop scheduler |
| 4 | **History** | 🕘 clock | Motor ON/OFF event history |

Settings (connection IP, poll interval, sensor toggles, notifications, serial monitor) remain accessible via the ⋮ gear icon in the Control tab toolbar.

---

## Tab 1 — Control (Dashboard)

- **Motor ON / OFF** buttons with visual banner and progress indicator.
- **Stall warning** — only while the current sensor is enabled (it is **off** for now, so this never shows).
- **Link-down state** — if the sender cannot hear the receiver the banner says "last known" and both ON and OFF stay usable.
- **Connection chip** — green = connected; shows `(ESP link down)` if the Wi-Fi link to the receiver is lost.
- **Sensor modules** (each toggleable in Settings):
  - Module A — Three-phase voltage (R/Y/B) — hidden, no firmware support
  - Module B — ACS712 current reading (A) — hidden, sensor disabled for now
  - Module C — Pipe-end water sensor (WATER / DRY / Checking…)
  - Module E — Wi-Fi signal strength of the motor-side ESP (dBm)
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
- For **auto-restart** timers the remaining time is checkpointed to EEPROM **every 2 minutes** (ordinary timers are not checkpointed at all). Writing more often would wear out the flash sector in weeks.
- When the countdown reaches zero → `Motor OFF` is issued and EEPROM is updated.
- The Sender forwards the timer config to the Receiver via **Wi-Fi (UDP) packet type `0x04`** (`TimerPacket`). The Receiver runs its own parallel countdown so that if the Sender reboots, the Receiver still cuts the motor on time.

### Auto-restart after power cut

When **Auto-restart is ON** and power is lost while the timer is active:

1. On reboot, both ESPs load EEPROM → see the active timer with `autoRestart=1`; the sender also finds `motorWasOn=1`.
2. The remaining seconds (last checkpoint, up to 2 minutes stale) are restored.
3. As soon as the sender hears the receiver it sends Motor ON; the countdown resumes from the checkpointed value on both.
4. If only the **receiver** reboots (sender stays powered) while the timer runs, the sender notices the receiver restarted and sends Motor ON again.

---

## Tab 3 — Scheduler

Create up to **8 start/stop schedule entries**, each tied to specific days of the week.

### RTC toggle (Settings → Sensors)

| Switch position | Behaviour |
|-----------------|-----------|
| **OFF** (default) | App sends phone's current time to ESP every **30 seconds** via `POST /api/time/sync`. Firmware advances the epoch internally between syncs. |
| **ON** | Dead code for now: the flag is stored on the ESPs but the firmware has **no DS3231 driver**, so phone time is still used and the app keeps syncing every 30 s. |

> The switch lives in **Settings → Sensors → RTC module**. There is currently **no `HAS_RTC_HARDWARE` / DS3231 code in either `.ino`** — to use a real RTC it would have to be written (I²C pins: sender SDA=D2/SCL=D1 shared with buttons, receiver SDA=D6/SCL=D5).

### Adding a schedule entry

1. Tap **+ ADD SCHEDULE**.
2. Pick **Start time** and **Stop time** using the time picker.
3. Select **repeat days** (Su Mo Tu We Th Fr Sa checkboxes).
4. Toggle **Auto-restart** for this entry.
5. Tap **Add** → entry is pushed to both ESPs via `POST /api/schedule/push`.

### Firmware side

- Sender and Receiver each run their own **scheduler tick every 15 seconds**, comparing current minute-of-day against each enabled entry.
- **Time zone:** the app sends the phone's LOCAL time (UTC + zone/DST offset) every 30 s; the ESPs have no time zone of their own.
- Start/stop are matched on the exact minute. A window whose stop is at/before its start (e.g. 22:00 → 06:00) runs **overnight**; the day mask belongs to the day it STARTED, so a Mon-only 22:00→06:00 entry stops on Tuesday morning.
- **Known gap:** the per-entry "Auto-restart" flag is stored and shown but not acted on by the firmware — after a power cut the clock is only as good as the last sync, so "am I inside the window?" can't be answered reliably without an RTC.
- On a **start minute match** → Motor ON.
- On a **stop minute match** → Motor OFF.
- Both devices deduplicate on the exact minute (minutes since 1970), so an event fires once even if the clock is re-synced.

---

## EEPROM layout

Common to both ESPs (sender uses 200 B, receiver 128 B):

| Address | Size | Field |
|---------|------|-------|
| 0 | 1 B | Magic byte (`0xBE`). If absent, all defaults are used. |
| 1–4 | 4 B | Timer total seconds (uint32) |
| 5 | 1 B | Timer `autoRestart` flag |
| 6 | 1 B | Timer `wasActive` flag |
| 7–10 | 4 B | Timer remaining seconds (checkpointed every 10 s) |
| 11 | 1 B | RTC enabled flag (stored only — no RTC driver) |
| 12–15 | 4 B | Last known (local) Unix epoch (a boot-time starting point only; refreshed at most every 15 min) |
| 16 | 1 B | Schedule count (0–8) |
| 17–80 | 64 B | Schedule entries × 8 (8 B each: startH, startM, stopH, stopM, days, autoRestart, enabled, pad) |
| 81 | 1 B | `motorWasOn` — follows the REAL relay state |

Receiver only:

| Address | Size | Field |
|---------|------|-------|
| 82 | 1 B | `0xD7` when the dry-run timeout below is valid |
| 83–86 | 4 B | Dry-run timeout in ms (5000–60000) |

Sender only:

| Address | Size | Field |
|---------|------|-------|
| 82 | 1 B | `0xCA` when router/hotspot credentials are saved |
| 83–115 | 33 B | Router/hotspot SSID |
| 116–179 | 64 B | Router/hotspot password (up to 63 chars) |
| 185 | 1 B | `0xD7` when the dry-run timeout below is valid |
| 186–189 | 4 B | Dry-run timeout (ms) last set from the app — re-sent to the receiver on every resync |

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
| POST | `/api/time/sync` | `{epoch:N, rtcEnabled:0\|1}` → `{success:true}` | Sync phone LOCAL time (epoch ≥ 1 000 000 000) |
| POST | `/api/config/drytimeout` | `{dryRunSec:5..60}` → `{success:true, dryRunSec}` | Dry-run cut-off time |
| POST | `/api/wifi/config` | `{ssid, pass}` → `{success:true}` | Save router/hotspot credentials and connect (AP+STA) |
| GET | `/api/wifi/status` | `{staConnected, staIp, apIp, staSSID}` | Router/hotspot link and addresses |
| POST | `/api/ota/receiver` | `{pass}` → `{success:true, receiverIp}` | Open the receiver's 5-min update window |
| GET/POST | `/update` | firmware `.bin` upload (HTTP basic auth `admin` / `OTA_PASS`) | Sender OTA |

### Status response field reference

| Field | Type | Meaning |
|-------|------|---------|
| `motorOn` | bool | Relay state (commanded by sender or receiver countdown) |
| `current` | float | Always `0.00` while the current sensor is disabled |
| `isRunning` | bool | `motorOn` mirrored (same value) |
| `waterOk` | bool | Real, debounced sensor state: `true` = water at the sensor (pin LOW). While the motor is ON, `false` starts the dry-run timer (default 10 s, 5–60 s in Settings); the receiver drops the relay when it expires |
| `stall` | bool | Always `false` while the current sensor is disabled |
| `commsMode` | string | `"WiFi"` |
| `linkOk` | bool | `true` = a status packet from the Receiver was received within the last 10 s |
| `rssi` | int/null | Wi-Fi RSSI (dBm) of the receiver ↔ sender AP link, reported by the receiver |

---

## UDP packet types (Sender ↔ Receiver, port 4210)

| Type | Direction | Struct | Purpose |
|------|-----------|--------|---------| 
| `0x01` | Sender → Receiver | `CmdPacket` | Motor ON/OFF command |
| `0x02` | Receiver → Sender | `StatusPacket` | current, motorOn, waterOk, stall, rssi (Wi-Fi dBm) |
| `0x03` | Receiver → Sender | `LogPacket` | Forwarded serial log line |
| `0x04` | Sender → Receiver | `TimerPacket` | Timer config (seconds, remaining, autoRestart, active) |
| `0x05` | Sender → Receiver | `SchedPacket` | Full schedule sync (count + 8 entries) |
| `0x06` | Sender → Receiver | `TimeSyncPacket` | Unix epoch + RTC-enabled flag |
| `0x07` | Sender → Receiver | `ConfigPacket` | Dry-run timeout (ms) |
| `0x08` | Sender → Receiver | 1 byte | Open the receiver OTA window (refused while the motor runs) |

---

## RTC switch (dead code)

Settings → Sensors → **RTC module (DS3231)** exists, default **OFF**. Switching it on only stores a flag on the ESPs: neither sketch contains DS3231 / `Wire` code, so phone time is always the clock and the app keeps syncing every 30 s. To use a real RTC the driver would have to be written.

---

## Pin summary

### Sender NodeMCU

| Pin | GPIO | Function |
|-----|------|----------|
| D1 | GPIO5 | Button ON (INPUT_PULLUP) / **I²C SCL if RTC wired** |
| D2 | GPIO4 | Button OFF (INPUT_PULLUP) / **I²C SDA if RTC wired** |
| D3 | GPIO0 | Green LED (motor ON) |
| D7 | GPIO13 | Red LED (motor OFF) |
| D4 | GPIO2 | On-board LED: solid = receiver link OK (heard within 10 s; a phone on the AP does not count), blink = command |

### Receiver NodeMCU

| Pin | GPIO | Function |
|-----|------|----------|
| D0 | GPIO16 | Relay IN — active-HIGH by default (`RELAY_ACTIVE_LOW` flag for active-LOW modules). **GPIO16 is driven HIGH by the chip while it boots**, so an active-HIGH relay can click briefly at power-up |
| D2 | GPIO4 | Water sensor — active-LOW by default (INPUT_PULLUP, LOW = water, so a cut wire reads "dry"); `WATER_ACTIVE_LOW 0` for an active-HIGH sensor (needs an external pull-down) |
| D3 | GPIO0 | Servo signal (attached only while moving, idles HIGH). End stops / centre / pulse range are set in the **SERVO TRAVEL** header at the top of the receiver sketch: `SERVO_MAX_LEFT_DEG` (0°) = START press, `SERVO_MAX_RIGHT_DEG` (180°) = STOP press, `SERVO_CENTER_DEG` (90°) = rest. Every move is clamped to the two end stops |
| D4 | GPIO2 | Built-in LED blink (active-LOW) |
| D5 / D6 | GPIO14 / 12 | free (would be I²C SCL / SDA for an RTC) |
| A0 | ADC | ACS712 current sensor — **disabled** (`CURRENT_SENSOR_ENABLED 0`) |

---

## Wi-Fi link behaviour (sender ⇄ receiver)

- **Resync:** whenever the sender hears the receiver again after silence, on first contact, or when the receiver's status counter restarts (it rebooted), the sender re-sends time, schedules and a running timer. The receiver also keeps its own copy of timers/schedules in EEPROM and enforces them even if the link is dead.
- **Motor state:** the receiver reports its relay GPIO. The sender stores "motor was on" from that real state, so a dry-run cut never causes an auto-restart after a power loss.
- **Dry-run timeout:** saved on the receiver the moment it is set from the app and restored after a reboot.
- **OTA over Wi-Fi:** sender `http://192.168.4.1/update` (always on); receiver window opened from Settings → OTA (5 min, motor must be OFF, starts are blocked while open). User `admin`, password `OTA_PASS` in both sketches (default `motor123`).
- **Clock:** re-based every hour, so a standalone controller keeps correct time of day across the 49.7-day `millis()` wrap. Without an RTC, after a power cut the clock restarts from the last saved time until the app syncs phone time again, so schedules can fire late until then.
- **Dry-run timeout** is also stored on the sender and re-sent whenever the receiver (re)appears, so setting it while the receiver is out of range is not lost.
- **Input checks (sender):** timer 1 s – 7 days, time-sync epoch must be sane, schedule count 0–8, Wi-Fi password up to 63 characters (quotes and backslashes are fine), SSID up to 32 bytes.
- **App:** schedules whose push to the ESP failed are re-sent automatically the next time the app connects. Overnight windows (e.g. 22:00 → 06:00) can be created and show "(+1 day)". When the sender is reachable but the receiver is not, the Control screen says the motor state is "last known" and keeps both ON and OFF usable.
- **Commands:** the receiver remembers the last 8 command numbers, so a late copy of an old ON can never restart the motor after a newer OFF.
- **Sender Wi-Fi client:** with saved router/hotspot credentials it tries once, gives up after 12 s, then waits 30 s → 60 s → 120 s → 180 s between attempts. Scanning for a missing network pulls the radio off the AP channel, so it is kept rare.
- **Flash wear:** each EEPROM commit erases a 4 KB sector good for roughly 100 000 erases. Time syncs are saved at most every 15 min and timers are checkpointed only for auto-restart, every 2 min — about 4 and 30 erases per hour instead of 120 and 360.
- **Strict EEPROM loading:** a power cut in the middle of a flash write can leave a valid magic byte with erased (0xFF) data behind it. Flags must be exactly 0 or 1 and times are range-checked, so garbage can never restore a phantom timer or auto-start the motor; schedules with impossible times are disabled and an impossible clock leaves the scheduler off until the app syncs.
- **Log lines** are cleaned (control characters, broken UTF-8) before they reach `/api/logs`, and the SSID is escaped in `/api/wifi/status`, so the app always receives valid JSON.

---

## Building and flashing

- **Board:** *NodeMCU 1.0 (ESP-12E Module)* in the Arduino IDE with the ESP8266 core. No extra libraries are needed (`ESP8266WiFi`, `ESP8266WebServer`, `ESP8266HTTPUpdateServer`, `EEPROM`, `Servo` all ship with the core).
- **Flash size:** pick a layout that leaves room for OTA, e.g. *4MB (FS:2MB OTA:~1019KB)*, otherwise Wi-Fi uploads fail.
- **Change before use:** the access-point password (`AP_PASS`, must match in both sketches) and the OTA password (`OTA_PASS`, same in both sketches and the app).
- **Everything in the receiver's header block can be overridden at build time** (`-D` flags), e.g. `RELAY_ACTIVE_LOW`, `SERVO_MAX_LEFT_DEG`, `WATER_ACTIVE_LOW`, `BLOCK_START_WHEN_DRY`.
- **Do not put a sketch-defined type in a function signature.** The Arduino IDE inserts auto-generated prototypes above the first function, before the type is declared, and the build fails. `firmware-sim/run.sh` includes a check that emulates this step.
- **Test kit:** `./firmware-sim/run.sh` runs both sketches against a software model of the hardware (needs g++ and python3).
