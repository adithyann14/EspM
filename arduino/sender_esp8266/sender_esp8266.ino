/**
 * ════════════════════════════════════════════════════════════════════════
 *  SENDER  —  NodeMCU ESP8266  v2 + OTA + STA/AP toggle
 *
 *  ── Network modes ────────────────────────────────────────────────────
 *  AP mode  (default): ESP creates "MotorControl" hotspot. Phone joins it.
 *  STA mode           : ESP joins home Wi-Fi. Phone must be on same network.
 *
 *  Toggle: hold D1 (ON) + D2 (OFF) together for 10 s → mode flips,
 *          saved to EEPROM, device reboots. Both LEDs flash during count.
 *
 *  ── OTA update (ESP8266HTTPUpdateServer at /update) ──────────────────
 *  Sender: always ready. POST .bin to http://<IP>/update  user=admin
 *  Receiver OTA: POST /api/ota/receiver {"pass":"..."} → sender sends
 *                ESP-NOW OtaEnablePacket (0x07) → receiver becomes
 *                "MCReceiver-OTA" AP and serves its own /update.
 *
 *  ── Credentials to edit before flashing ─────────────────────────────
 *  #define HOME_WIFI_SSID   "YourHomeSSID"
 *  #define HOME_WIFI_PASS   "YourHomePass"
 *  #define OTA_HTTP_PASS    "motor123"     ← must match app OTA password
 *
 *  ── EEPROM layout (128 B) ────────────────────────────────────────────
 *   0   magic(0xBE)  1-4  timer total   5   autoRestart  6   wasActive
 *   7-10 remaining  11   RTC flag      12-15 epoch       16  sched count
 *  17-80 scheds×8   81   motorWasOn    82   staMode
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
#include <EEPROM.h>

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>
}

// ── LoRa DEAD CODE ────────────────────────────────────────────────────────
// #include <SPI.h>  #include <LoRa.h>
// ─────────────────────────────────────────────────────────────────────────

// ══ USER CONFIGURATION ═══════════════════════════════════════════════════
#define HOME_WIFI_SSID  "YourHomeSSID"   // ← STA mode: edit before flash
#define HOME_WIFI_PASS  "YourHomePass"   // ← STA mode: edit before flash
#define OTA_HTTP_USER   "admin"          // OTA update username (fixed)
#define OTA_HTTP_PASS   "motor123"       // ← must match app OTA password field
// ═════════════════════════════════════════════════════════════════════════

// ── Pins ──────────────────────────────────────────────────────────────────
#define PIN_BTN_ON   D1   // GPIO5
#define PIN_BTN_OFF  D2   // GPIO4
#define PIN_LED_ON   D3   // GPIO0  green (motor ON)
#define PIN_LED_OFF  3    // GPIO3/RX  red  (motor OFF)  — TX-only serial

// ── AP credentials ────────────────────────────────────────────────────────
static const char*   AP_SSID = "MotorControl";
static const char*   AP_PASS = "motor1234";
static const uint8_t WIFI_CH = 1;

// ── EEPROM ────────────────────────────────────────────────────────────────
#define EE_SIZE         128
#define EE_MAGIC          0
#define EE_TIMER_TOTAL    1
#define EE_TIMER_AUTO     5
#define EE_TIMER_ACTIVE   6
#define EE_TIMER_REM      7
#define EE_RTC_FLAG      11
#define EE_EPOCH         12
#define EE_SCHED_COUNT   16
#define EE_SCHED_BASE    17
#define EE_MOTOR_WAS_ON  81
#define EE_STA_MODE      82   // 0 = AP (default)  1 = STA/server
#define EE_MAGIC_VAL   0xBE

static inline void eeWriteU32(int a, uint32_t v) {
  EEPROM.write(a,   v        & 0xFF);
  EEPROM.write(a+1, (v>> 8) & 0xFF);
  EEPROM.write(a+2, (v>>16) & 0xFF);
  EEPROM.write(a+3, (v>>24) & 0xFF);
}
static inline uint32_t eeReadU32(int a) {
  return (uint32_t)EEPROM.read(a)
       |((uint32_t)EEPROM.read(a+1)<< 8)
       |((uint32_t)EEPROM.read(a+2)<<16)
       |((uint32_t)EEPROM.read(a+3)<<24);
}

// ── Log ring-buffer ────────────────────────────────────────────────────────
#define LOG_ENTRIES 30
#define LOG_LEN     90
static char    g_log[LOG_ENTRIES][LOG_LEN];
static uint8_t g_logHead  = 0;
static uint8_t g_logCount = 0;

static void logAdd(const char* msg) {
  Serial.println(msg);
  strncpy(g_log[g_logHead], msg, LOG_LEN-1);
  g_log[g_logHead][LOG_LEN-1] = '\0';
  g_logHead = (g_logHead+1) % LOG_ENTRIES;
  if (g_logCount < LOG_ENTRIES) g_logCount++;
}
static void logFmt(const char* fmt, ...) {
  char buf[LOG_LEN]; va_list ap;
  va_start(ap,fmt); vsnprintf(buf,sizeof(buf),fmt,ap); va_end(ap);
  logAdd(buf);
}

// ── ESP-NOW packets ────────────────────────────────────────────────────────
static uint8_t BCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct __attribute__((packed)) {
  uint8_t type; uint8_t cmd; uint32_t seq;
} CmdPacket;                                    // 0x01

typedef struct __attribute__((packed)) {
  uint8_t type; float current; uint8_t motorOn;
  uint8_t waterOk; uint8_t stall; uint32_t seq;
} StatusPacket;                                  // 0x02

typedef struct __attribute__((packed)) {
  uint8_t type; char msg[59];
} LogPacket;                                     // 0x03

typedef struct __attribute__((packed)) {
  uint8_t type; uint32_t totalSec; uint32_t remainingSec;
  uint8_t autoRestart; uint8_t active;
} TimerPacket;                                   // 0x04

typedef struct __attribute__((packed)) {
  uint8_t type; uint8_t count;
  struct { uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad; } entries[8];
} SchedPacket;                                   // 0x05

typedef struct __attribute__((packed)) {
  uint8_t type; uint32_t epoch; uint8_t rtcEnabled;
} TimeSyncPacket;                                // 0x06

typedef struct __attribute__((packed)) {
  uint8_t type; char pass[32];
} OtaEnablePacket;                               // 0x07

// ── State ─────────────────────────────────────────────────────────────────
static bool          g_motorOn     = false;
static volatile float g_currentA   = 0.0f;
static volatile bool  g_waterOk    = true;
static volatile bool  g_stall      = false;
static uint32_t       g_cmdSeq     = 0;
static unsigned long  g_lastRecvMs = 0;
static bool           g_linkLost   = true;
static bool           g_staMode    = false;   // loaded from EEPROM

// ── Buttons (3-state debounce) ────────────────────────────────────────────
static uint8_t       g_btnOnState  = 0;
static unsigned long g_btnOnMs     = 0;
static uint8_t       g_btnOffState = 0;
static unsigned long g_btnOffMs    = 0;
#define DEBOUNCE_MS 50UL

// ── Dual-button hold → mode toggle ───────────────────────────────────────
static bool          g_dualActive  = false;
static unsigned long g_dualStart   = 0;
#define MODE_HOLD_MS 10000UL   // 10 s

// ── LEDs ──────────────────────────────────────────────────────────────────
#define BLINK_ON_MS   80UL
#define BLINK_OFF_MS 120UL
static volatile bool g_blinkReq    = false;
static unsigned long g_blinkPhaseMs= 0;
static uint8_t       g_blinkPhase  = 0;

// ── Timer ─────────────────────────────────────────────────────────────────
static bool          g_timerActive   = false;
static uint32_t      g_timerTotal    = 0;
static uint32_t      g_timerRemain   = 0;
static bool          g_timerAutoRst  = false;
static unsigned long g_timerLastTick = 0;
static unsigned long g_timerLastSave = 0;
#define TIMER_SAVE_MS 10000UL

// ── Scheduler ─────────────────────────────────────────────────────────────
struct SchedEntry { uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad; };
static uint8_t     g_schedCount = 0;
static SchedEntry  g_scheds[8]  = {};
static int         g_lastActedMin    = -1;
static unsigned long g_schedLastCheck= 0;

// ── RTC ───────────────────────────────────────────────────────────────────
static bool     g_rtcEnabled = false;
static uint32_t g_epoch      = 0;
static unsigned long g_epochMs = 0;

static uint32_t currentEpoch() {
  return g_epoch + (uint32_t)((millis()-g_epochMs)/1000UL);
}
static uint8_t epochDayBit(uint32_t e) {
  return (uint8_t)(((e/86400UL)+4UL)%7UL);
}

// ── HTTP ──────────────────────────────────────────────────────────────────
static ESP8266WebServer          server(80);
static ESP8266HTTPUpdateServer   httpUpdater;

// ═════════════════════════════════════════════════════════════════════════
//  EEPROM
// ═════════════════════════════════════════════════════════════════════════

static void eeSave() {
  EEPROM.write(EE_MAGIC,       EE_MAGIC_VAL);
  eeWriteU32(EE_TIMER_TOTAL,   g_timerTotal);
  EEPROM.write(EE_TIMER_AUTO,  g_timerAutoRst ? 1:0);
  EEPROM.write(EE_TIMER_ACTIVE,g_timerActive  ? 1:0);
  eeWriteU32(EE_TIMER_REM,     g_timerRemain);
  EEPROM.write(EE_RTC_FLAG,    g_rtcEnabled ? 1:0);
  eeWriteU32(EE_EPOCH,         currentEpoch());
  EEPROM.write(EE_SCHED_COUNT, g_schedCount);
  for (uint8_t i=0;i<8;i++){
    int b=EE_SCHED_BASE+i*8;
    EEPROM.write(b+0,g_scheds[i].startH); EEPROM.write(b+1,g_scheds[i].startM);
    EEPROM.write(b+2,g_scheds[i].stopH);  EEPROM.write(b+3,g_scheds[i].stopM);
    EEPROM.write(b+4,g_scheds[i].days);   EEPROM.write(b+5,g_scheds[i].autoRestart);
    EEPROM.write(b+6,g_scheds[i].enabled);EEPROM.write(b+7,0);
  }
  EEPROM.write(EE_MOTOR_WAS_ON, g_motorOn  ? 1:0);
  EEPROM.write(EE_STA_MODE,     g_staMode  ? 1:0);
  EEPROM.commit();
}

static void eeLoad() {
  if (EEPROM.read(EE_MAGIC) != EE_MAGIC_VAL) { logAdd("[SEND] EEPROM blank"); return; }
  g_timerTotal   = eeReadU32(EE_TIMER_TOTAL);
  g_timerAutoRst = EEPROM.read(EE_TIMER_AUTO)    != 0;
  bool wasActive = EEPROM.read(EE_TIMER_ACTIVE)   != 0;
  g_timerRemain  = eeReadU32(EE_TIMER_REM);
  g_rtcEnabled   = EEPROM.read(EE_RTC_FLAG)       != 0;
  g_epoch        = eeReadU32(EE_EPOCH);
  g_epochMs      = millis();
  g_schedCount   = min((uint8_t)8, EEPROM.read(EE_SCHED_COUNT));
  for (uint8_t i=0;i<8;i++){
    int b=EE_SCHED_BASE+i*8;
    g_scheds[i].startH=EEPROM.read(b+0); g_scheds[i].startM=EEPROM.read(b+1);
    g_scheds[i].stopH =EEPROM.read(b+2); g_scheds[i].stopM =EEPROM.read(b+3);
    g_scheds[i].days  =EEPROM.read(b+4); g_scheds[i].autoRestart=EEPROM.read(b+5);
    g_scheds[i].enabled=EEPROM.read(b+6);
  }
  bool motorWasOn = EEPROM.read(EE_MOTOR_WAS_ON) != 0;
  g_staMode       = EEPROM.read(EE_STA_MODE)      != 0;
  logFmt("[SEND] EEPROM: mode=%s timer=%lus(%s) sched=%d",
    g_staMode?"STA":"AP",
    (unsigned long)g_timerTotal, wasActive?"active":"idle",
    (int)g_schedCount);
  if (wasActive && g_timerAutoRst && g_timerRemain > 0) {
    g_timerActive   = true;
    g_timerLastTick = millis();
    logFmt("[SEND] auto-restart timer: %lus remain", (unsigned long)g_timerRemain);
    sendTimerPacket();
  }
  if (motorWasOn && g_timerAutoRst) sendMotorCommandInternal(true);
}

// ═════════════════════════════════════════════════════════════════════════
//  LEDs
// ═════════════════════════════════════════════════════════════════════════

static void updateLEDs() {
  static bool last = false;
  if (g_motorOn == last) return;
  last = g_motorOn;
  digitalWrite(PIN_LED_ON,  g_motorOn ? HIGH:LOW);
  digitalWrite(PIN_LED_OFF, g_motorOn ? LOW:HIGH);
}

static void handleBlink() {
  unsigned long now = millis();
  if (g_blinkReq && g_blinkPhase==0){
    g_blinkReq=false; g_blinkPhase=1; g_blinkPhaseMs=now;
    digitalWrite(PIN_LED_ON,LOW); digitalWrite(PIN_LED_OFF,LOW);
  }
  if (g_blinkPhase==1 && (now-g_blinkPhaseMs)>=BLINK_ON_MS){
    g_blinkPhase=2; g_blinkPhaseMs=now; updateLEDs();
  }
  if (g_blinkPhase==2 && (now-g_blinkPhaseMs)>=BLINK_OFF_MS) g_blinkPhase=0;
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW send helpers
// ═════════════════════════════════════════════════════════════════════════

static void sendTimerPacket() {
  TimerPacket p; p.type=0x04; p.totalSec=g_timerTotal;
  p.remainingSec=g_timerRemain; p.autoRestart=g_timerAutoRst?1:0; p.active=g_timerActive?1:0;
  esp_now_send(BCAST,(uint8_t*)&p,sizeof(p));
}
static void sendSchedPacket() {
  SchedPacket p; memset(&p,0,sizeof(p)); p.type=0x05; p.count=g_schedCount;
  for (uint8_t i=0;i<8;i++) memcpy(&p.entries[i],&g_scheds[i],8);
  esp_now_send(BCAST,(uint8_t*)&p,sizeof(p));
}
static void sendTimeSyncPacket() {
  TimeSyncPacket p; p.type=0x06; p.epoch=currentEpoch(); p.rtcEnabled=g_rtcEnabled?1:0;
  esp_now_send(BCAST,(uint8_t*)&p,sizeof(p));
}
static void sendOtaEnablePacket(const char* pass) {
  OtaEnablePacket p; p.type=0x07;
  strncpy(p.pass, pass, sizeof(p.pass)-1); p.pass[sizeof(p.pass)-1]='\0';
  logFmt("[SEND] OTA enable → receiver (pass len=%d)", (int)strlen(pass));
  for (uint8_t i=0;i<3;i++){ esp_now_send(BCAST,(uint8_t*)&p,sizeof(p)); delay(15); }
}

// ═════════════════════════════════════════════════════════════════════════
//  Motor command
// ═════════════════════════════════════════════════════════════════════════

static void sendMotorCommandInternal(bool on) {
  CmdPacket pkt; pkt.type=0x01; pkt.cmd=on?0x01:0x02; pkt.seq=++g_cmdSeq;
  logFmt("[SEND] Motor %s seq=%lu", on?"ON":"OFF", (unsigned long)g_cmdSeq);
  for (uint8_t i=0;i<3;i++){ esp_now_send(BCAST,(uint8_t*)&pkt,sizeof(pkt)); delay(12); }
  EEPROM.write(EE_MOTOR_WAS_ON, on?1:0); EEPROM.commit();
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═════════════════════════════════════════════════════════════════════════

void espnowOnSend(uint8_t*,uint8_t) {}

void espnowOnRecv(uint8_t*,uint8_t* data,uint8_t len) {
  if (len<1) return;
  switch(data[0]) {
    case 0x02: {
      if (len<sizeof(StatusPacket)) return;
      StatusPacket pkt; memcpy(&pkt,data,sizeof(pkt));
      // Receiver is ground truth — never override with optimistic state
      g_motorOn    = pkt.motorOn  != 0;
      g_currentA   = pkt.current;
      g_waterOk    = pkt.waterOk  != 0;
      g_stall      = pkt.stall    != 0;
      g_lastRecvMs = millis(); g_linkLost = false;
      g_blinkReq   = true;
      if (g_stall) logAdd("[SEND] ⚠ STALL from receiver");
      updateLEDs(); break;
    }
    case 0x03: {
      if (len<2) return;
      LogPacket pkt; memcpy(&pkt,data,min((size_t)len,sizeof(pkt)));
      pkt.msg[sizeof(pkt.msg)-1]='\0'; logAdd(pkt.msg); g_blinkReq=true; break;
    }
    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Timer + Scheduler
// ═════════════════════════════════════════════════════════════════════════

static void handleTimerTick() {
  if (!g_timerActive) return;
  unsigned long now=millis();
  uint32_t elapsed=(uint32_t)((now-g_timerLastTick)/1000UL);
  if (!elapsed) return;
  g_timerLastTick=now-((now-g_timerLastTick)%1000UL);
  if (elapsed>=g_timerRemain) {
    g_timerRemain=0; g_timerActive=false;
    logAdd("[SEND] ⏰ timer expired → Motor OFF");
    sendMotorCommandInternal(false);
    EEPROM.write(EE_TIMER_ACTIVE,0); eeWriteU32(EE_TIMER_REM,0); EEPROM.commit();
    sendTimerPacket();
  } else {
    g_timerRemain -= elapsed;
    if (now-g_timerLastSave>=TIMER_SAVE_MS) {
      g_timerLastSave=now; eeWriteU32(EE_TIMER_REM,g_timerRemain); EEPROM.commit();
    }
  }
}

static void handleScheduler() {
  unsigned long now=millis();
  if (now-g_schedLastCheck<15000UL) return;
  g_schedLastCheck=now;
  if (!g_epoch) return;
  uint32_t ep=currentEpoch();
  uint16_t tod=(uint16_t)((ep%86400UL)/60UL);
  uint8_t  day=epochDayBit(ep);
  if ((int)tod==g_lastActedMin) return;
  for (uint8_t i=0;i<g_schedCount;i++){
    SchedEntry& s=g_scheds[i];
    if (!s.enabled||!(s.days&(1<<day))) continue;
    uint16_t st=s.startH*60+s.startM, sp=s.stopH*60+s.stopM;
    if (tod==st){ logFmt("[SCHED] %d→ON",i); sendMotorCommandInternal(true);  g_lastActedMin=(int)tod; break; }
    if (tod==sp){ logFmt("[SCHED] %d→OFF",i); sendMotorCommandInternal(false); g_lastActedMin=(int)tod; break; }
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  HTTP helpers + status JSON
// ═════════════════════════════════════════════════════════════════════════

static void cors() {
  server.sendHeader("Access-Control-Allow-Origin","*");
  server.sendHeader("Access-Control-Allow-Methods","GET,POST,OPTIONS");
  server.sendHeader("Access-Control-Allow-Headers","Content-Type");
}
static bool linkOk() { return g_lastRecvMs>0 && (millis()-g_lastRecvMs)<10000UL; }

static String statusJson() {
  String j="{";
  j+="\"motorOn\":"  +String(g_motorOn  ?"true":"false")+",";
  j+="\"current\":"  +String(g_currentA,2)+",";
  j+="\"isRunning\":"+String(g_motorOn  ?"true":"false")+",";
  j+="\"waterOk\":"  +String(g_waterOk  ?"true":"false")+",";
  j+="\"stall\":"    +String(g_stall    ?"true":"false")+",";
  j+="\"linkOk\":"   +String(linkOk()   ?"true":"false");
  j+="}"; return j;
}

// ── Route handlers ────────────────────────────────────────────────────────
void hStatus()      { cors(); server.send(200,"application/json",statusJson()); }
void hMotorOn()     { sendMotorCommandInternal(true);  cors(); server.send(200,"application/json","{\"success\":true}"); }
void hMotorOff()    { sendMotorCommandInternal(false); cors(); server.send(200,"application/json","{\"success\":true}"); }
void hAlerts()      { cors(); server.send(200,"application/json","{\"alerts\":[]}"); }
void hAlertsClear() { cors(); server.send(200,"application/json","{\"success\":true}"); }
void hOptions()     { cors(); server.send(204); }
void hNotFound()    { server.send(404,"application/json","{\"error\":\"not found\"}"); }

void hLogs() {
  cors();
  String j="{\"logs\":[";
  uint8_t tot=min((int)g_logCount,(int)LOG_ENTRIES);
  uint8_t st=(g_logCount<LOG_ENTRIES)?0:g_logHead;
  for (uint8_t i=0;i<tot;i++){
    uint8_t idx=(st+i)%LOG_ENTRIES;
    if (i) j+=','; j+='"';
    for(const char*p=g_log[idx];*p;p++){
      if(*p=='"') j+="\\\""; else if(*p=='\\') j+="\\\\"; else j+=*p;
    }
    j+='"';
  }
  j+="]}"; server.send(200,"application/json",j);
}

void hTimerSet() {
  String b=server.arg("plain");
  int si=b.indexOf("\"seconds\":"), ai=b.indexOf("\"autoRestart\":");
  if (si<0){server.send(400,"application/json","{\"error\":\"missing seconds\"}");return;}
  uint32_t sec=(uint32_t)b.substring(si+10).toInt();
  bool ar=(ai>=0)&&(b.substring(ai+14).toInt()!=0);
  g_timerTotal=sec; g_timerRemain=sec; g_timerAutoRst=ar;
  g_timerActive=true; g_timerLastTick=millis(); g_timerLastSave=0;
  eeSave(); logFmt("[TIMER] set %lus autoRst=%d",(unsigned long)sec,(int)ar);
  sendTimerPacket(); cors(); server.send(200,"application/json","{\"success\":true}");
}
void hTimerCancel() {
  g_timerActive=false; g_timerRemain=0;
  EEPROM.write(EE_TIMER_ACTIVE,0); eeWriteU32(EE_TIMER_REM,0); EEPROM.commit();
  logAdd("[TIMER] cancelled"); sendTimerPacket();
  cors(); server.send(200,"application/json","{\"success\":true}");
}
void hTimerStatus() {
  cors();
  String j="{\"active\":"+String(g_timerActive?"true":"false")+
            ",\"remaining\":"+String(g_timerRemain)+
            ",\"total\":"+String(g_timerTotal)+
            ",\"autoRestart\":"+String(g_timerAutoRst?"true":"false")+"}";
  server.send(200,"application/json",j);
}

void hSchedPush() {
  String b=server.arg("plain");
  int ci=b.indexOf("\"count\":"), ei=b.indexOf("\"entries\":");
  if (ci<0||ei<0){server.send(400,"application/json","{\"error\":\"malformed\"}");return;}
  g_schedCount=(uint8_t)min(8,b.substring(ci+8).toInt());
  int pos=ei+10;
  for (uint8_t i=0;i<g_schedCount;i++){
    auto rd=[&](const char*k)->uint8_t{int ki=b.indexOf(k,pos);return ki<0?0:(uint8_t)b.substring(ki+strlen(k)).toInt();};
    g_scheds[i].startH   =rd("\"startH\":");
    g_scheds[i].startM   =rd("\"startM\":");
    g_scheds[i].stopH    =rd("\"stopH\":");
    g_scheds[i].stopM    =rd("\"stopM\":");
    g_scheds[i].days     =rd("\"days\":");
    g_scheds[i].autoRestart=rd("\"autoRestart\":");
    g_scheds[i].enabled  =rd("\"enabled\":");
    pos=b.indexOf("{",pos+1);
  }
  eeSave(); logFmt("[SCHED] pushed %d",(int)g_schedCount);
  sendSchedPacket(); cors(); server.send(200,"application/json","{\"success\":true}");
}
void hSchedClear() {
  g_schedCount=0; memset(g_scheds,0,sizeof(g_scheds)); eeSave();
  logAdd("[SCHED] cleared"); sendSchedPacket();
  cors(); server.send(200,"application/json","{\"success\":true}");
}

void hTimeSync() {
  String b=server.arg("plain");
  int ei=b.indexOf("\"epoch\":"), ri=b.indexOf("\"rtcEnabled\":");
  if (ei<0){server.send(400,"application/json","{\"error\":\"missing epoch\"}");return;}
  g_epoch=(uint32_t)b.substring(ei+8).toInt();
  g_epochMs=millis();
  g_rtcEnabled=(ri>=0)&&(b.substring(ri+13).toInt()!=0);
  EEPROM.write(EE_RTC_FLAG,g_rtcEnabled?1:0);
  eeWriteU32(EE_EPOCH,g_epoch); EEPROM.commit();
  sendTimeSyncPacket(); cors(); server.send(200,"application/json","{\"success\":true}");
}

void hOtaReceiver() {
  String body=server.arg("plain");
  int pi=body.indexOf("\"pass\":");
  if (pi<0){server.send(400,"application/json","{\"error\":\"missing pass\"}");return;}
  // Extract password from JSON string value
  int q1=body.indexOf('"',pi+7);
  int q2=(q1>=0)?body.indexOf('"',q1+1):-1;
  String pass=(q1>=0&&q2>q1)?body.substring(q1+1,q2):"";
  if (pass.isEmpty()||pass==OTA_HTTP_PASS) { /* accept own pass or matched */
    sendOtaEnablePacket(pass.isEmpty()?OTA_HTTP_PASS:pass.c_str());
    cors();
    server.send(200,"application/json",
      "{\"success\":true,\"ssid\":\"MCReceiver-OTA\",\"url\":\"http://192.168.4.1/update\"}");
  } else {
    // Wrong password — still forward but the receiver will also verify
    sendOtaEnablePacket(pass.c_str());
    cors(); server.send(200,"application/json","{\"success\":true}");
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200,SERIAL_8N1,SERIAL_TX_ONLY);
  delay(100); logAdd("[SEND] boot v2");

  pinMode(PIN_BTN_ON,  INPUT_PULLUP);
  pinMode(PIN_BTN_OFF, INPUT_PULLUP);
  pinMode(PIN_LED_ON,  OUTPUT);
  pinMode(PIN_LED_OFF, OUTPUT);
  digitalWrite(PIN_LED_ON,LOW); digitalWrite(PIN_LED_OFF,HIGH);

  EEPROM.begin(EE_SIZE);
  eeLoad();

  // ── Network setup ─────────────────────────────────────────────────────
  WiFi.persistent(false);
  if (g_staMode) {
    // STA / server mode — join home Wi-Fi
    WiFi.mode(WIFI_STA);
    WiFi.begin(HOME_WIFI_SSID, HOME_WIFI_PASS);
    WiFi.setOutputPower(20.5f);
    wifi_set_phy_mode(PHY_MODE_11B);
    logFmt("[SEND] STA mode → joining \"%s\"", HOME_WIFI_SSID);
    unsigned long t=millis();
    while (WiFi.status()!=WL_CONNECTED && millis()-t<15000UL) {
      delay(200); Serial.print('.');
    }
    if (WiFi.status()==WL_CONNECTED) {
      logFmt("[SEND] STA connected  IP=%s", WiFi.localIP().toString().c_str());
    } else {
      logAdd("[SEND] STA connect failed — falling back to AP mode");
      g_staMode=false;
      EEPROM.write(EE_STA_MODE,0); EEPROM.commit();
    }
  }
  if (!g_staMode) {
    // AP mode (default)
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS, WIFI_CH);
    WiFi.setOutputPower(20.5f);
    wifi_set_phy_mode(PHY_MODE_11B);
    logFmt("[SEND] AP mode  IP=%s  SSID=%s", WiFi.softAPIP().toString().c_str(), AP_SSID);
  }

  // Boot blink 2× = sender identifier
  for (int i=0;i<2;i++){
    digitalWrite(PIN_LED_ON,HIGH);delay(220);
    digitalWrite(PIN_LED_ON,LOW); delay(150);
  }
  delay(300);

  // ── ESP-NOW ───────────────────────────────────────────────────────────
  int tries=3;
  while (esp_now_init()!=0 && tries-->0) { logAdd("[SEND] ESP-NOW retry"); delay(500); }
  if (tries<0){ logAdd("[SEND] ESP-NOW FAILED — reboot"); delay(1000); ESP.restart(); }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);
  esp_now_add_peer(BCAST,ESP_NOW_ROLE_COMBO,WIFI_CH,nullptr,0);
  logAdd("[SEND] ESP-NOW ready");

  // ── HTTP routes ───────────────────────────────────────────────────────
  // OTA update endpoint — password-protected
  httpUpdater.setup(&server, "/update", OTA_HTTP_USER, OTA_HTTP_PASS);

  server.on("/api/status",         HTTP_GET,    hStatus);
  server.on("/api/motor/on",       HTTP_POST,   hMotorOn);
  server.on("/api/motor/off",      HTTP_POST,   hMotorOff);
  server.on("/api/logs",           HTTP_GET,    hLogs);
  server.on("/api/alerts",         HTTP_GET,    hAlerts);
  server.on("/api/alerts/clear",   HTTP_POST,   hAlertsClear);
  server.on("/api/timer/set",      HTTP_POST,   hTimerSet);
  server.on("/api/timer/cancel",   HTTP_POST,   hTimerCancel);
  server.on("/api/timer/status",   HTTP_GET,    hTimerStatus);
  server.on("/api/schedule/push",  HTTP_POST,   hSchedPush);
  server.on("/api/schedule/clear", HTTP_POST,   hSchedClear);
  server.on("/api/time/sync",      HTTP_POST,   hTimeSync);
  server.on("/api/ota/receiver",   HTTP_POST,   hOtaReceiver);

  // Blanket OPTIONS pre-flight
  server.onNotFound([](){
    if (server.method()==HTTP_OPTIONS){ cors(); server.send(204); }
    else hNotFound();
  });
  server.begin();

  logFmt("[SEND] HTTP :80 ready (/update = OTA  user=%s)", OTA_HTTP_USER);
  updateLEDs();
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();
  unsigned long now=millis();

  // ── Link-lost ─────────────────────────────────────────────────────────
  if (g_lastRecvMs>0 && (now-g_lastRecvMs)>10000UL && !g_linkLost) {
    g_linkLost=true; logAdd("[SEND] ⚠ ESP-NOW link lost");
  }

  // ── Buttons ───────────────────────────────────────────────────────────
  bool onRaw  = (digitalRead(PIN_BTN_ON)  == LOW);
  bool offRaw = (digitalRead(PIN_BTN_OFF) == LOW);

  // Dual-button hold → toggle AP / STA mode
  if (onRaw && offRaw) {
    if (!g_dualActive) { g_dualActive=true; g_dualStart=now; }
    unsigned long held = now - g_dualStart;
    // Flash both LEDs proportionally during hold to show progress
    if (held < MODE_HOLD_MS) {
      bool flash = ((held / 500UL) % 2 == 0);
      digitalWrite(PIN_LED_ON,  flash ? HIGH : LOW);
      digitalWrite(PIN_LED_OFF, flash ? HIGH : LOW);
    }
    if (held >= MODE_HOLD_MS) {
      g_dualActive = false;
      g_staMode    = !g_staMode;
      EEPROM.write(EE_STA_MODE, g_staMode?1:0); EEPROM.commit();
      logFmt("[SEND] Mode → %s — rebooting", g_staMode?"STA/Server":"AP/Client");
      // 5× fast flash to confirm
      for (int i=0;i<5;i++){
        digitalWrite(PIN_LED_ON,HIGH);digitalWrite(PIN_LED_OFF,HIGH);delay(100);
        digitalWrite(PIN_LED_ON,LOW); digitalWrite(PIN_LED_OFF,LOW); delay(100);
      }
      delay(200); ESP.restart();
    }
    // While both pressed: suppress individual button processing
    g_btnOnState=0; g_btnOffState=0;
  } else {
    g_dualActive = false;
    updateLEDs();  // restore correct LED state after dual-button released

    // ON button (3-state)
    switch (g_btnOnState) {
      case 0: if (onRaw){ g_btnOnMs=now; g_btnOnState=1; } break;
      case 1:
        if (!onRaw){ g_btnOnState=0; break; }
        if ((now-g_btnOnMs)>=DEBOUNCE_MS){
          logAdd("[SEND] BTN ON"); sendMotorCommandInternal(true); g_btnOnState=2;
        }
        break;
      case 2: if (!onRaw) g_btnOnState=0; break;
    }

    // OFF button (3-state)
    switch (g_btnOffState) {
      case 0: if (offRaw){ g_btnOffMs=now; g_btnOffState=1; } break;
      case 1:
        if (!offRaw){ g_btnOffState=0; break; }
        if ((now-g_btnOffMs)>=DEBOUNCE_MS){
          logAdd("[SEND] BTN OFF"); sendMotorCommandInternal(false); g_btnOffState=2;
        }
        break;
      case 2: if (!offRaw) g_btnOffState=0; break;
    }
  }

  handleTimerTick();
  handleScheduler();
  handleBlink();
}
