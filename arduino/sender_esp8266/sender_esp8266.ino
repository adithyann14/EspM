/**
 * ════════════════════════════════════════════════════════════════════════
 *  SENDER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)   v5
 *
 *  Comms: plain Wi-Fi only (no ESP-NOW, no LoRa).
 *
 *  Wi-Fi AP "MotorControl" / "motor1234" — always up.  The RECEIVER joins
 *    this AP as a station; sender ⇄ receiver traffic is UDP on port 4210.
 *    The phone app also joins this AP and talks HTTP to 192.168.4.1.
 *  Wi-Fi STA — OPTIONAL.  Only started when home-router credentials are
 *    set (STA_SSID_DEFAULT/STA_PASS_DEFAULT or POST /api/wifi/config).
 *    With no credentials the radio runs AP-only, which is the most stable
 *    mode (an AP+STA radio that keeps scanning for a missing router
 *    interrupts the AP and drops packets).
 *
 *  Receiver address: learned from the source IP of its status packets
 *    (the phone is ALSO a station, so "first station in the list" would
 *    be wrong).  Until the first status arrives, commands go to the AP
 *    subnet broadcast address.
 *
 *  ── Pins ──────────────────────────────────────────────────────────────
 *  D1 (GPIO5)  → Button ON  (to GND, INPUT_PULLUP)
 *  D2 (GPIO4)  → Button OFF (to GND, INPUT_PULLUP)
 *  D3 (GPIO0)  → LED ON  (HIGH = motor ON,  only while receiver link is fresh)
 *  D7 (GPIO13) → LED OFF (HIGH = motor OFF, only while receiver link is fresh)
 *  D4 (GPIO2)  → On-board LED, active LOW: solid = receiver joined the AP,
 *                brief OFF blink on every button press / command sent
 *
 *  ── REST endpoints ────────────────────────────────────────────────────
 *  GET  /api/status
 *  POST /api/motor/on|off
 *  GET  /api/logs
 *  POST /api/timer/set|cancel   GET /api/timer/status
 *  POST /api/schedule/push|clear
 *  POST /api/time/sync
 *  POST /api/config/drytimeout   {dryRunSec:5-60}
 *  POST /api/wifi/config         {ssid,pass}
 *  GET  /api/wifi/status
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WiFiUdp.h>
#include <EEPROM.h>

extern "C" {
  #include <user_interface.h>
}

// ── Pins ──────────────────────────────────────────────────────────────────
#define PIN_BTN_ON   D1   // GPIO5
#define PIN_BTN_OFF  D2   // GPIO4
#define PIN_LED_ON   D3   // GPIO0  green LED  (motor ON)
#define PIN_LED_OFF  D7   // GPIO13 red LED    (motor OFF)
#define PIN_LED_LINK 2    // GPIO2  on-board LED, active LOW (= D4)

// ── Wi-Fi AP ──────────────────────────────────────────────────────────────
static const char*   AP_SSID = "MotorControl";
static const char*   AP_PASS = "motor1234";
static const uint8_t WIFI_CH = 1;           // AP channel (follows router if STA is joined)
static const uint16_t UDP_PORT = 4210;      // same port on sender and receiver

// ── Wi-Fi STA (home router) ───────────────────────────────────────────────
#ifndef STA_SSID_DEFAULT
  #define STA_SSID_DEFAULT ""
#endif
#ifndef STA_PASS_DEFAULT
  #define STA_PASS_DEFAULT ""
#endif
static char g_staSSID[33] = STA_SSID_DEFAULT;
static char g_staPass[33] = STA_PASS_DEFAULT;
static bool g_staConnected = false;

// ── EEPROM ────────────────────────────────────────────────────────────────
#define EE_SIZE          200
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
#define EE_STA_MAGIC     82
#define EE_STA_SSID      83
#define EE_STA_PASS     116
#define EE_STA_MAGIC_VAL 0xCA
#define EE_MAGIC_VAL     0xBE

static inline void eeWriteU32(int addr, uint32_t v) {
  EEPROM.write(addr,     (v      ) & 0xFF);
  EEPROM.write(addr + 1, (v >>  8) & 0xFF);
  EEPROM.write(addr + 2, (v >> 16) & 0xFF);
  EEPROM.write(addr + 3, (v >> 24) & 0xFF);
}
static inline uint32_t eeReadU32(int addr) {
  return (uint32_t)EEPROM.read(addr)
       | ((uint32_t)EEPROM.read(addr+1) <<  8)
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

// ── Packets (sent as UDP datagrams) ───────────────────────────────────────

typedef struct __attribute__((packed)) {
  uint8_t type; uint8_t cmd; uint32_t seq;
} CmdPacket;                                        // 0x01

typedef struct __attribute__((packed)) {
  uint8_t type; float current; uint8_t motorOn;
  uint8_t waterOk; uint8_t stall; uint32_t seq;
  int8_t  rssi;                                     // receiver's Wi-Fi RSSI to this AP (dBm)
} StatusPacket;                                     // 0x02

typedef struct __attribute__((packed)) {
  uint8_t type; char msg[59];
} LogPacket;                                        // 0x03

typedef struct __attribute__((packed)) {
  uint8_t type; uint32_t totalSec; uint32_t remainingSec;
  uint8_t autoRestart; uint8_t active;
} TimerPacket;                                      // 0x04

typedef struct __attribute__((packed)) {
  uint8_t type; uint8_t count;
  struct { uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad; } entries[8];
} SchedPacket;                                      // 0x05

typedef struct __attribute__((packed)) {
  uint8_t type; uint32_t epoch; uint8_t rtcEnabled;
} TimeSyncPacket;                                   // 0x06

typedef struct __attribute__((packed)) {
  uint8_t type; uint32_t dryRunMs;
} ConfigPacket;                                     // 0x07

// ── Motor / sensor state ──────────────────────────────────────────────────
static bool           g_motorOn  = false;
static volatile float g_currentA = 0.0f;
static volatile bool  g_waterOk  = true;
static volatile bool  g_stall    = false;
static uint32_t       g_cmdSeq   = 0;
static bool           g_linkLost = true;
static unsigned long  g_lastRxMs = 0;

// ── Wi-Fi link to receiver ─────────────────────────────────────────────────
static WiFiUDP          g_udp;
static IPAddress        g_receiverIp;                // learned from its status packets
static bool             g_receiverIpKnown = false;
static volatile int8_t  g_lastRssi        = 0;       // receiver ↔ AP, dBm
static volatile bool    g_hasRssi         = false;
static unsigned long    g_blinkUntil      = 0;       // on-board LED blink (command sent)
static unsigned long    g_staLastTryMs    = 0;
static bool             g_needResync      = false;   // push timer/schedule/time when receiver (re)appears
static bool             g_pendingAutoOn   = false;   // auto-restart ON, sent once receiver is heard

// ── Button debounce ────────────────────────────────────────────────────────
static uint8_t       g_btnOnState  = 0, g_btnOffState = 0;
static unsigned long g_btnOnMs     = 0, g_btnOffMs    = 0;
#define DEBOUNCE_MS 50UL

// ── Timer state ───────────────────────────────────────────────────────────
static bool          g_timerActive     = false;
static uint32_t      g_timerTotal      = 0;
static uint32_t      g_timerRemain     = 0;
static bool          g_timerAutoRst    = false;
static unsigned long g_timerLastTick   = 0;
static unsigned long g_timerLastSaveMs = 0;
#define TIMER_SAVE_INTERVAL_MS 10000UL

// ── Schedule state ────────────────────────────────────────────────────────
struct SchedEntry { uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad; };
static uint8_t     g_schedCount = 0;
static SchedEntry  g_scheds[8]  = {};
static int         g_lastActedMinute = -1;

// ── RTC / epoch ───────────────────────────────────────────────────────────
static bool          g_rtcEnabled = false;
static uint32_t      g_epoch      = 0;
static unsigned long g_epochMs    = 0;

static uint32_t currentEpoch() {
  return g_epoch + (uint32_t)((millis() - g_epochMs) / 1000UL);
}
static uint8_t epochDayBit(uint32_t e) {
  return (uint8_t)(((e / 86400UL) + 4UL) % 7UL);
}

// ── HTTP server ────────────────────────────────────────────────────────────
static ESP8266WebServer server(80);

// Forward declarations
static void eeSave();
static void sendMotorCommandInternal(bool on);
static void sendTimerPacket();
static void updateLEDs();

// ══════════════════════════════════════════════════════════════════════════
//  EEPROM
// ══════════════════════════════════════════════════════════════════════════

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
    int b = EE_SCHED_BASE + i * 8;
    EEPROM.write(b+0, g_scheds[i].startH);  EEPROM.write(b+1, g_scheds[i].startM);
    EEPROM.write(b+2, g_scheds[i].stopH);   EEPROM.write(b+3, g_scheds[i].stopM);
    EEPROM.write(b+4, g_scheds[i].days);    EEPROM.write(b+5, g_scheds[i].autoRestart);
    EEPROM.write(b+6, g_scheds[i].enabled); EEPROM.write(b+7, 0);
  }
  EEPROM.write(EE_MOTOR_WAS_ON, g_motorOn ? 1 : 0);
  EEPROM.commit();
}

static void eeLoad() {
  if (EEPROM.read(EE_MAGIC) != EE_MAGIC_VAL) {
    logAdd("[SEND] EEPROM blank — using defaults"); return;
  }
  g_timerTotal   = eeReadU32(EE_TIMER_TOTAL);
  g_timerAutoRst = EEPROM.read(EE_TIMER_AUTO)  != 0;
  bool wasActive = EEPROM.read(EE_TIMER_ACTIVE) != 0;
  g_timerRemain  = eeReadU32(EE_TIMER_REM);
  g_rtcEnabled   = EEPROM.read(EE_RTC_FLAG)     != 0;
  g_epoch        = eeReadU32(EE_EPOCH);
  g_epochMs      = millis();
  g_schedCount   = min((uint8_t)8, EEPROM.read(EE_SCHED_COUNT));
  for (uint8_t i = 0; i < 8; i++) {
    int b = EE_SCHED_BASE + i * 8;
    g_scheds[i].startH      = EEPROM.read(b+0); g_scheds[i].startM      = EEPROM.read(b+1);
    g_scheds[i].stopH       = EEPROM.read(b+2); g_scheds[i].stopM       = EEPROM.read(b+3);
    g_scheds[i].days        = EEPROM.read(b+4); g_scheds[i].autoRestart = EEPROM.read(b+5);
    g_scheds[i].enabled     = EEPROM.read(b+6);
  }
  bool motorWasOn = EEPROM.read(EE_MOTOR_WAS_ON) != 0;
  logFmt("[SEND] EEPROM loaded  timer=%lus(%s)  scheds=%d  motorWasOn=%d",
    (unsigned long)g_timerTotal, wasActive?"active":"idle",
    (int)g_schedCount, (int)motorWasOn);
  if (wasActive && g_timerAutoRst && g_timerRemain > 0) {
    g_timerActive   = true;
    g_timerLastTick = millis();
    logFmt("[SEND] auto-restart timer: %lus remaining", (unsigned long)g_timerRemain);
  }
  // The receiver has not joined our AP yet at boot, so anything sent now would
  // be lost.  Defer: the loop sends it as soon as the receiver is heard.
  if (motorWasOn && g_timerAutoRst) g_pendingAutoOn = true;
}

static void eeSaveStaCreds() {
  for (int i = 0; i < 33; i++) EEPROM.write(EE_STA_SSID + i, (uint8_t)g_staSSID[i]);
  for (int i = 0; i < 33; i++) EEPROM.write(EE_STA_PASS + i, (uint8_t)g_staPass[i]);
  EEPROM.write(EE_STA_MAGIC, EE_STA_MAGIC_VAL);
  EEPROM.commit();
  logFmt("[WIFI] STA creds saved  SSID='%s'", g_staSSID);
}

static void eeLoadStaCreds() {
  if (EEPROM.read(EE_STA_MAGIC) != EE_STA_MAGIC_VAL) {
    if (g_staSSID[0]) logFmt("[WIFI] compile-time STA SSID='%s'", g_staSSID);
    else              logAdd("[WIFI] No STA creds — AP-only mode (192.168.4.1)");
    return;
  }
  for (int i = 0; i < 33; i++) g_staSSID[i] = (char)EEPROM.read(EE_STA_SSID + i);
  for (int i = 0; i < 33; i++) g_staPass[i] = (char)EEPROM.read(EE_STA_PASS + i);
  g_staSSID[32] = '\0';
  g_staPass[32] = '\0';
  logFmt("[WIFI] EEPROM STA creds  SSID='%s'", g_staSSID);
}

// ══════════════════════════════════════════════════════════════════════════
//  Wi-Fi / UDP transport
// ══════════════════════════════════════════════════════════════════════════

static inline bool isApSubnet(IPAddress ip) {
  IPAddress me = WiFi.softAPIP();
  return ip[0] == me[0] && ip[1] == me[1] && ip[2] == me[2];
}

// Unicast to the receiver once its address is known (gets 802.11 ACK/retries);
// before that, broadcast on the AP subnet.  Never guesses "first station":
// the phone is a station too.
static void commsSend(uint8_t* data, size_t len) {
  IPAddress dst;
  if (g_receiverIpKnown) dst = g_receiverIp;
  else { IPAddress me = WiFi.softAPIP(); dst = IPAddress(me[0], me[1], me[2], 255); }
  g_udp.beginPacket(dst, UDP_PORT);
  g_udp.write(data, len);
  g_udp.endPacket();
}

// ══════════════════════════════════════════════════════════════════════════
//  Incoming packet handler
// ══════════════════════════════════════════════════════════════════════════

static void processIncomingPacket(uint8_t* data, int len) {
  if (len < 1) return;
  switch (data[0]) {
    case 0x02: {
      if (len < (int)sizeof(StatusPacket)) return;
      StatusPacket pkt; memcpy(&pkt, data, sizeof(pkt));

      bool prevMotor = g_motorOn;
      bool prevWater = g_waterOk;
      bool prevStall = g_stall;
      if (g_linkLost) {                       // first packet, or back after ≥10 s silence
        g_needResync = true;
        logAdd("[WIFI] receiver link UP");
      }

      g_motorOn  = (pkt.motorOn != 0);
      g_currentA = pkt.current;
      g_waterOk  = (pkt.waterOk != 0);
      g_stall    = (pkt.stall   != 0);
      if (pkt.rssi < 0) { g_lastRssi = pkt.rssi; g_hasRssi = true; }
      g_lastRxMs = millis();
      g_linkLost = false;

      if (prevMotor != g_motorOn)
        logFmt("[RECV] relay → %s  I=%.2fA", g_motorOn ? "ON" : "OFF", g_currentA);
      if (prevWater != g_waterOk)
        logFmt("[RECV] water → %s", g_waterOk ? "YES" : "NO");
      if (!prevStall && g_stall)
        logAdd("[RECV] ⚠ STALL — motor ON, current ≈ 0 for 5 s");
      if (prevStall && !g_stall)
        logAdd("[RECV] stall cleared");
      break;
    }
    case 0x03: {
      if (len < 2) return;
      LogPacket pkt; memset(&pkt, 0, sizeof(pkt));
      memcpy(&pkt, data, min((size_t)len, sizeof(pkt)));
      pkt.msg[sizeof(pkt.msg)-1] = '\0';
      logAdd(pkt.msg);
      g_lastRxMs = millis();
      g_linkLost = false;
      break;
    }
    default: break;
  }
}

// Drain pending datagrams.  Only hosts on the AP subnet are accepted, and
// the receiver's address is learned from its status/log packets.
static void pollUdp() {
  for (uint8_t n = 0; n < 6; n++) {
    int len = g_udp.parsePacket();
    if (len <= 0) return;
    uint8_t buf[250];
    int got = g_udp.read(buf, sizeof(buf));
    if (got <= 0) continue;
    IPAddress from = g_udp.remoteIP();
    if (!isApSubnet(from)) continue;
    if (buf[0] == 0x02 || buf[0] == 0x03) {          // only the receiver sends these
      if (!g_receiverIpKnown || g_receiverIp != from) {
        g_receiverIp = from; g_receiverIpKnown = true;
        logFmt("[WIFI] receiver at %s", from.toString().c_str());
      }
      processIncomingPacket(buf, got);
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  LED helpers
// ══════════════════════════════════════════════════════════════════════════

// "Joined" = at least one station on the AP.  "Fresh" = receiver status heard
// within 3 s.  Motor LEDs show the REAL motor state only while fresh; both
// go dark when the link is down so a stale LED can never lie.
static void updateLEDs() {
  unsigned long now = millis();
  bool joined   = WiFi.softAPgetStationNum() > 0;
  bool fresh    = joined && g_lastRxMs > 0 && (now - g_lastRxMs) < 3000UL;
  bool blinking = (long)(now - g_blinkUntil) < 0;
  digitalWrite(PIN_LED_LINK, (joined && !blinking) ? LOW : HIGH);     // active LOW
  digitalWrite(PIN_LED_ON,  (fresh &&  g_motorOn) ? HIGH : LOW);
  digitalWrite(PIN_LED_OFF, (fresh && !g_motorOn) ? HIGH : LOW);
}

// ══════════════════════════════════════════════════════════════════════════
//  Packet send helpers
// ══════════════════════════════════════════════════════════════════════════

static void sendTimerPacket() {
  TimerPacket p;
  p.type = 0x04; p.totalSec = g_timerTotal; p.remainingSec = g_timerRemain;
  p.autoRestart = g_timerAutoRst ? 1 : 0; p.active = g_timerActive ? 1 : 0;
  commsSend((uint8_t*)&p, sizeof(p));
}

static void sendSchedPacket() {
  SchedPacket p; memset(&p, 0, sizeof(p));
  p.type = 0x05; p.count = g_schedCount;
  for (uint8_t i = 0; i < 8; i++) p.entries[i] = *(decltype(&p.entries[0]))&g_scheds[i];
  commsSend((uint8_t*)&p, sizeof(p));
}

static void sendConfigPacket(uint32_t dryRunMs) {
  ConfigPacket p;
  p.type = 0x07; p.dryRunMs = dryRunMs;
  commsSend((uint8_t*)&p, sizeof(p));
  logFmt("[SEND] dry-run timeout → %lums", (unsigned long)dryRunMs);
}

static void sendTimeSyncPacket() {
  TimeSyncPacket p;
  p.type = 0x06; p.epoch = currentEpoch(); p.rtcEnabled = g_rtcEnabled ? 1 : 0;
  commsSend((uint8_t*)&p, sizeof(p));
}

// ══════════════════════════════════════════════════════════════════════════
//  Motor command
// ══════════════════════════════════════════════════════════════════════════

static void sendMotorCommandInternal(bool on) {
  CmdPacket pkt;
  pkt.type = 0x01; pkt.cmd = on ? 0x01 : 0x02; pkt.seq = ++g_cmdSeq;
  g_blinkUntil = millis() + 200UL;
  if (WiFi.softAPgetStationNum() == 0) {
    logFmt("[SEND] Motor %s NOT sent — nothing joined the AP", on ? "ON" : "OFF");
    return;
  }
  logFmt("[SEND] Motor %s  seq=%lu  → %s", on ? "ON" : "OFF", (unsigned long)g_cmdSeq,
         g_receiverIpKnown ? g_receiverIp.toString().c_str() : "broadcast");
  // 3 copies (receiver drops repeats by seq) — covers a lost datagram.
  for (uint8_t i = 0; i < 3; i++) {
    commsSend((uint8_t*)&pkt, sizeof(pkt));
    delay(40);
    pollUdp();                         // keep draining while we wait
  }
  EEPROM.write(EE_MOTOR_WAS_ON, on ? 1 : 0);
  EEPROM.commit();
}

// ══════════════════════════════════════════════════════════════════════════
//  Timer tick
// ══════════════════════════════════════════════════════════════════════════

static void handleTimerTick() {
  if (!g_timerActive) return;
  unsigned long now     = millis();
  unsigned long elapsed = (now - g_timerLastTick) / 1000UL;
  if (elapsed == 0) return;
  g_timerLastTick = now - ((now - g_timerLastTick) % 1000UL);
  if (elapsed >= g_timerRemain) {
    g_timerRemain = 0; g_timerActive = false;
    logAdd("[SEND] ⏰ timer expired — Motor OFF");
    sendMotorCommandInternal(false);
    EEPROM.write(EE_TIMER_ACTIVE, 0); eeWriteU32(EE_TIMER_REM, 0); EEPROM.commit();
    sendTimerPacket();
  } else {
    g_timerRemain -= (uint32_t)elapsed;
    if (now - g_timerLastSaveMs >= TIMER_SAVE_INTERVAL_MS) {
      g_timerLastSaveMs = now;
      eeWriteU32(EE_TIMER_REM, g_timerRemain); EEPROM.commit();
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Scheduler tick
// ══════════════════════════════════════════════════════════════════════════

static unsigned long g_schedLastCheckMs = 0;

static void handleScheduler() {
  unsigned long now = millis();
  if (now - g_schedLastCheckMs < 15000UL) return;
  g_schedLastCheckMs = now;
  if (g_epoch == 0) return;
  uint32_t ep  = currentEpoch();
  uint16_t tod = (uint16_t)((ep % 86400UL) / 60UL);
  uint8_t  day = epochDayBit(ep);
  if ((int)tod == g_lastActedMinute) return;
  for (uint8_t i = 0; i < g_schedCount; i++) {
    SchedEntry& s = g_scheds[i];
    if (!s.enabled) continue;
    if (!(s.days & (1 << day))) continue;
    uint16_t startTod = s.startH * 60 + s.startM;
    uint16_t stopTod  = s.stopH  * 60 + s.stopM;
    if (tod == startTod) {
      logFmt("[SCHED] slot %d → ON  (%02d:%02d)", (int)i, (int)s.startH, (int)s.startM);
      sendMotorCommandInternal(true);  g_lastActedMinute = (int)tod; break;
    }
    if (tod == stopTod) {
      logFmt("[SCHED] slot %d → OFF (%02d:%02d)", (int)i, (int)s.stopH, (int)s.stopM);
      sendMotorCommandInternal(false); g_lastActedMinute = (int)tod; break;
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  HTTP helpers + status JSON
// ══════════════════════════════════════════════════════════════════════════

static void addCorsHeaders() {
  server.sendHeader("Access-Control-Allow-Origin",  "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers", "Content-Type");
}

static inline bool isLinkOk() {
  return (g_lastRxMs > 0) && (millis() - g_lastRxMs < 10000UL);
}

static String buildStatusJson() {
  String j = "{";
  j += "\"motorOn\":"    + String(g_motorOn  ? "true":"false") + ",";
  j += "\"current\":"    + String(g_currentA, 2)               + ",";
  j += "\"isRunning\":"  + String(g_motorOn  ? "true":"false") + ",";
  j += "\"waterOk\":"    + String(g_waterOk  ? "true":"false") + ",";
  j += "\"stall\":"      + String(g_stall    ? "true":"false") + ",";
  j += "\"linkOk\":"     + String(isLinkOk() ? "true":"false") + ",";
  j += "\"commsMode\":\"WiFi\",";
  if (g_staConnected && WiFi.status() == WL_CONNECTED)
    j += "\"staIp\":\"" + WiFi.localIP().toString() + "\",";
  else
    j += "\"staIp\":null,";
  // MODULE E: Wi-Fi RSSI of the receiver ↔ this AP (dBm, negative), reported by the receiver
  if (g_hasRssi)
    j += "\"rssi\":" + String((int)g_lastRssi);
  else
    j += "\"rssi\":null";
  j += "}";
  return j;
}

// ── Route handlers ────────────────────────────────────────────────────────
void handleStatus()      { addCorsHeaders(); server.send(200,"application/json",buildStatusJson()); }
void handleMotorOn()     { sendMotorCommandInternal(true);  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}"); }
void handleMotorOff()    { sendMotorCommandInternal(false); addCorsHeaders(); server.send(200,"application/json","{\"success\":true}"); }
void handleAlerts()      { addCorsHeaders(); server.send(200,"application/json","{\"alerts\":[]}"); }
void handleAlertsClear() { addCorsHeaders(); server.send(200,"application/json","{\"success\":true}"); }
void handleNotFound()    { server.send(404,"application/json","{\"error\":\"not found\"}"); }

void handleWifiConfig() {
  addCorsHeaders();
  String body = server.arg("plain");
  auto extractStr = [&](const char* key) -> String {
    int ki = body.indexOf(key); if (ki < 0) return "";
    int q1 = body.indexOf('"', ki + strlen(key));
    if (q1 < 0) return "";
    int q2 = body.indexOf('"', q1 + 1);
    if (q2 < 0) return "";
    return body.substring(q1 + 1, q2);
  };
  String ssid = extractStr("\"ssid\":");
  String pass = extractStr("\"pass\":");
  if (ssid.length() == 0 || ssid.length() > 32) {
    server.send(400,"application/json","{\"error\":\"ssid must be 1-32 chars\"}"); return;
  }
  ssid.toCharArray(g_staSSID, sizeof(g_staSSID));
  pass.toCharArray(g_staPass, sizeof(g_staPass));
  eeSaveStaCreds();
  if (WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(AP_SSID, AP_PASS, WIFI_CH);   // re-assert AP after the mode change
  }
  WiFi.begin(g_staSSID, g_staPass);
  g_staLastTryMs = millis();
  logFmt("[WIFI] STA: connecting to '%s'…", g_staSSID);
  addCorsHeaders();
  server.send(200,"application/json","{\"success\":true,\"message\":\"Connecting — poll /api/wifi/status in 10s\"}");
}

void handleWifiStatus() {
  addCorsHeaders();
  bool connected = (WiFi.status() == WL_CONNECTED);
  String apIp = WiFi.softAPIP().toString();
  String j = "{";
  j += "\"staConnected\":" + String(connected ? "true":"false") + ",";
  j += connected ? ("\"staIp\":\"" + WiFi.localIP().toString() + "\",") : "\"staIp\":null,";
  j += "\"apIp\":\"" + apIp + "\",";
  j += g_staSSID[0] ? ("\"staSSID\":\"" + String(g_staSSID) + "\"") : "\"staSSID\":null";
  j += "}";
  server.send(200,"application/json",j);
}

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
      char c = *p;
      if      (c == '"')  j += "\\\"";
      else if (c == '\\') j += "\\\\";
      else if (c == '\n') j += "\\n";
      else if (c == '\r') j += "\\r";
      else if (c == '\t') j += "\\t";
      else if ((uint8_t)c < 0x20) { /* skip other control chars */ }
      else                j += c;
    }
    j += '"';
  }
  j += "]}";
  server.send(200,"application/json",j);
}

