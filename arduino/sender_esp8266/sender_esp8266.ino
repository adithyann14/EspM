/**
 * ════════════════════════════════════════════════════════════════════════
 *  SENDER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)   v2
 *
 *  Wi-Fi AP "MotorControl" / "motor1234" — app connects here.
 *
 *  REST endpoints:
 *    GET  /api/status          → motorOn, current, isRunning, waterOk, stall, linkOk
 *    POST /api/motor/on|off    → send ESP-NOW CmdPacket to Receiver
 *    GET  /api/logs            → 30-entry serial log ring-buffer
 *    GET  /api/alerts          → stub []
 *    POST /api/alerts/clear    → stub
 *    POST /api/timer/set       → {seconds, autoRestart}
 *    POST /api/timer/cancel
 *    GET  /api/timer/status    → {active, remaining, total, autoRestart}
 *    POST /api/schedule/push   → {count, entries:[{slot,startH,startM,stopH,stopM,days,autoRestart,enabled}]}
 *    POST /api/schedule/clear
 *    POST /api/time/sync       → {epoch, rtcEnabled}
 *
 *  ESP-NOW packet types:
 *    0x01  CmdPacket        Sender → Receiver  (ON / OFF)
 *    0x02  StatusPacket     Receiver → Sender  (current, motorOn, waterOk, stall)
 *    0x03  LogPacket        Receiver → Sender  (log forwarding)
 *    0x04  TimerPacket      Sender → Receiver  (timer config)
 *    0x05  SchedPacket      Sender → Receiver  (full schedule)
 *    0x06  TimeSyncPacket   Sender → Receiver  (Unix epoch + RTC flag)
 *
 *  EEPROM layout (EE_SIZE = 128):
 *    0       magic byte (0xBE)
 *    1–4     timer total seconds
 *    5       timer autoRestart
 *    6       timer wasActive
 *    7–10    timer remaining (checkpointed every 10 s)
 *    11      RTC enabled flag
 *    12–15   last known epoch
 *    16      schedule count
 *    17–80   schedule entries × 8 (8 B each)
 *    81      motorWasOn
 *
 *  RTC DEAD CODE — uncomment #define HAS_RTC_HARDWARE and wire DS3231:
 *    SDA = D2/GPIO4 (same pin as BTN_OFF — remove button when RTC fitted)
 *    SCL = D1/GPIO5 (same pin as BTN_ON  — remove button when RTC fitted)
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <EEPROM.h>
// #define HAS_RTC_HARDWARE
// #ifdef  HAS_RTC_HARDWARE
// #include <Wire.h>
// #include <RTClib.h>
// static RTC_DS3231 rtc;
// #endif

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>
}

// ── LoRa SX1278 — DEAD CODE ─────────────────────────────────────────────
// #include <SPI.h>  #include <LoRa.h>
// #define LORA_RST D0  #define LORA_DIO0 D4  #define LORA_SCK D5
// #define LORA_MISO D6  #define LORA_MOSI D7  #define LORA_NSS D8
// ─────────────────────────────────────────────────────────────────────────

// ── Pins ──────────────────────────────────────────────────────────────────
#define PIN_BTN_ON   D1   // GPIO5  (also I²C SCL if RTC wired — remove btn)
#define PIN_BTN_OFF  D2   // GPIO4  (also I²C SDA if RTC wired — remove btn)
#define PIN_LED_ON   D3   // GPIO0  green LED
#define PIN_LED_OFF  3    // GPIO3 (RX) red LED  — TX-only serial frees this

// ── Wi-Fi AP ──────────────────────────────────────────────────────────────
static const char*   AP_SSID = "MotorControl";
static const char*   AP_PASS = "motor1234";
static const uint8_t WIFI_CH = 1;

// ── EEPROM ────────────────────────────────────────────────────────────────
#define EE_SIZE          128
#define EE_MAGIC         0
#define EE_TIMER_TOTAL   1
#define EE_TIMER_AUTO    5
#define EE_TIMER_ACTIVE  6
#define EE_TIMER_REM     7
#define EE_RTC_FLAG      11
#define EE_EPOCH         12
#define EE_SCHED_COUNT   16
#define EE_SCHED_BASE    17
#define EE_MOTOR_WAS_ON  81
#define EE_MAGIC_VAL     0xBE

static inline void eeWriteU32(int addr, uint32_t v) {
  EEPROM.write(addr,     (v      ) & 0xFF);
  EEPROM.write(addr + 1, (v >>  8) & 0xFF);
  EEPROM.write(addr + 2, (v >> 16) & 0xFF);
  EEPROM.write(addr + 3, (v >> 24) & 0xFF);
}
static inline uint32_t eeReadU32(int addr) {
  return (uint32_t)EEPROM.read(addr)
       | ((uint32_t)EEPROM.read(addr+1) << 8)
       | ((uint32_t)EEPROM.read(addr+2) << 16)
       | ((uint32_t)EEPROM.read(addr+3) << 24);
}

// ── Log ring-buffer ────────────────────────────────────────────────────────
#define LOG_ENTRIES 30
#define LOG_LEN     90
static char    g_log[LOG_ENTRIES][LOG_LEN];
static uint8_t g_logHead  = 0;
static uint8_t g_logCount = 0;

static void logAdd(const char* msg) {
  Serial.println(msg);
  strncpy(g_log[g_logHead], msg, LOG_LEN - 1);
  g_log[g_logHead][LOG_LEN - 1] = '\0';
  g_logHead  = (g_logHead  + 1) % LOG_ENTRIES;
  if (g_logCount < LOG_ENTRIES) g_logCount++;
}
static void logFmt(const char* fmt, ...) {
  char buf[LOG_LEN]; va_list ap;
  va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  logAdd(buf);
}

// ── ESP-NOW packets ────────────────────────────────────────────────────────
static uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct __attribute__((packed)) {
  uint8_t type; uint8_t cmd; uint32_t seq;
} CmdPacket;                                // 0x01

typedef struct __attribute__((packed)) {
  uint8_t type; float current; uint8_t motorOn;
  uint8_t waterOk; uint8_t stall; uint32_t seq;
} StatusPacket;                             // 0x02

typedef struct __attribute__((packed)) {
  uint8_t type; char msg[59];
} LogPacket;                                // 0x03

typedef struct __attribute__((packed)) {
  uint8_t  type;           // 0x04
  uint32_t totalSec;
  uint32_t remainingSec;
  uint8_t  autoRestart;
  uint8_t  active;
} TimerPacket;

typedef struct __attribute__((packed)) {
  uint8_t type;            // 0x05
  uint8_t count;
  struct { uint8_t startH, startM, stopH, stopM, days, autoRestart, enabled, pad; } entries[8];
} SchedPacket;

typedef struct __attribute__((packed)) {
  uint8_t  type;           // 0x06
  uint32_t epoch;
  uint8_t  rtcEnabled;
} TimeSyncPacket;

// ── Motor / sensor state ──────────────────────────────────────────────────
static bool          g_motorOn  = false;   // synced from receiver — ground truth
static volatile float g_currentA = 0.0f;
static volatile bool  g_waterOk  = true;
static volatile bool  g_stall    = false;
static uint32_t       g_cmdSeq   = 0;
static unsigned long  g_lastRecvMs = 0;
static bool           g_linkLost  = true;

// ── Button debounce — 3-state machine: 0=idle 1=armed 2=wait-release ──────
static uint8_t       g_btnOnState  = 0;
static unsigned long g_btnOnMs     = 0;
static uint8_t       g_btnOffState = 0;
static unsigned long g_btnOffMs    = 0;
#define DEBOUNCE_MS 50UL

// ── LEDs ──────────────────────────────────────────────────────────────────
#define BLINK_ON_MS   80UL
#define BLINK_OFF_MS 120UL
static volatile bool g_blinkReq = false;
static unsigned long g_blinkPhaseMs = 0;
static uint8_t       g_blinkPhase   = 0;   // 0=idle 1=off 2=restore

// ── Timer state ───────────────────────────────────────────────────────────
static bool          g_timerActive  = false;
static uint32_t      g_timerTotal   = 0;
static uint32_t      g_timerRemain  = 0;
static bool          g_timerAutoRst = false;
static unsigned long g_timerLastTick   = 0;
static unsigned long g_timerLastSaveMs = 0;
#define TIMER_SAVE_INTERVAL_MS 10000UL

// ── Schedule state ────────────────────────────────────────────────────────
struct SchedEntry { uint8_t startH, startM, stopH, stopM, days, autoRestart, enabled, pad; };
static uint8_t     g_schedCount = 0;
static SchedEntry  g_scheds[8]  = {};
static int         g_lastActedMinute = -1;  // dedup within the same minute

// ── RTC / epoch ───────────────────────────────────────────────────────────
static bool     g_rtcEnabled = false;
static uint32_t g_epoch      = 0;   // last synced Unix epoch
static unsigned long g_epochMs = 0; // millis() at the time of last sync

static uint32_t currentEpoch() {
  uint32_t elapsed = (millis() - g_epochMs) / 1000UL;
  return g_epoch + elapsed;
}

// Day-of-week bit from epoch (0=Sun … 6=Sat)
static uint8_t epochDayBit(uint32_t e) {
  // Jan 1 1970 was Thursday (day 4)
  return (uint8_t)(((e / 86400UL) + 4UL) % 7UL);
}

// ── RTC dead code ─────────────────────────────────────────────────────────
// #ifdef HAS_RTC_HARDWARE
// static uint32_t rtcRead() {
//   DateTime now = rtc.now();
//   return now.unixtime();
// }
// static void rtcSet(uint32_t epoch) { rtc.adjust(DateTime(epoch)); }
// #endif

// ── EEPROM helpers ─────────────────────────────────────────────────────────
static void eeSave() {
  EEPROM.write(EE_MAGIC, EE_MAGIC_VAL);
  eeWriteU32(EE_TIMER_TOTAL,  g_timerTotal);
  EEPROM.write(EE_TIMER_AUTO,   g_timerAutoRst ? 1 : 0);
  EEPROM.write(EE_TIMER_ACTIVE, g_timerActive  ? 1 : 0);
  eeWriteU32(EE_TIMER_REM,    g_timerRemain);
  EEPROM.write(EE_RTC_FLAG,   g_rtcEnabled ? 1 : 0);
  eeWriteU32(EE_EPOCH,        currentEpoch());
  EEPROM.write(EE_SCHED_COUNT, g_schedCount);
  for (uint8_t i = 0; i < 8; i++) {
    int base = EE_SCHED_BASE + i * 8;
    EEPROM.write(base + 0, g_scheds[i].startH);
    EEPROM.write(base + 1, g_scheds[i].startM);
    EEPROM.write(base + 2, g_scheds[i].stopH);
    EEPROM.write(base + 3, g_scheds[i].stopM);
    EEPROM.write(base + 4, g_scheds[i].days);
    EEPROM.write(base + 5, g_scheds[i].autoRestart);
    EEPROM.write(base + 6, g_scheds[i].enabled);
    EEPROM.write(base + 7, 0);
  }
  EEPROM.write(EE_MOTOR_WAS_ON, g_motorOn ? 1 : 0);
  EEPROM.commit();
}

static void eeLoad() {
  if (EEPROM.read(EE_MAGIC) != EE_MAGIC_VAL) {
    logAdd("[SEND] EEPROM blank — using defaults");
    return;
  }
  g_timerTotal   = eeReadU32(EE_TIMER_TOTAL);
  g_timerAutoRst = EEPROM.read(EE_TIMER_AUTO)   != 0;
  bool wasActive = EEPROM.read(EE_TIMER_ACTIVE)  != 0;
  g_timerRemain  = eeReadU32(EE_TIMER_REM);
  g_rtcEnabled   = EEPROM.read(EE_RTC_FLAG)      != 0;
  g_epoch        = eeReadU32(EE_EPOCH);
  g_epochMs      = millis();
  g_schedCount   = min((uint8_t)8, EEPROM.read(EE_SCHED_COUNT));
  for (uint8_t i = 0; i < 8; i++) {
    int base = EE_SCHED_BASE + i * 8;
    g_scheds[i].startH    = EEPROM.read(base + 0);
    g_scheds[i].startM    = EEPROM.read(base + 1);
    g_scheds[i].stopH     = EEPROM.read(base + 2);
    g_scheds[i].stopM     = EEPROM.read(base + 3);
    g_scheds[i].days      = EEPROM.read(base + 4);
    g_scheds[i].autoRestart = EEPROM.read(base + 5);
    g_scheds[i].enabled   = EEPROM.read(base + 6);
  }
  bool motorWasOn = EEPROM.read(EE_MOTOR_WAS_ON) != 0;
  logFmt("[SEND] EEPROM loaded  timer=%lus(%s)  scheds=%d  motorWasOn=%d",
    (unsigned long)g_timerTotal, wasActive?"active":"idle",
    (int)g_schedCount, (int)motorWasOn);

  // Auto-restart: resume if timer was running and autoRestart is set
  if (wasActive && g_timerAutoRst && g_timerRemain > 0) {
    g_timerActive    = true;
    g_timerLastTick  = millis();
    logFmt("[SEND] auto-restart timer: %lus remaining", (unsigned long)g_timerRemain);
    sendTimerPacket();
  }
  if (motorWasOn && g_timerAutoRst) {
    sendMotorCommandInternal(true);
  }
}

// ── HTTP server ────────────────────────────────────────────────────────────
static ESP8266WebServer server(80);

// ═════════════════════════════════════════════════════════════════════════
//  LED helpers
// ═════════════════════════════════════════════════════════════════════════

static void updateLEDs() {
  static bool lastOn = false;
  if (g_motorOn == lastOn) return;   // only update on change
  lastOn = g_motorOn;
  digitalWrite(PIN_LED_ON,  g_motorOn ? HIGH : LOW);
  digitalWrite(PIN_LED_OFF, g_motorOn ? LOW  : HIGH);
}

static void handleBlink() {
  unsigned long now = millis();
  if (g_blinkReq && g_blinkPhase == 0) {
    g_blinkReq = false; g_blinkPhase = 1; g_blinkPhaseMs = now;
    digitalWrite(PIN_LED_ON, LOW); digitalWrite(PIN_LED_OFF, LOW);
  }
  if (g_blinkPhase == 1 && (now - g_blinkPhaseMs) >= BLINK_ON_MS) {
    g_blinkPhase = 2; g_blinkPhaseMs = now; updateLEDs();
  }
  if (g_blinkPhase == 2 && (now - g_blinkPhaseMs) >= BLINK_OFF_MS) g_blinkPhase = 0;
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW send helpers
// ═════════════════════════════════════════════════════════════════════════

static void sendTimerPacket() {
  TimerPacket p;
  p.type        = 0x04;
  p.totalSec    = g_timerTotal;
  p.remainingSec= g_timerRemain;
  p.autoRestart = g_timerAutoRst ? 1 : 0;
  p.active      = g_timerActive  ? 1 : 0;
  esp_now_send(BCAST_MAC, (uint8_t*)&p, sizeof(p));
}

static void sendSchedPacket() {
  SchedPacket p; memset(&p, 0, sizeof(p));
  p.type  = 0x05;
  p.count = g_schedCount;
  for (uint8_t i = 0; i < 8; i++) p.entries[i] = *(decltype(&p.entries[0]))&g_scheds[i];
  esp_now_send(BCAST_MAC, (uint8_t*)&p, sizeof(p));
}

static void sendTimeSyncPacket() {
  TimeSyncPacket p;
  p.type       = 0x06;
  p.epoch      = currentEpoch();
  p.rtcEnabled = g_rtcEnabled ? 1 : 0;
  esp_now_send(BCAST_MAC, (uint8_t*)&p, sizeof(p));
}

// ═════════════════════════════════════════════════════════════════════════
//  Motor command (internal — no HTTP response here)
// ═════════════════════════════════════════════════════════════════════════

static void sendMotorCommandInternal(bool on) {
  CmdPacket pkt;
  pkt.type = 0x01; pkt.cmd = on ? 0x01 : 0x02; pkt.seq = ++g_cmdSeq;
  logFmt("[SEND] Motor %s  seq=%lu", on ? "ON" : "OFF", (unsigned long)g_cmdSeq);
  for (uint8_t i = 0; i < 3; i++) { esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt)); delay(12); }
  // Save EEPROM on every motor state change so auto-restart knows motorWasOn
  EEPROM.write(EE_MOTOR_WAS_ON, on ? 1 : 0);
  EEPROM.commit();
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═════════════════════════════════════════════════════════════════════════

void espnowOnSend(uint8_t* /*mac*/, uint8_t /*status*/) { }

