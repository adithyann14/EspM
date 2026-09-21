/**
 * ════════════════════════════════════════════════════════════════════════
 *  RECEIVER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)   v2
 *
 *  ── Hardware features (from v2 upload) ───────────────────────────────
 *  • Relay fires ONLY after servo holds at target for SERVO_HOLD_MS
 *  • ServoIntent STARTING / STOPPING — clean relay sequencing
 *  • EMA current smoothing, non-blocking, every 50 ms
 *  • Stall detection: motor ON but I < threshold for 5 s → flag + auto-off
 *  • Water sensor 30 s auto cut-off after relay fires
 *  • g_queuedCmd: OFF wins over ON, queued while servo busy (race fix)
 *  • ESP-NOW init 3 retries; boot blink 3× = receiver identifier
 *  • Servo attached with explicit 500–2400 µs range (GPIO0 quirk fix)
 *
 *  ── Scheduler / Timer features (from patch) ──────────────────────────
 *  • EEPROM 128 B: timer, schedule, RTC flag, epoch, motorWasOn
 *  • Handles TimerPacket (0x04): local countdown → Motor OFF at expiry
 *  • Handles SchedPacket (0x05): store + run 8-entry day/time schedule
 *  • Handles TimeSyncPacket (0x06): sync phone epoch / RTC flag
 *  • Auto-restart on power cut: EEPROM timer/motor state restored at boot
 *  • Timer checkpointed to EEPROM every 10 s
 *
 *  ── Dead code (compile-guarded) ──────────────────────────────────────
 *  • LoRa SX1278 (D0=RST, D4=DIO0, D5=SCK, D6=MISO, D7=MOSI, D8=NSS)
 *  • DS3231 RTC over I²C: SDA=D6/GPIO12, SCL=D5/GPIO14
 *    Uncomment #define HAS_RTC_HARDWARE and wire DS3231 to activate.
 *  • Water sensor via BC547 (D2)
 *
 *  EEPROM layout (shared with Sender, 128 B):
 *    0       magic (0xBE)     1-4    timer total    5   autoRestart
 *    6       wasActive        7-10   remaining      11  RTC flag
 *    12-15   epoch            16     sched count    17-80 sched×8
 *    81      motorWasOn
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <Servo.h>
#include <EEPROM.h>

extern "C" {
  #include <espnow.h>
  #include <user_interface.h>
}

// ── RTC dead code ─────────────────────────────────────────────────────────
// #define HAS_RTC_HARDWARE
// #ifdef  HAS_RTC_HARDWARE
// #include <Wire.h>
// #include <RTClib.h>
// static RTC_DS3231 rtc;
// #endif

// ── LoRa SX1278 — DEAD CODE ─────────────────────────────────────────────
// #include <SPI.h>  #include <LoRa.h>
// #define LORA_RST D0  #define LORA_DIO0 D4  #define LORA_SCK  D5
// #define LORA_MISO D6  #define LORA_MOSI D7  #define LORA_NSS  D8

// ── Pins ──────────────────────────────────────────────────────────────────
#define PIN_RELAY       D1   // GPIO5  Relay IN, active HIGH
#define PIN_WATER       D2   // GPIO4  BC547 water sensor, active LOW — DEAD/TUNING
#define PIN_SERVO       D3   // GPIO0  Servo signal  ⚠ boot pin
// D5/D6 → RTC SCL/SDA if HAS_RTC_HARDWARE, else LoRa dead code
// D4/D7/D8/D0 → LoRa dead code
#define PIN_LED         2    // GPIO2  Built-in LED, active LOW

// ── Sender AP credentials ─────────────────────────────────────────────────
static const char*    AP_SSID      = "MotorControl";
static const char*    AP_PASS      = "motor1234";
static const uint8_t  WIFI_CH      = 1;
static const uint16_t AP_TIMEOUT   = 10000;

// ── EEPROM (same layout as Sender) ───────────────────────────────────────
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
  EEPROM.write(addr,     v        & 0xFF);
  EEPROM.write(addr + 1, (v >> 8) & 0xFF);
  EEPROM.write(addr + 2, (v >>16) & 0xFF);
  EEPROM.write(addr + 3, (v >>24) & 0xFF);
}
static inline uint32_t eeReadU32(int addr) {
  return (uint32_t)EEPROM.read(addr)
       | ((uint32_t)EEPROM.read(addr+1) <<  8)
       | ((uint32_t)EEPROM.read(addr+2) << 16)
       | ((uint32_t)EEPROM.read(addr+3) << 24);
}

// ── ESP-NOW packets ────────────────────────────────────────────────────────
static uint8_t BCAST_MAC[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct __attribute__((packed)) {
  uint8_t  type;   // 0x01
  uint8_t  cmd;    // 0x01=ON  0x02=OFF
  uint32_t seq;
} CmdPacket;

typedef struct __attribute__((packed)) {
  uint8_t  type;     // 0x02
  float    current;  // EMA-smoothed ACS712 (A)
  uint8_t  motorOn;  // relay state
  uint8_t  waterOk;  // 1=confirmed or motor off  0=waiting
  uint8_t  stall;    // 1=relay ON but no current for 5 s
  uint32_t seq;
} StatusPacket;

typedef struct __attribute__((packed)) {
  uint8_t  type;          // 0x04
  uint32_t totalSec;
  uint32_t remainingSec;
  uint8_t  autoRestart;
  uint8_t  active;
} TimerPacket;

typedef struct __attribute__((packed)) {
  uint8_t type;            // 0x05
  uint8_t count;
  struct { uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad; } entries[8];
} SchedPacket;

typedef struct __attribute__((packed)) {
  uint8_t  type;           // 0x06
  uint32_t epoch;
  uint8_t  rtcEnabled;
} TimeSyncPacket;

// ── ACS712  (5A module, 3.3 V supply) ───────────────────────────────────
static const float ACS_ZERO_V         = 1.65f;
static const float ACS_SENS_V_PER_A   = 0.122f;
static const float CURRENT_RUN_THRESH = 0.10f;
#define ACS_ALPHA     0.15f
#define ACS_SAMPLE_MS 50UL

// ── Servo ─────────────────────────────────────────────────────────────────
#define SERVO_NEUTRAL  90
#define SERVO_START     0
#define SERVO_STOP    180
#define SERVO_HOLD_MS 600UL
#define SERVO_BACK_MS 400UL

// ── Timing constants ──────────────────────────────────────────────────────
#define WATER_TIMEOUT_MS   30000UL
#define STALL_TIMEOUT_MS    5000UL
#define STATUS_INTERVAL_MS  1000UL
#define TIMER_SAVE_MS      10000UL
#define BLINK_MS             150UL

// ── Servo state machine ───────────────────────────────────────────────────
enum class ServoState  : uint8_t { IDLE, PRESSING, RETURNING };
enum class ServoIntent : uint8_t { NONE, STARTING, STOPPING  };

static Servo       motorServo;
static ServoState  servoState  = ServoState::IDLE;
static ServoIntent servoIntent = ServoIntent::NONE;
static unsigned long servoTimer = 0;

// ── Motor / command state ─────────────────────────────────────────────────
static volatile bool     g_pendingOn  = false;
static volatile bool     g_pendingOff = false;
static volatile uint32_t g_lastSeq    = 0;
static volatile bool     g_rxFlag     = false;
static bool              g_motorOn    = false;
static uint8_t           g_queuedCmd  = 0;   // 0=none 1=ON 2=OFF

// ── Water sensor ──────────────────────────────────────────────────────────
static bool          g_waterCheckActive = false;
static unsigned long g_waterCheckStart  = 0;
static bool          g_waterDetected    = false;

// ── ACS712 EMA ────────────────────────────────────────────────────────────
static float         g_currentSmooth = 0.0f;
static unsigned long g_acsLastMs     = 0;

// ── Stall detection ───────────────────────────────────────────────────────
static unsigned long g_stallTimer    = 0;
static bool          g_stallDetected = false;

// ── Status broadcast ──────────────────────────────────────────────────────
static uint32_t      g_statusSeq    = 0;
static unsigned long g_lastStatusMs = 0;

// ── Blink ─────────────────────────────────────────────────────────────────
static unsigned long g_blinkUntil = 0;

// ── Timer state ───────────────────────────────────────────────────────────
static bool          g_timerActive    = false;
static uint32_t      g_timerTotal     = 0;
static uint32_t      g_timerRemain    = 0;
static bool          g_timerAutoRst   = false;
static unsigned long g_timerLastTick  = 0;
static unsigned long g_timerLastSaveMs= 0;

// ── Schedule state ────────────────────────────────────────────────────────
struct SchedEntry { uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad; };
static uint8_t     g_schedCount = 0;
static SchedEntry  g_scheds[8]  = {};
static int         g_lastActedMinute = -1;
static unsigned long g_schedLastCheckMs = 0;

// ── RTC / epoch ───────────────────────────────────────────────────────────
static bool          g_rtcEnabled = false;
static uint32_t      g_epoch      = 0;
static unsigned long g_epochMs    = 0;

static uint32_t currentEpoch() { return g_epoch + (uint32_t)((millis() - g_epochMs) / 1000UL); }
static uint8_t  epochDayBit(uint32_t e) { return (uint8_t)(((e / 86400UL) + 4UL) % 7UL); }

// ── RTC dead code ─────────────────────────────────────────────────────────
// #ifdef HAS_RTC_HARDWARE
// static uint32_t rtcRead() { return rtc.now().unixtime(); }
// static void     rtcSet(uint32_t e) { rtc.adjust(DateTime(e)); }
// #endif

// ═════════════════════════════════════════════════════════════════════════
//  EEPROM
// ═════════════════════════════════════════════════════════════════════════

static void eeSave() {
  EEPROM.write(EE_MAGIC, EE_MAGIC_VAL);
  eeWriteU32(EE_TIMER_TOTAL, g_timerTotal);
  EEPROM.write(EE_TIMER_AUTO,   g_timerAutoRst ? 1 : 0);
  EEPROM.write(EE_TIMER_ACTIVE, g_timerActive  ? 1 : 0);
  eeWriteU32(EE_TIMER_REM,   g_timerRemain);
  EEPROM.write(EE_RTC_FLAG,  g_rtcEnabled ? 1 : 0);
  eeWriteU32(EE_EPOCH,       currentEpoch());
  EEPROM.write(EE_SCHED_COUNT, g_schedCount);
  for (uint8_t i = 0; i < 8; i++) {
    int b = EE_SCHED_BASE + i * 8;
    EEPROM.write(b+0, g_scheds[i].startH);    EEPROM.write(b+1, g_scheds[i].startM);
    EEPROM.write(b+2, g_scheds[i].stopH);     EEPROM.write(b+3, g_scheds[i].stopM);
    EEPROM.write(b+4, g_scheds[i].days);      EEPROM.write(b+5, g_scheds[i].autoRestart);
    EEPROM.write(b+6, g_scheds[i].enabled);   EEPROM.write(b+7, 0);
  }
  EEPROM.write(EE_MOTOR_WAS_ON, g_motorOn ? 1 : 0);
  EEPROM.commit();
}

static void eeLoad() {
  if (EEPROM.read(EE_MAGIC) != EE_MAGIC_VAL) { Serial.println("[RECV] EEPROM blank"); return; }
  g_timerTotal   = eeReadU32(EE_TIMER_TOTAL);
  g_timerAutoRst = EEPROM.read(EE_TIMER_AUTO)   != 0;
  bool wasActive = EEPROM.read(EE_TIMER_ACTIVE)  != 0;
  g_timerRemain  = eeReadU32(EE_TIMER_REM);
  g_rtcEnabled   = EEPROM.read(EE_RTC_FLAG)      != 0;
  g_epoch        = eeReadU32(EE_EPOCH);
  g_epochMs      = millis();
  g_schedCount   = min((uint8_t)8, EEPROM.read(EE_SCHED_COUNT));
  for (uint8_t i = 0; i < 8; i++) {
    int b = EE_SCHED_BASE + i * 8;
    g_scheds[i].startH    = EEPROM.read(b+0); g_scheds[i].startM    = EEPROM.read(b+1);
    g_scheds[i].stopH     = EEPROM.read(b+2); g_scheds[i].stopM     = EEPROM.read(b+3);
    g_scheds[i].days      = EEPROM.read(b+4); g_scheds[i].autoRestart = EEPROM.read(b+5);
    g_scheds[i].enabled   = EEPROM.read(b+6);
  }
  bool motorWasOn = EEPROM.read(EE_MOTOR_WAS_ON) != 0;
  Serial.printf("[RECV] EEPROM loaded  timer=%lus(%s)  sched=%d  motorWasOn=%d\n",
    (unsigned long)g_timerTotal, wasActive?"active":"idle",
    (int)g_schedCount, (int)motorWasOn);
  if (wasActive && g_timerAutoRst && g_timerRemain > 0) {
    g_timerActive   = true;
    g_timerLastTick = millis();
    Serial.printf("[RECV] auto-restart timer: %lus remaining\n", (unsigned long)g_timerRemain);
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  ACS712 EMA — non-blocking, called every ACS_SAMPLE_MS
// ═════════════════════════════════════════════════════════════════════════

static void updateCurrentEMA(unsigned long now) {
  if (now - g_acsLastMs < ACS_SAMPLE_MS) return;
  g_acsLastMs = now;
  float voltage = (analogRead(A0) / 1023.0f) * 3.3f;
  float sample  = (voltage - ACS_ZERO_V) / ACS_SENS_V_PER_A;
  g_currentSmooth = ACS_ALPHA * sample + (1.0f - ACS_ALPHA) * g_currentSmooth;
}

// ═════════════════════════════════════════════════════════════════════════
//  Status broadcast
// ═════════════════════════════════════════════════════════════════════════

static void sendStatusNow() {
  StatusPacket pkt;
  pkt.type    = 0x02;
  pkt.current = g_currentSmooth;
  pkt.motorOn = g_motorOn ? 1 : 0;
  pkt.waterOk = (g_waterDetected || !g_waterCheckActive) ? 1 : 0;
  pkt.stall   = g_stallDetected ? 1 : 0;
  pkt.seq     = ++g_statusSeq;
  esp_now_send(BCAST_MAC, (uint8_t*)&pkt, sizeof(pkt));
  g_lastStatusMs = millis();
}

static void sendStatusPeriodic(unsigned long now) {
  if (now - g_lastStatusMs < STATUS_INTERVAL_MS) return;
  sendStatusNow();
  Serial.printf("[RECV] I=%.2fA  relay=%d  water=%s  stall=%d  timer=%lus\n",
    g_currentSmooth, (int)g_motorOn,
    g_waterDetected ? "OK" : (g_waterCheckActive ? "WAIT" : "IDLE"),
    (int)g_stallDetected, (unsigned long)g_timerRemain);
}

// ═════════════════════════════════════════════════════════════════════════
//  Motor helpers — servo move; relay fires in state machine
// ═════════════════════════════════════════════════════════════════════════

static void activateMotor() {
  if (servoState != ServoState::IDLE || g_motorOn) return;
  servoIntent = ServoIntent::STARTING;
  motorServo.write(SERVO_START);
  servoState = ServoState::PRESSING; servoTimer = millis();
  Serial.println("[RECV] servo→0° (START; relay fires after hold)");
}

static void deactivateMotor() {
  if (servoState != ServoState::IDLE || !g_motorOn) return;
  servoIntent = ServoIntent::STOPPING;
  motorServo.write(SERVO_STOP);
  servoState = ServoState::PRESSING; servoTimer = millis();
  g_waterCheckActive = false; g_waterDetected = false;
  Serial.println("[RECV] servo→180° (STOP; relay cuts after hold)");
}

// ═════════════════════════════════════════════════════════════════════════
//  Servo state machine — relay changes happen here
// ═════════════════════════════════════════════════════════════════════════

static void handleServo(unsigned long now) {
  switch (servoState) {

    case ServoState::PRESSING:
      if (now - servoTimer >= SERVO_HOLD_MS) {
        if (servoIntent == ServoIntent::STARTING) {
          digitalWrite(PIN_RELAY, HIGH);
          g_motorOn = true;
          g_waterCheckActive = true; g_waterCheckStart = now; g_waterDetected = false;
          g_stallTimer = 0; g_stallDetected = false;
          Serial.println("[RECV] ✔ RELAY ON");
          EEPROM.write(EE_MOTOR_WAS_ON, 1); EEPROM.commit();
          sendStatusNow();
        } else if (servoIntent == ServoIntent::STOPPING) {
          digitalWrite(PIN_RELAY, LOW);
          g_motorOn = false;
          g_stallTimer = 0; g_stallDetected = false;
          Serial.println("[RECV] ✔ RELAY OFF");
          EEPROM.write(EE_MOTOR_WAS_ON, 0); EEPROM.commit();
          sendStatusNow();
        }
        motorServo.write(SERVO_NEUTRAL);
        servoState = ServoState::RETURNING; servoTimer = now;
      }
      break;

    case ServoState::RETURNING:
      if (now - servoTimer >= SERVO_BACK_MS) {
        servoState = ServoState::IDLE; servoIntent = ServoIntent::NONE;
        Serial.println("[RECV] servo→90° neutral");
        if      (g_queuedCmd == 1) { g_queuedCmd = 0; activateMotor();   }
        else if (g_queuedCmd == 2) { g_queuedCmd = 0; deactivateMotor(); }
      }
      break;

    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Water sensor auto cut-off
// ═════════════════════════════════════════════════════════════════════════

static void handleWaterSensor(unsigned long now) {
  if (!g_waterCheckActive || g_waterDetected) return;
  if (digitalRead(PIN_WATER) == LOW) {
    g_waterDetected = true; g_waterCheckActive = false;
    Serial.println("[RECV] ✔ water detected"); sendStatusNow(); return;
  }
  if (now - g_waterCheckStart >= WATER_TIMEOUT_MS) {
    Serial.println("[RECV] ✗ WATER TIMEOUT 30 s — auto cut-off");
    g_waterCheckActive = false;
    if      (servoState == ServoState::IDLE) deactivateMotor();
    else                                     g_queuedCmd = 2;
    sendStatusNow();
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Stall detection
// ═════════════════════════════════════════════════════════════════════════

static void handleStallDetect(unsigned long now) {
  if (!g_motorOn) { g_stallTimer = 0; g_stallDetected = false; return; }
  if (fabsf(g_currentSmooth) < CURRENT_RUN_THRESH) {
    if (g_stallTimer == 0) g_stallTimer = now;
    if ((now - g_stallTimer > STALL_TIMEOUT_MS) && !g_stallDetected) {
      g_stallDetected = true;
      Serial.println("[RECV] ⚠ STALL — motor ON but I≈0 for 5 s");
      sendStatusNow();
    }
  } else {
    if (g_stallDetected) { g_stallDetected = false; Serial.println("[RECV] stall cleared"); sendStatusNow(); }
    g_stallTimer = 0;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Timer tick
// ═════════════════════════════════════════════════════════════════════════

static void handleTimerTick(unsigned long now) {
  if (!g_timerActive) return;
  uint32_t elapsed = (uint32_t)((now - g_timerLastTick) / 1000UL);
  if (elapsed == 0) return;
  g_timerLastTick = now - ((now - g_timerLastTick) % 1000UL);
  if (elapsed >= g_timerRemain) {
    g_timerRemain = 0; g_timerActive = false;
    Serial.println("[RECV] ⏰ timer expired → Motor OFF");
    if (servoState == ServoState::IDLE) deactivateMotor();
    else g_queuedCmd = 2;
    EEPROM.write(EE_TIMER_ACTIVE, 0); eeWriteU32(EE_TIMER_REM, 0); EEPROM.commit();
  } else {
    g_timerRemain -= elapsed;
    if (now - g_timerLastSaveMs >= TIMER_SAVE_MS) {
      g_timerLastSaveMs = now;
      eeWriteU32(EE_TIMER_REM, g_timerRemain); EEPROM.commit();
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Scheduler tick (every 15 s)
// ═════════════════════════════════════════════════════════════════════════

static void handleScheduler(unsigned long now) {
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
      Serial.printf("[SCHED] slot %d → ON  (%02d:%02d)\n", i, s.startH, s.startM);
      if (servoState == ServoState::IDLE) activateMotor(); else g_queuedCmd = 1;
      g_lastActedMinute = (int)tod; break;
    }
    if (tod == stopTod) {
      Serial.printf("[SCHED] slot %d → OFF (%02d:%02d)\n", i, s.stopH, s.stopM);
      if (servoState == ServoState::IDLE) deactivateMotor(); else g_queuedCmd = 2;
      g_lastActedMinute = (int)tod; break;
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks
// ═════════════════════════════════════════════════════════════════════════

void espnowOnSend(uint8_t*, uint8_t) { }

void espnowOnRecv(uint8_t*, uint8_t* data, uint8_t len) {
  if (len < 1) return;
  switch (data[0]) {
    case 0x01: {
      if (len < sizeof(CmdPacket)) return;
      CmdPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      if (pkt.seq == g_lastSeq) return;
      g_lastSeq = pkt.seq; g_rxFlag = true;
      if (pkt.cmd == 0x01) g_pendingOn  = true;
      if (pkt.cmd == 0x02) g_pendingOff = true;
      break;
    }
    case 0x04: {
      if (len < sizeof(TimerPacket)) return;
      TimerPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      g_timerTotal   = pkt.totalSec;
      g_timerRemain  = pkt.remainingSec;
      g_timerAutoRst = (pkt.autoRestart != 0);
      if (pkt.active && !g_timerActive) {
        g_timerActive   = true;
        g_timerLastTick = millis();
        Serial.printf("[RECV] Timer set: %lus  auto=%d\n",
          (unsigned long)pkt.totalSec, (int)pkt.autoRestart);
      } else if (!pkt.active) {
        g_timerActive = false;
        Serial.println("[RECV] Timer cancelled");
      }
      eeSave();
      break;
    }
    case 0x05: {
      if (len < 2) return;
      SchedPacket pkt; memcpy(&pkt, data, min((size_t)len, sizeof(pkt)));
      g_schedCount = min((uint8_t)8, pkt.count);
      for (uint8_t i = 0; i < g_schedCount; i++) {
        g_scheds[i].startH     = pkt.entries[i].startH;
        g_scheds[i].startM     = pkt.entries[i].startM;
        g_scheds[i].stopH      = pkt.entries[i].stopH;
        g_scheds[i].stopM      = pkt.entries[i].stopM;
        g_scheds[i].days       = pkt.entries[i].days;
        g_scheds[i].autoRestart= pkt.entries[i].autoRestart;
        g_scheds[i].enabled    = pkt.entries[i].enabled;
      }
      eeSave();
      Serial.printf("[RECV] Schedules updated: %d entries\n", (int)g_schedCount);
      break;
    }
    case 0x06: {
      if (len < sizeof(TimeSyncPacket)) return;
      TimeSyncPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      g_epoch      = pkt.epoch;
      g_epochMs    = millis();
      g_rtcEnabled = (pkt.rtcEnabled != 0);
      // #ifdef HAS_RTC_HARDWARE
      // if (g_rtcEnabled) rtcSet(g_epoch);
      // #endif
      break;
    }
    default: break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════

void setup() {
  // Relay OFF — must be first line
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);

  Serial.begin(115200);
  delay(100);
  Serial.println("\n[RECV] boot v2");

  pinMode(PIN_LED,   OUTPUT); digitalWrite(PIN_LED, HIGH);  // off (active LOW)
  pinMode(PIN_WATER, INPUT);
  Serial.printf("[RECV] water at boot: %s\n", digitalRead(PIN_WATER)==LOW ? "WET":"DRY");

  // Servo
  motorServo.attach(PIN_SERVO, 500, 2400);
  motorServo.write(SERVO_NEUTRAL);
  delay(500); Serial.println("[RECV] servo @ 90°");

  // EEPROM
  EEPROM.begin(EE_SIZE);
  eeLoad();

  // #ifdef HAS_RTC_HARDWARE
  // Wire.begin(D6,D5);
  // if (!rtc.begin()) Serial.println("[RECV] DS3231 not found");
  // else { g_epoch = rtcRead(); g_epochMs = millis(); }
  // #endif

  // Boot blink: 5× rapid = alive, then 3× slow = RECEIVER id
  for (int i = 0; i < 5; i++) { digitalWrite(PIN_LED,LOW);delay(80);digitalWrite(PIN_LED,HIGH);delay(80); }
  delay(400);
  for (int i = 0; i < 3; i++) { digitalWrite(PIN_LED,LOW);delay(300);digitalWrite(PIN_LED,HIGH);delay(300); }

  // Wi-Fi
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID, AP_PASS);
  WiFi.setOutputPower(20.5f);
  wifi_set_phy_mode(PHY_MODE_11B);
  Serial.print("[RECV] connecting to AP");
  unsigned long t = millis();
  while (WiFi.status() != WL_CONNECTED && millis()-t < AP_TIMEOUT) { delay(200); Serial.print('.'); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[RECV] joined  IP=%s  ch=%d\n",
      WiFi.localIP().toString().c_str(), (int)WiFi.channel());
  } else {
    Serial.println("\n[RECV] AP not found — forcing ch=" + String(WIFI_CH));
    WiFi.disconnect(); wifi_set_channel(WIFI_CH);
  }

  // ESP-NOW (3 retries)
  int tries = 3;
  while (esp_now_init() != 0 && tries-- > 0) { Serial.println("[RECV] ESP-NOW retry"); delay(500); }
  if (tries < 0) { Serial.println("[RECV] ESP-NOW FAILED — reboot"); delay(1000); ESP.restart(); }
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);
  esp_now_add_peer(BCAST_MAC, ESP_NOW_ROLE_COMBO, WIFI_CH, nullptr, 0);
  for (int i=0;i<3;i++){digitalWrite(PIN_LED,LOW);delay(80);digitalWrite(PIN_LED,HIGH);delay(80);}
  Serial.println("[RECV] ready");
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();

  updateCurrentEMA(now);

  // Blink on ESP-NOW receive
  if (g_rxFlag) { g_rxFlag=false; g_blinkUntil=now+BLINK_MS; digitalWrite(PIN_LED,LOW); }
  if (now >= g_blinkUntil) digitalWrite(PIN_LED, HIGH);

  // Command conflict: OFF always wins
  if (g_pendingOff && g_pendingOn) g_pendingOn = false;

  if (g_pendingOn) {
    g_pendingOn = false;
    if      (servoState == ServoState::IDLE && !g_motorOn) activateMotor();
    else if (servoState != ServoState::IDLE)               g_queuedCmd = 1;
  }
  if (g_pendingOff) {
    g_pendingOff = false;
    if      (servoState == ServoState::IDLE && g_motorOn) deactivateMotor();
    else if (servoState != ServoState::IDLE)              g_queuedCmd = 2;
  }

  handleServo(now);
  handleWaterSensor(now);
  handleStallDetect(now);
  handleTimerTick(now);
  handleScheduler(now);
  sendStatusPeriodic(now);
}