void handleTimerSet() {
  String body   = server.arg("plain");
  int    secIdx = body.indexOf("\"seconds\":");
  int    autIdx = body.indexOf("\"autoRestart\":");
  if (secIdx < 0) { server.send(400,"application/json","{\"error\":\"missing seconds\"}"); return; }
  uint32_t sec = (uint32_t)body.substring(secIdx + 10).toInt();
  bool     ar  = (autIdx >= 0) && (body.substring(autIdx + 14).toInt() != 0);
  g_timerTotal = sec; g_timerRemain = sec; g_timerAutoRst = ar;
  g_timerActive = true; g_timerLastTick = millis(); g_timerLastSaveMs = 0;
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
  j += "\"active\":"      + String(g_timerActive  ? "true":"false") + ",";
  j += "\"remaining\":"   + String(g_timerRemain)                   + ",";
  j += "\"total\":"       + String(g_timerTotal)                    + ",";
  j += "\"autoRestart\":" + String(g_timerAutoRst ? "true":"false");
  j += "}";
  server.send(200,"application/json",j);
}

void handleSchedPush() {
  String body   = server.arg("plain");
  int    cntIdx = body.indexOf("\"count\":");
  int    entIdx = body.indexOf("\"entries\":");
  if (cntIdx < 0 || entIdx < 0) { server.send(400,"application/json","{\"error\":\"malformed\"}"); return; }
  g_schedCount = (uint8_t)min(8L, body.substring(cntIdx + 8).toInt());
  int pos = entIdx + 10;
  for (uint8_t i = 0; i < g_schedCount; i++) {
    // Guard: if we can't find the next entry object, stop — don't re-parse
    // from the beginning (indexOf({,−1) would wrap and corrupt schedules).
    int entryStart = body.indexOf("{", pos);
    if (entryStart < 0) { g_schedCount = i; break; }
    pos = entryStart;

    SchedEntry& s = g_scheds[i];
    auto rd = [&](const char* k) -> uint8_t {
      int ki = body.indexOf(k, pos); if (ki < 0) return 0;
      return (uint8_t)body.substring(ki + (int)strlen(k)).toInt();
    };
    s.startH = rd("\"startH\":"); s.startM = rd("\"startM\":");
    s.stopH  = rd("\"stopH\":");  s.stopM  = rd("\"stopM\":");
    s.days        = rd("\"days\":");
    s.autoRestart = rd("\"autoRestart\":");
    s.enabled     = rd("\"enabled\":");
    s.pad         = 0;

    pos = body.indexOf("{", pos + 1);   // advance past current entry
    if (pos < 0) { if (i + 1 < g_schedCount) g_schedCount = i + 1; break; }
  }
  eeSave();
  logFmt("[SCHED] pushed %d schedule(s)", (int)g_schedCount);
  sendSchedPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

void handleSchedClear() {
  g_schedCount = 0; memset(g_scheds, 0, sizeof(g_scheds)); eeSave();
  logAdd("[SCHED] cleared"); sendSchedPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

void handleTimeSync() {
  String body   = server.arg("plain");
  int    epIdx  = body.indexOf("\"epoch\":");
  int    rtcIdx = body.indexOf("\"rtcEnabled\":");
  if (epIdx < 0) { server.send(400,"application/json","{\"error\":\"missing epoch\"}"); return; }
  g_epoch = (uint32_t)body.substring(epIdx + 8).toInt();
  g_epochMs = millis();
  g_rtcEnabled = (rtcIdx >= 0) && (body.substring(rtcIdx + 13).toInt() != 0);
  EEPROM.write(EE_RTC_FLAG, g_rtcEnabled ? 1 : 0);
  eeWriteU32(EE_EPOCH, g_epoch); EEPROM.commit();
  sendTimeSyncPacket();
  addCorsHeaders(); server.send(200,"application/json","{\"success\":true}");
}

void handleDryTimeout() {
  String body   = server.arg("plain");
  int    secIdx = body.indexOf("\"dryRunSec\":");
  if (secIdx < 0) {
    server.send(400,"application/json","{\"error\":\"missing dryRunSec\"}"); return;
  }
  int secs = body.substring(secIdx + 12).toInt();
  secs = max(5, min(60, secs));
  sendConfigPacket((uint32_t)secs * 1000UL);
  addCorsHeaders();
  server.send(200,"application/json",
    "{\"success\":true,\"dryRunSec\":" + String(secs) + "}");
}

// ══════════════════════════════════════════════════════════════════════════
//  Setup
// ══════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(100);
  logAdd("[SEND] boot v5  Wi-Fi (UDP)");

  pinMode(PIN_BTN_ON,  INPUT_PULLUP);
  pinMode(PIN_BTN_OFF, INPUT_PULLUP);
  pinMode(PIN_LED_ON,  OUTPUT);
  pinMode(PIN_LED_OFF, OUTPUT);
  pinMode(PIN_LED_LINK, OUTPUT);
  digitalWrite(PIN_LED_ON, LOW); digitalWrite(PIN_LED_OFF, LOW);
  digitalWrite(PIN_LED_LINK, HIGH);

  // Command sequence numbers start at a random point so that after a sender
  // reboot the receiver's "same seq = duplicate" filter can't swallow the
  // first command (seq 1 again).
  g_cmdSeq = RANDOM_REG32;

  EEPROM.begin(EE_SIZE);
  eeLoadStaCreds();

  // ── Wi-Fi ─────────────────────────────────────────────────────────────
  // AP-only unless router credentials exist (most stable for the receiver).
  WiFi.persistent(false);
  bool wantSta = (g_staSSID[0] != '\0');
  WiFi.mode(wantSta ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS, WIFI_CH);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   // modem sleep drops incoming UDP
  WiFi.setOutputPower(20.5f);           // max power: 20.5 dBm
  wifi_set_phy_mode(PHY_MODE_11B);      // 802.11b for best range
  logFmt("[SEND] AP  IP=%s  SSID=%s  mode=%s", WiFi.softAPIP().toString().c_str(),
         AP_SSID, wantSta ? "AP+STA" : "AP-only");

  if (wantSta) {
    logFmt("[WIFI] STA connecting to '%s'…", g_staSSID);
    WiFi.begin(g_staSSID, g_staPass);
    g_staLastTryMs = millis();
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - t0) < 8000UL) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      g_staConnected = true;
      logFmt("[WIFI] STA connected  IP=%s", WiFi.localIP().toString().c_str());
    } else {
      logFmt("[WIFI] STA '%s' not reachable — AP still up (192.168.4.1)", g_staSSID);
    }
  }

  g_udp.begin(UDP_PORT);
  logFmt("[SEND] UDP listening on %u", (unsigned)UDP_PORT);

  eeLoad();

  // HTTP routes
  server.on("/api/status",            HTTP_GET,  handleStatus);
  server.on("/api/motor/on",          HTTP_POST, handleMotorOn);
  server.on("/api/motor/off",         HTTP_POST, handleMotorOff);
  server.on("/api/logs",              HTTP_GET,  handleLogs);
  server.on("/api/alerts",            HTTP_GET,  handleAlerts);
  server.on("/api/alerts/clear",      HTTP_POST, handleAlertsClear);
  server.on("/api/timer/set",         HTTP_POST, handleTimerSet);
  server.on("/api/timer/cancel",      HTTP_POST, handleTimerCancel);
  server.on("/api/timer/status",      HTTP_GET,  handleTimerStatus);
  server.on("/api/schedule/push",     HTTP_POST, handleSchedPush);
  server.on("/api/schedule/clear",    HTTP_POST, handleSchedClear);
  server.on("/api/time/sync",         HTTP_POST, handleTimeSync);
  server.on("/api/config/drytimeout", HTTP_POST, handleDryTimeout);
  server.on("/api/wifi/config",       HTTP_POST, handleWifiConfig);
  server.on("/api/wifi/status",       HTTP_GET,  handleWifiStatus);
  auto opt = [](){ server.sendHeader("Access-Control-Allow-Origin","*");
                   server.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
                   server.sendHeader("Access-Control-Allow-Headers","Content-Type");
                   server.send(204); };
  server.on("/api/motor/on",          HTTP_OPTIONS, opt);
  server.on("/api/motor/off",         HTTP_OPTIONS, opt);
  server.on("/api/timer/set",         HTTP_OPTIONS, opt);
  server.on("/api/timer/cancel",      HTTP_OPTIONS, opt);
  server.on("/api/schedule/push",     HTTP_OPTIONS, opt);
  server.on("/api/schedule/clear",    HTTP_OPTIONS, opt);
  server.on("/api/time/sync",         HTTP_OPTIONS, opt);
  server.on("/api/config/drytimeout", HTTP_OPTIONS, opt);
  server.on("/api/wifi/config",       HTTP_OPTIONS, opt);
  server.onNotFound(handleNotFound);
  server.begin();

  // Boot blink 2× = SENDER identifier
  for (int i = 0; i < 2; i++) {
    digitalWrite(PIN_LED_ON, HIGH); delay(220);
    digitalWrite(PIN_LED_ON, LOW);  delay(150);
  }
  delay(300);

  updateLEDs();
  logAdd("[SEND] ready — Wi-Fi only");
}