void espnowOnRecv(uint8_t* /*mac*/, uint8_t* data, uint8_t len) {
  if (len < 1) return;
  switch (data[0]) {
    case 0x02: {
      if (len < sizeof(StatusPacket)) return;
      StatusPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      // Receiver is ground truth for motor state + sensors.
      // g_motorOn is only synced from the RECEIVER's relay state —
      // not from any app-optimistic update — to prevent false triggers.
      g_motorOn       = (pkt.motorOn  != 0);
      g_currentA      = pkt.current;
      g_waterOk       = (pkt.waterOk  != 0);
      g_stall         = (pkt.stall    != 0);
      g_lastRecvMs    = millis();
      g_linkLost      = false;
      g_blinkReq      = true;
      if (g_stall) logAdd("[SEND] ⚠ STALL reported by receiver");
      updateLEDs();
      break;
    }
    case 0x03: {
      if (len < 2) return;
      LogPacket pkt; memcpy(&pkt, data, min((size_t)len, sizeof(pkt)));
      pkt.msg[sizeof(pkt.msg)-1] = '\0';
      logAdd(pkt.msg);
      g_blinkReq = true;
      break;
    }
    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Timer tick
// ═════════════════════════════════════════════════════════════════════════

static void handleTimerTick() {
  if (!g_timerActive) return;
  unsigned long now = millis();
  unsigned long elapsed = (now - g_timerLastTick) / 1000UL;
  if (elapsed == 0) return;
  g_timerLastTick = now - ((now - g_timerLastTick) % 1000UL);

  if (elapsed >= g_timerRemain) {
    g_timerRemain  = 0;
    g_timerActive  = false;
    logAdd("[SEND] ⏰ timer expired — Motor OFF");
    sendMotorCommandInternal(false);
    EEPROM.write(EE_TIMER_ACTIVE, 0); eeWriteU32(EE_TIMER_REM, 0); EEPROM.commit();
    sendTimerPacket();
  } else {
    g_timerRemain -= (uint32_t)elapsed;
    // Checkpoint to EEPROM every TIMER_SAVE_INTERVAL_MS
    if (now - g_timerLastSaveMs >= TIMER_SAVE_INTERVAL_MS) {
      g_timerLastSaveMs = now;
      eeWriteU32(EE_TIMER_REM, g_timerRemain);
      EEPROM.commit();
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Scheduler tick (checked every 15 s, acts once per minute-match)
// ═════════════════════════════════════════════════════════════════════════

static unsigned long g_schedLastCheckMs = 0;

static void handleScheduler() {
  unsigned long now = millis();
  if (now - g_schedLastCheckMs < 15000UL) return;
  g_schedLastCheckMs = now;
  if (g_epoch == 0) return;   // no time synced yet
  uint32_t ep  = currentEpoch();
  uint16_t tod = (uint16_t)((ep % 86400UL) / 60UL);  // minute of day
  uint8_t  day = epochDayBit(ep);

  if ((int)tod == g_lastActedMinute) return;   // already fired this minute

  for (uint8_t i = 0; i < g_schedCount; i++) {
    SchedEntry& s = g_scheds[i];
    if (!s.enabled) continue;
    if (!(s.days & (1 << day))) continue;
    uint16_t startTod = (uint16_t)(s.startH * 60 + s.startM);
    uint16_t stopTod  = (uint16_t)(s.stopH  * 60 + s.stopM );
    if (tod == startTod) {
      logFmt("[SCHED] slot %d → Motor ON  (%02d:%02d)", (int)i, (int)s.startH, (int)s.startM);
      sendMotorCommandInternal(true);
      g_lastActedMinute = (int)tod;
      break;
    }
    if (tod == stopTod) {
      logFmt("[SCHED] slot %d → Motor OFF (%02d:%02d)", (int)i, (int)s.stopH,  (int)s.stopM);
      sendMotorCommandInternal(false);
      g_lastActedMinute = (int)tod;
      break;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  HTTP: CORS + status JSON
// ═════════════════════════════════════════════════════════════════════════

static void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static inline bool isLinkOk() { return (g_lastRecvMs > 0) && (millis() - g_lastRecvMs < 10000UL); }

static String buildStatusJson() {
  String j = "{";
  j += "\"motorOn\":"   + String(g_motorOn           ? "true":"false") + ",";
  j += "\"current\":"   + String(g_currentA, 2)                        + ",";
  j += "\"isRunning\":" + String(g_motorOn           ? "true":"false") + ",";
  j += "\"waterOk\":"   + String(g_waterOk           ? "true":"false") + ",";
  j += "\"stall\":"     + String(g_stall             ? "true":"false") + ",";
  j += "\"linkOk\":"    + String(isLinkOk()           ? "true":"false");
  j += "}";
  return j;
}

// ── Route handlers ────────────────────────────────────────────────────────
void handleStatus()      { addCorsHeaders(); server.send(200,"application/json",buildStatusJson()); }
void handleMotorOn()     { sendMotorCommandInternal(true);  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}"); }
void handleMotorOff()    { sendMotorCommandInternal(false); addCorsHeaders(); server.send(200,"application/json","{\"success\":true}"); }
void handleAlerts()      { addCorsHeaders(); server.send(200,"application/json","{\"alerts\":[]}"); }
void handleAlertsClear() { addCorsHeaders(); server.send(200,"application/json","{\"success\":true}"); }
void handleOptions()     { addCorsHeaders(); server.send(204); }
void handleNotFound()    { server.send(404,"application/json","{\"error\":\"not found\"}"); }

void handleLogs() {
  addCorsHeaders();
  String j = "{\"logs\":[";
  uint8_t total = min((int)g_logCount, (int)LOG_ENTRIES);
  uint8_t start = (g_logCount < LOG_ENTRIES) ? 0 : g_logHead;
  for (uint8_t i = 0; i < total; i++) {
    uint8_t idx = (start + i) % LOG_ENTRIES;
    if (i > 0) j += ',';
    j += '"';
    for (const char* p = g_log[idx]; *p; p++) {
      if      (*p == '"')  j += "\\\"";
      else if (*p == '\\') j += "\\\\";
      else                 j += *p;
    }
    j += '"';
  }
  j += "]}";
  server.send(200,"application/json",j);
}

void handleTimerSet() {
  String body = server.arg("plain");
  int secIdx = body.indexOf("\"seconds\":");
  int autIdx = body.indexOf("\"autoRestart\":");
  if (secIdx < 0) { server.send(400,"application/json","{\"error\":\"missing seconds\"}"); return; }
  uint32_t sec = (uint32_t)body.substring(secIdx + 10).toInt();
  bool     ar  = (autIdx >= 0) && (body.substring(autIdx + 14).toInt() != 0);
  g_timerTotal   = sec;
  g_timerRemain  = sec;
  g_timerAutoRst = ar;
  g_timerActive  = true;
  g_timerLastTick = millis();
  g_timerLastSaveMs = 0;
  eeSave();
  logFmt("[TIMER] set %lus  autoRestart=%d", (unsigned long)sec, (int)ar);
  sendTimerPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

void handleTimerCancel() {
  g_timerActive = false; g_timerRemain = 0;
  EEPROM.write(EE_TIMER_ACTIVE, 0); eeWriteU32(EE_TIMER_REM, 0); EEPROM.commit();
  logAdd("[TIMER] cancelled");
  sendTimerPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

void handleTimerStatus() {
  addCorsHeaders();
  String j = "{";
  j += "\"active\":"     + String(g_timerActive  ? "true":"false") + ",";
  j += "\"remaining\":"  + String(g_timerRemain)                   + ",";
  j += "\"total\":"      + String(g_timerTotal)                    + ",";
  j += "\"autoRestart\":" + String(g_timerAutoRst? "true":"false");
  j += "}";
  server.send(200,"application/json",j);
}

void handleSchedPush() {
  String body = server.arg("plain");
  int cntIdx = body.indexOf("\"count\":");
  int entIdx = body.indexOf("\"entries\":");
  if (cntIdx < 0 || entIdx < 0) { server.send(400,"application/json","{\"error\":\"malformed\"}"); return; }
  g_schedCount = (uint8_t)min(8, body.substring(cntIdx + 8).toInt());
  // Naive parse — find each slot object
  int pos = entIdx + 10;
  for (uint8_t i = 0; i < g_schedCount; i++) {
    SchedEntry& s = g_scheds[i];
    auto rd = [&](const char* k) -> uint8_t {
      int ki = body.indexOf(k, pos);
      if (ki < 0) return 0;
      return (uint8_t)body.substring(ki + strlen(k)).toInt();
    };
    s.startH    = rd("\"startH\":");
    s.startM    = rd("\"startM\":");
    s.stopH     = rd("\"stopH\":");
    s.stopM     = rd("\"stopM\":");
    s.days      = rd("\"days\":");
    s.autoRestart = rd("\"autoRestart\":");
    s.enabled   = rd("\"enabled\":");
    pos = body.indexOf("{", pos + 1);  // advance past this entry
  }
  eeSave();
  logFmt("[SCHED] pushed %d schedule(s)", (int)g_schedCount);
  sendSchedPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

void handleSchedClear() {
  g_schedCount = 0;
  memset(g_scheds, 0, sizeof(g_scheds));
  eeSave();
  logAdd("[SCHED] cleared");
  sendSchedPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

void handleTimeSync() {
  String body = server.arg("plain");
  int epIdx  = body.indexOf("\"epoch\":");
  int rtcIdx = body.indexOf("\"rtcEnabled\":");
  if (epIdx < 0) { server.send(400,"application/json","{\"error\":\"missing epoch\"}"); return; }
  uint32_t ep  = (uint32_t)body.substring(epIdx + 8).toInt();
  bool     rtc = (rtcIdx >= 0) && (body.substring(rtcIdx + 13).toInt() != 0);
  g_epoch     = ep;
  g_epochMs   = millis();
  g_rtcEnabled = rtc;
  EEPROM.write(EE_RTC_FLAG, rtc ? 1 : 0);
  eeWriteU32(EE_EPOCH, ep); EEPROM.commit();
  // #ifdef HAS_RTC_HARDWARE
  // if (rtc) rtcSet(ep);
  // #endif
  sendTimeSyncPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200, SERIAL_8N1, SERIAL_TX_ONLY);
  delay(100);
  logAdd("[SEND] boot v2");

  pinMode(PIN_BTN_ON,  INPUT_PULLUP);
  pinMode(PIN_BTN_OFF, INPUT_PULLUP);
  pinMode(PIN_LED_ON,  OUTPUT);
  pinMode(PIN_LED_OFF, OUTPUT);
  digitalWrite(PIN_LED_ON, LOW); digitalWrite(PIN_LED_OFF, HIGH);  // red = OFF at boot

  // EEPROM
  EEPROM.begin(EE_SIZE);
  eeLoad();

  // #ifdef HAS_RTC_HARDWARE
  // Wire.begin(); if (!rtc.begin()) logAdd("[SEND] DS3231 not found");
  // else { g_epoch = rtcRead(); g_epochMs = millis(); logFmt("[SEND] RTC epoch=%lu", g_epoch); }
  // #endif

  // Wi-Fi AP
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, WIFI_CH);
  WiFi.setOutputPower(20.5f);
  wifi_set_phy_mode(PHY_MODE_11B);
  logFmt("[SEND] AP  IP=%s  SSID=%s", WiFi.softAPIP().toString().c_str(), AP_SSID);

  // Boot blink 2× = "I am SENDER" (receiver does 3×)
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED_ON, HIGH); delay(220);
    digitalWrite(PIN_LED_ON, LOW);  delay(150);
  }
  delay(300);

  // ESP-NOW (3 retries)
  int tries = 3;
  while (esp_now_init() != 0 && tries-- > 0) { logAdd("[SEND] ESP-NOW init retry…"); delay(500); }
  if (tries < 0) { logAdd("[SEND] ESP-NOW FAILED — reboot"); delay(1000); ESP.restart(); }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);
  esp_now_add_peer(BCAST_MAC, ESP_NOW_ROLE_COMBO, WIFI_CH, nullptr, 0);
  logAdd("[SEND] ESP-NOW ready (broadcast)");

  // HTTP routes
  server.on("/api/status",         HTTP_GET,    handleStatus);
  server.on("/api/motor/on",       HTTP_POST,   handleMotorOn);
  server.on("/api/motor/off",      HTTP_POST,   handleMotorOff);
  server.on("/api/logs",           HTTP_GET,    handleLogs);
  server.on("/api/alerts",         HTTP_GET,    handleAlerts);
  server.on("/api/alerts/clear",   HTTP_POST,   handleAlertsClear);
  server.on("/api/timer/set",      HTTP_POST,   handleTimerSet);
  server.on("/api/timer/cancel",   HTTP_POST,   handleTimerCancel);
  server.on("/api/timer/status",   HTTP_GET,    handleTimerStatus);
  server.on("/api/schedule/push",  HTTP_POST,   handleSchedPush);
  server.on("/api/schedule/clear", HTTP_POST,   handleSchedClear);
  server.on("/api/time/sync",      HTTP_POST,   handleTimeSync);
  // OPTIONS pre-flight for all
  auto opt = [](){ server.sendHeader("Access-Control-Allow-Origin","*");
                   server.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
                   server.sendHeader("Access-Control-Allow-Headers","Content-Type");
                   server.send(204); };
  server.on("/api/motor/on",       HTTP_OPTIONS, opt);
  server.on("/api/motor/off",      HTTP_OPTIONS, opt);
  server.on("/api/timer/set",      HTTP_OPTIONS, opt);
  server.on("/api/timer/cancel",   HTTP_OPTIONS, opt);
  server.on("/api/schedule/push",  HTTP_OPTIONS, opt);
  server.on("/api/schedule/clear", HTTP_OPTIONS, opt);
  server.on("/api/time/sync",      HTTP_OPTIONS, opt);
  server.onNotFound(handleNotFound);
  server.begin();

  updateLEDs();
  logAdd("[SEND] ready — connect phone to MotorControl / motor1234");
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // ── Link-lost detection (> 10 s without status from receiver) ─────────
  if (g_lastRecvMs > 0 && (now - g_lastRecvMs) > 10000UL && !g_linkLost) {
    g_linkLost = true;
    logAdd("[SEND] ⚠ ESP-NOW link lost — no receiver status for >10 s");
  }

  // ── Button ON ─────────────────────────────────────────────────────────
  bool onRaw = (digitalRead(PIN_BTN_ON) == LOW);
  switch (g_btnOnState) {
    case 0: if (onRaw) { g_btnOnMs = now; g_btnOnState = 1; } break;
    case 1:
      if (!onRaw) { g_btnOnState = 0; break; }
      if ((now - g_btnOnMs) >= DEBOUNCE_MS) {
        logAdd("[SEND] BTN ON pressed"); sendMotorCommandInternal(true); g_btnOnState = 2;
      }
      break;
    case 2: if (!onRaw) g_btnOnState = 0; break;
  }

  // ── Button OFF ────────────────────────────────────────────────────────
  bool offRaw = (digitalRead(PIN_BTN_OFF) == LOW);
  switch (g_btnOffState) {
    case 0: if (offRaw) { g_btnOffMs = now; g_btnOffState = 1; } break;
    case 1:
      if (!offRaw) { g_btnOffState = 0; break; }
      if ((now - g_btnOffMs) >= DEBOUNCE_MS) {
        logAdd("[SEND] BTN OFF pressed"); sendMotorCommandInternal(false); g_btnOffState = 2;
      }
      break;
    case 2: if (!offRaw) g_btnOffState = 0; break;
  }

  handleTimerTick();
  handleScheduler();
  handleBlink();
}