// ══════════════════════════════════════════════════════════════════════════
//  Main loop
// ══════════════════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();
  pollUdp();
  unsigned long now = millis();
  g_staConnected = (WiFi.status() == WL_CONNECTED);

  // ── Home-router STA retry (rare: every scan briefly disturbs the AP) ──
  if (g_staSSID[0] && !g_staConnected && (now - g_staLastTryMs) > 60000UL) {
    g_staLastTryMs = now;
    WiFi.begin(g_staSSID, g_staPass);
  }

  // ── Link-lost flag ────────────────────────────────────────────────────
  if (g_lastRxMs > 0 && (now - g_lastRxMs) > 10000UL && !g_linkLost) {
    g_linkLost = true;
    logAdd("[SEND] ⚠ link lost (no RX from receiver)");
  }

  // ── Receiver just (re)appeared: bring it up to date ───────────────────
  if (g_needResync && !g_linkLost) {
    g_needResync = false;
    if (g_epoch != 0)   sendTimeSyncPacket();
    if (g_schedCount)   sendSchedPacket();
    if (g_timerActive)  sendTimerPacket();   // never send "inactive": it would cancel
                                             // the receiver's own running countdown
    logAdd("[SEND] receiver resynced (time / schedule / timer)");
  }
  if (g_pendingAutoOn && !g_linkLost && g_lastRxMs > 0) {
    g_pendingAutoOn = false;
    logAdd("[SEND] auto-restart: Motor ON");
    sendMotorCommandInternal(true);
  }

  // ── Button ON ─────────────────────────────────────────────────────────
  bool onRaw = (digitalRead(PIN_BTN_ON) == LOW);
  switch (g_btnOnState) {
    case 0: if (onRaw) { g_btnOnMs = now; g_btnOnState = 1; } break;
    case 1:
      if (!onRaw) { g_btnOnState = 0; break; }
      if ((now - g_btnOnMs) >= DEBOUNCE_MS) {
        logAdd("[SEND] BTN ON"); sendMotorCommandInternal(true); g_btnOnState = 2;
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
        logAdd("[SEND] BTN OFF"); sendMotorCommandInternal(false); g_btnOffState = 2;
      }
      break;
    case 2: if (!offRaw) g_btnOffState = 0; break;
  }

  handleTimerTick();
  handleScheduler();
  updateLEDs();
}
