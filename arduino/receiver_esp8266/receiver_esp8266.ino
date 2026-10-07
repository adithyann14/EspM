/**
 * ════════════════════════════════════════════════════════════════════════
 *  RECEIVER  —  NodeMCU ESP8266  (board: NodeMCU 1.0 / ESP-12E)   v5
 *
 *  Comms: plain Wi-Fi only (no ESP-NOW, no LoRa).  The receiver joins the
 *  sender's access point "MotorControl" as a station and exchanges UDP
 *  datagrams with it on port 4210.  Max TX power 20.5 dBm, PHY 802.11b,
 *  modem sleep OFF (sleeping radios drop UDP).
 *
 *  ── Pins ──────────────────────────────────────────────────────────────
 *  D1 (GPIO5)  → Relay IN, active HIGH
 *  D2 (GPIO4)  → Water sensor (INPUT_PULLUP), ACTIVE LOW: LOW=water, HIGH=dry
 *  D3 (GPIO0)  → Servo signal (attached only while moving, idles HIGH)
 *  D4 (GPIO2)  → Built-in LED, active LOW: solid = Wi-Fi up, blink = command
 *  A0          → ACS712 current sensor output
 *
 *  ── Dry-run protection (what changed and why) ────────────────────────
 *  • While the motor is ON, every continuous "no water" spell is timed.
 *    When it reaches the dry-run timeout (default 10 s, set from the app,
 *    stored in EEPROM) the RELAY IS DROPPED IMMEDIATELY from the main
 *    loop.  It does not wait for, or depend on, the servo state machine,
 *    a queued command, or an "armed" flag.  It is level-triggered: as
 *    long as (motor ON && dry for ≥ timeout) it cuts, so it cannot miss.
 *  • Water must be present for WATER_WET_CONFIRM_MS (1 s) before the dry
 *    timer is reset.  Contactor/motor EMI on a long sensor wire can pull
 *    the pin LOW for a few ms; that used to reset the timer forever.
 *  • Status "waterOk" is the real sensor state, not "true when idle".
 *  Set BLOCK_START_WHEN_DRY to 1 to also refuse to start when dry.
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <Servo.h>
#include <EEPROM.h>

extern "C" {
  #include <user_interface.h>
}

// ── Pins ──────────────────────────────────────────────────────────────────
#define PIN_RELAY  D1   // GPIO5   Relay IN, active HIGH
#define PIN_WATER  D2   // GPIO4   Water sensor: HIGH=no water, LOW=water
#define PIN_SERVO  D3   // GPIO0   Servo signal
#define PIN_LED    2    // GPIO2   Built-in LED, active LOW (= D4)

// ── Sender AP credentials (must match the sender sketch) ─────────────────
static const char*    AP_SSID    = "MotorControl";
static const char*    AP_PASS    = "motor1234";
static const uint16_t AP_TIMEOUT = 8000;      // max wait for Wi-Fi at boot (ms)
static const uint16_t UDP_PORT   = 4210;      // same port on sender and receiver
#define WIFI_RETRY_MS 15000UL                 // re-issue WiFi.begin() while down

// ── Water sensor behaviour ────────────────────────────────────────────────
#define WATER_ACTIVE_LOW      1   // 1: pin LOW = water (your sensor).  0: pin HIGH = water
#define BLOCK_START_WHEN_DRY  0   // 1: refuse to start when no water at the sensor
                                  // 0: start, then cut off after the dry-run timeout
                                  //    (needed when the sensor is at the pipe END and
                                  //     water only arrives after the pump runs)
#define WATER_DRY_DEBOUNCE_MS  300UL   // wet→dry must hold this long (fail-safe: quick)
#define WATER_WET_CONFIRM_MS  1000UL   // dry→wet must hold this long (ignores EMI spikes)

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
#define EE_DRYRUN_MAGIC  82   // 0xD7 when EE_DRYRUN_MS holds a valid value
#define EE_DRYRUN_MS     83   // uint32, dry-run timeout in ms
#define EE_DRYRUN_MAGIC_VAL 0xD7
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

// ── Packets (sent as UDP datagrams) ───────────────────────────────────────

typedef struct __attribute__((packed)) {
  uint8_t type; uint8_t cmd; uint32_t seq;
} CmdPacket;                                        // 0x01

typedef struct __attribute__((packed)) {
  uint8_t type; float current; uint8_t motorOn;
  uint8_t waterOk; uint8_t stall; uint32_t seq;
  int8_t  rssi;                                     // Wi-Fi RSSI to sender AP (dBm)
} StatusPacket;                                     // 0x02

typedef struct __attribute__((packed)) {
  uint8_t type; char msg[59];
} LogPacket;                                        // 0x03  Receiver→Sender

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

// ── Wi-Fi / UDP transport ─────────────────────────────────────────────────
static WiFiUDP       g_udp;
static bool          g_wifiUp      = false;
static unsigned long g_lastWifiTry = 0;

static inline bool wifiIsUp() { return WiFi.status() == WL_CONNECTED; }

// Send one datagram to the sender (the AP's gateway address, 192.168.4.1).
static void commsSend(const uint8_t* data, size_t len) {
  if (!wifiIsUp()) return;
  g_udp.beginPacket(WiFi.gatewayIP(), UDP_PORT);
  g_udp.write(data, len);
  g_udp.endPacket();
}

// ── Log forwarding to the sender ──────────────────────────────────────────
static void sendLogToSender(const char* msg) {
  Serial.println(msg);
  LogPacket pkt; pkt.type = 0x03;
  strncpy(pkt.msg, msg, sizeof(pkt.msg) - 1);
  pkt.msg[sizeof(pkt.msg) - 1] = '\0';
  commsSend((uint8_t*)&pkt, sizeof(pkt));
}

static void sendLogFmt(const char* fmt, ...) {
  char buf[60]; va_list ap;
  va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
  sendLogToSender(buf);
}

// ── ACS712  (5A module, 3.3 V supply) ────────────────────────────────────
static const float ACS_ZERO_V        = 1.65f;
static const float ACS_SENS_V_PER_A  = 0.122f;
static const float CURRENT_RUN_THRESH = 0.10f;
#define ACS_ALPHA     0.15f
#define ACS_SAMPLE_MS 50UL

// ── Servo ─────────────────────────────────────────────────────────────────
#define SERVO_NEUTRAL  90
#define SERVO_START     0
#define SERVO_STOP    180
#define SERVO_HOLD_MS 800UL   // servo travel time before the relay changes
#define SERVO_BACK_MS 500UL   // return-to-neutral time before detaching

// ── Timing constants ──────────────────────────────────────────────────────
#define WATER_TIMEOUT_DEFAULT_MS 10000UL
static uint32_t g_dryRunMs = WATER_TIMEOUT_DEFAULT_MS;
#define STALL_TIMEOUT_MS         5000UL
#define STATUS_INTERVAL_MS       1000UL
#define TIMER_SAVE_MS           10000UL
#define BLINK_MS                  150UL

// ── Servo state machine ───────────────────────────────────────────────────
enum class ServoState  : uint8_t { IDLE, PRESSING, RETURNING };
enum class ServoIntent : uint8_t { NONE, STARTING, STOPPING  };

static Servo         motorServo;
static ServoState    servoState  = ServoState::IDLE;
static ServoIntent   servoIntent = ServoIntent::NONE;
static unsigned long servoTimer  = 0;

// ── Motor / command state ─────────────────────────────────────────────────
static volatile bool     g_pendingOn  = false;
static volatile bool     g_pendingOff = false;
static volatile uint32_t g_lastSeq    = 0;
static volatile bool     g_rxFlag     = false;
static bool              g_motorOn    = false;
static uint8_t           g_queuedCmd  = 0;  // 0=none 1=ON 2=OFF

// ── Water sensor (debounced) ──────────────────────────────────────────────
static bool          g_wet          = false;  // debounced: true = water present
static bool          g_wetRaw       = false;  // last raw reading
static unsigned long g_wetRawSince  = 0;      // when raw reading last changed
static unsigned long g_dryStartMs   = 0;      // when the current dry spell began (0 = not dry)

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
static bool          g_timerActive     = false;
static uint32_t      g_timerTotal      = 0;
static uint32_t      g_timerRemain     = 0;
static bool          g_timerAutoRst    = false;
static unsigned long g_timerLastTick   = 0;
static unsigned long g_timerLastSaveMs = 0;

// ── Schedule state ────────────────────────────────────────────────────────
struct SchedEntry { uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad; };
static uint8_t       g_schedCount = 0;
static SchedEntry    g_scheds[8]  = {};
static int           g_lastActedMinute  = -1;
static unsigned long g_schedLastCheckMs = 0;

// ── RTC / epoch ───────────────────────────────────────────────────────────
static bool          g_rtcEnabled = false;
static uint32_t      g_epoch      = 0;
static unsigned long g_epochMs    = 0;

static uint32_t currentEpoch() { return g_epoch + (uint32_t)((millis() - g_epochMs) / 1000UL); }
static uint8_t  epochDayBit(uint32_t e) { return (uint8_t)(((e / 86400UL) + 4UL) % 7UL); }

// ══════════════════════════════════════════════════════════════════════════
//  EEPROM
// ══════════════════════════════════════════════════════════════════════════

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
    EEPROM.write(b+0, g_scheds[i].startH);     EEPROM.write(b+1, g_scheds[i].startM);
    EEPROM.write(b+2, g_scheds[i].stopH);      EEPROM.write(b+3, g_scheds[i].stopM);
    EEPROM.write(b+4, g_scheds[i].days);       EEPROM.write(b+5, g_scheds[i].autoRestart);
    EEPROM.write(b+6, g_scheds[i].enabled);    EEPROM.write(b+7, 0);
  }
  EEPROM.write(EE_MOTOR_WAS_ON, g_motorOn ? 1 : 0);
  EEPROM.commit();
}

static void eeLoad() {
  if (EEPROM.read(EE_MAGIC) != EE_MAGIC_VAL) { Serial.println("[RECV] EEPROM blank"); return; }
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
  if (EEPROM.read(EE_DRYRUN_MAGIC) == EE_DRYRUN_MAGIC_VAL) {
    uint32_t d = eeReadU32(EE_DRYRUN_MS);
    if (d >= 5000UL && d <= 60000UL) g_dryRunMs = d;
  }
  bool motorWasOn = EEPROM.read(EE_MOTOR_WAS_ON) != 0;
  Serial.printf("[RECV] dry-run timeout %lus\n", (unsigned long)(g_dryRunMs / 1000UL));
  Serial.printf("[RECV] EEPROM  timer=%lus(%s)  sched=%d  motorWasOn=%d\n",
    (unsigned long)g_timerTotal, wasActive?"active":"idle",
    (int)g_schedCount, (int)motorWasOn);
  if (wasActive && g_timerAutoRst && g_timerRemain > 0) {
    g_timerActive = true; g_timerLastTick = millis();
    Serial.printf("[RECV] auto-restart timer: %lus remaining\n", (unsigned long)g_timerRemain);
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  ACS712 EMA — non-blocking, every ACS_SAMPLE_MS
// ══════════════════════════════════════════════════════════════════════════

static void updateCurrentEMA(unsigned long now) {
  if (now - g_acsLastMs < ACS_SAMPLE_MS) return;
  g_acsLastMs = now;
  float voltage = (analogRead(A0) / 1023.0f) * 3.3f;
  float sample  = (voltage - ACS_ZERO_V) / ACS_SENS_V_PER_A;
  g_currentSmooth = ACS_ALPHA * sample + (1.0f - ACS_ALPHA) * g_currentSmooth;
}

// ══════════════════════════════════════════════════════════════════════════
//  Status → sender (UDP)
// ══════════════════════════════════════════════════════════════════════════

static void sendStatusNow() {
  StatusPacket pkt;
  pkt.type    = 0x02;
  pkt.current = g_currentSmooth;
  pkt.motorOn = g_motorOn ? 1 : 0;
  pkt.waterOk = g_wet ? 1 : 0;              // real, debounced sensor state
  pkt.stall   = g_stallDetected ? 1 : 0;
  pkt.seq     = ++g_statusSeq;
  pkt.rssi    = wifiIsUp() ? (int8_t)constrain(WiFi.RSSI(), -127, 0) : 0;
  commsSend((uint8_t*)&pkt, sizeof(pkt));
  g_lastStatusMs = millis();
}

static void sendStatusPeriodic(unsigned long now) {
  if (now - g_lastStatusMs < STATUS_INTERVAL_MS) return;
  sendStatusNow();
  Serial.printf("[RECV] I=%.2fA  relay=%d  water=%s(pin=%s)  stall=%d  timer=%lus  rssi=%ddBm\n",
    g_currentSmooth, (int)g_motorOn,
    g_wet ? "WET" : "DRY", digitalRead(PIN_WATER) == LOW ? "LOW" : "HIGH",
    (int)g_stallDetected, (unsigned long)g_timerRemain,
    wifiIsUp() ? (int)WiFi.RSSI() : 0);
}

// ══════════════════════════════════════════════════════════════════════════
//  Motor helpers
// ══════════════════════════════════════════════════════════════════════════

static void deactivateMotor();

// Servo is attached ONLY while it moves, then released with GPIO0 idling HIGH.
// A permanently attached servo keeps a timer ISR running that jitters the
// Wi-Fi stack, and GPIO0 held LOW at reset would drop the ESP into flash mode.
static void servoMove(int angle) {
  if (!motorServo.attached()) motorServo.attach(PIN_SERVO, 500, 2400);
  motorServo.write(angle);
}
static void servoRelease() {
  motorServo.detach();
  pinMode(PIN_SERVO, OUTPUT);
  digitalWrite(PIN_SERVO, HIGH);
}

static void beginServoPress(ServoIntent intent) {
  servoIntent = intent;
  servoMove(intent == ServoIntent::STARTING ? SERVO_START : SERVO_STOP);
  servoState = ServoState::PRESSING; servoTimer = millis();
  Serial.println(intent == ServoIntent::STARTING ? "[RECV] servo→0° (START)"
                                                  : "[RECV] servo→180° (STOP)");
}

static void activateMotor() {
  if (servoState != ServoState::IDLE || g_motorOn) return;
#if BLOCK_START_WHEN_DRY
  if (!g_wet) {
    sendLogToSender("[RECV] ✗ WATER GUARD — no water, start BLOCKED");
    sendStatusNow();
    return;
  }
#endif
  beginServoPress(ServoIntent::STARTING);
}

static void deactivateMotor() {
  if (servoState != ServoState::IDLE || !g_motorOn) return;
  beginServoPress(ServoIntent::STOPPING);
}

// SAFETY CUT: drop the relay right now, no matter what the servo is doing.
// Used by the dry-run guard.  The servo STOP press (if free) follows as a
// courtesy so the starter's own button state matches.
static void cutMotorNow(const char* why) {
  digitalWrite(PIN_RELAY, LOW);
  g_motorOn = false;
  g_dryStartMs = 0;
  g_stallTimer = 0; g_stallDetected = false;
  g_queuedCmd = 0;
  sendLogToSender(why);
  EEPROM.write(EE_MOTOR_WAS_ON, 0); EEPROM.commit();
  sendStatusNow();
  if (servoState == ServoState::IDLE) beginServoPress(ServoIntent::STOPPING);
}

// ══════════════════════════════════════════════════════════════════════════
//  Servo state machine — relay changes happen here
// ══════════════════════════════════════════════════════════════════════════

static void handleServo(unsigned long now) {
  switch (servoState) {
    case ServoState::PRESSING:
      if (now - servoTimer >= SERVO_HOLD_MS) {
        if (servoIntent == ServoIntent::STARTING) {
          digitalWrite(PIN_RELAY, HIGH);
          g_motorOn = true;
          g_dryStartMs = 0;                       // fresh dry-run window
          g_stallTimer = 0; g_stallDetected = false;
          sendLogToSender("[RECV] ✔ RELAY ON");
          EEPROM.write(EE_MOTOR_WAS_ON, 1); EEPROM.commit();
          sendStatusNow();
        } else if (servoIntent == ServoIntent::STOPPING) {
          digitalWrite(PIN_RELAY, LOW);
          if (g_motorOn) {                        // (already false after a safety cut)
            g_motorOn = false;
            g_dryStartMs = 0;
            g_stallTimer = 0; g_stallDetected = false;
            sendLogToSender("[RECV] ✔ RELAY OFF");
            EEPROM.write(EE_MOTOR_WAS_ON, 0); EEPROM.commit();
            sendStatusNow();
          }
        }
        servoMove(SERVO_NEUTRAL);
        servoState = ServoState::RETURNING; servoTimer = now;
      }
      break;
    case ServoState::RETURNING:
      if (now - servoTimer >= SERVO_BACK_MS) {
        servoRelease();
        servoState = ServoState::IDLE; servoIntent = ServoIntent::NONE;
        Serial.println("[RECV] servo released (neutral)");
        if      (g_queuedCmd == 1) { g_queuedCmd = 0; activateMotor();   }
        else if (g_queuedCmd == 2) { g_queuedCmd = 0; deactivateMotor(); }
      }
      break;
    default: break;
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Water sensor — debounce + dry-run cut-off
// ══════════════════════════════════════════════════════════════════════════

static inline bool waterPinWet() {
#if WATER_ACTIVE_LOW
  return digitalRead(PIN_WATER) == LOW;     // LOW  = water present
#else
  return digitalRead(PIN_WATER) == HIGH;    // HIGH = water present
#endif
}

// Runs every loop, motor ON or OFF: keeps g_wet = stable sensor state.
// Asymmetric debounce: going dry is accepted fast, going wet must be proven.
static void updateWaterSensor(unsigned long now) {
  bool raw = waterPinWet();
  if (raw != g_wetRaw) { g_wetRaw = raw; g_wetRawSince = now; return; }
  if (raw == g_wet) return;
  unsigned long need = raw ? WATER_WET_CONFIRM_MS : WATER_DRY_DEBOUNCE_MS;
  if ((now - g_wetRawSince) >= need) {
    g_wet = raw;
    sendLogToSender(g_wet ? "[RECV] water detected" : "[RECV] water gone");
    sendStatusNow();
  }
}

// Level-triggered dry-run guard (see header).  Called every loop.
static void handleWaterSensor(unsigned long now) {
  if (!g_motorOn) { g_dryStartMs = 0; return; }
  if (g_wet) {
    if (g_dryStartMs != 0) sendLogToSender("[RECV] water back — dry timer reset");
    g_dryStartMs = 0;
    return;
  }
  if (g_dryStartMs == 0) {
    g_dryStartMs = now;
    sendLogFmt("[RECV] ⚠ no water — cut-off in %lus", (unsigned long)(g_dryRunMs / 1000UL));
    return;
  }
  if ((now - g_dryStartMs) >= g_dryRunMs) {
    char msg[60];
    snprintf(msg, sizeof(msg), "[RECV] ✗ NO WATER %lus — relay cut", (unsigned long)(g_dryRunMs / 1000UL));
    cutMotorNow(msg);
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Stall detection
// ══════════════════════════════════════════════════════════════════════════

static void handleStallDetect(unsigned long now) {
  if (!g_motorOn) { g_stallTimer = 0; g_stallDetected = false; return; }
  if (fabsf(g_currentSmooth) < CURRENT_RUN_THRESH) {
    if (g_stallTimer == 0) g_stallTimer = now;
    if ((now - g_stallTimer > STALL_TIMEOUT_MS) && !g_stallDetected) {
      g_stallDetected = true;
      sendLogToSender("[RECV] ⚠ STALL — relay ON but I≈0 for 5 s");
      sendStatusNow();
    }
  } else {
    if (g_stallDetected) { g_stallDetected = false; Serial.println("[RECV] stall cleared"); sendStatusNow(); }
    g_stallTimer = 0;
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Timer tick
// ══════════════════════════════════════════════════════════════════════════

static void handleTimerTick(unsigned long now) {
  if (!g_timerActive) return;
  uint32_t elapsed = (uint32_t)((now - g_timerLastTick) / 1000UL);
  if (elapsed == 0) return;
  g_timerLastTick = now - ((now - g_timerLastTick) % 1000UL);
  if (elapsed >= g_timerRemain) {
    g_timerRemain = 0; g_timerActive = false;
    Serial.println("[RECV] ⏰ timer expired → Motor OFF");
    if (servoState == ServoState::IDLE) deactivateMotor(); else g_queuedCmd = 2;
    EEPROM.write(EE_TIMER_ACTIVE, 0); eeWriteU32(EE_TIMER_REM, 0); EEPROM.commit();
  } else {
    g_timerRemain -= elapsed;
    if (now - g_timerLastSaveMs >= TIMER_SAVE_MS) {
      g_timerLastSaveMs = now;
      eeWriteU32(EE_TIMER_REM, g_timerRemain); EEPROM.commit();
    }
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Scheduler tick
// ══════════════════════════════════════════════════════════════════════════

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

// ══════════════════════════════════════════════════════════════════════════
//  Packet processor
// ══════════════════════════════════════════════════════════════════════════

static void processPacket(uint8_t* data, uint8_t len) {
  if (len < 1) return;
  switch (data[0]) {
    case 0x01: {
      if (len < sizeof(CmdPacket)) return;
      CmdPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      if (pkt.seq == g_lastSeq) return;   // duplicate filter
      g_lastSeq = pkt.seq;
      g_rxFlag  = true;
      if (pkt.cmd == 0x01) g_pendingOn  = true;
      if (pkt.cmd == 0x02) g_pendingOff = true;
      Serial.printf("[RECV] cmd %s  seq=%lu\n",
                    pkt.cmd==0x01?"ON":"OFF", (unsigned long)pkt.seq);
      break;
    }
    case 0x04: {
      if (len < sizeof(TimerPacket)) return;
      TimerPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      g_timerTotal   = pkt.totalSec;
      g_timerRemain  = pkt.remainingSec;
      g_timerAutoRst = (pkt.autoRestart != 0);
      if (pkt.active && !g_timerActive) {
        g_timerActive = true; g_timerLastTick = millis();
        Serial.printf("[RECV] timer set %lus  auto=%d\n",
                      (unsigned long)pkt.totalSec, (int)pkt.autoRestart);
      } else if (!pkt.active) {
        g_timerActive = false; Serial.println("[RECV] timer cancelled");
      }
      eeSave();
      break;
    }
    case 0x05: {
      if (len < 2) return;
      SchedPacket pkt; memcpy(&pkt, data, min((size_t)len, sizeof(pkt)));
      g_schedCount = min((uint8_t)8, pkt.count);
      for (uint8_t i = 0; i < g_schedCount; i++) {
        g_scheds[i].startH      = pkt.entries[i].startH;
        g_scheds[i].startM      = pkt.entries[i].startM;
        g_scheds[i].stopH       = pkt.entries[i].stopH;
        g_scheds[i].stopM       = pkt.entries[i].stopM;
        g_scheds[i].days        = pkt.entries[i].days;
        g_scheds[i].autoRestart = pkt.entries[i].autoRestart;
        g_scheds[i].enabled     = pkt.entries[i].enabled;
      }
      eeSave();
      Serial.printf("[RECV] schedules updated: %d entries\n", (int)g_schedCount);
      break;
    }
    case 0x06: {
      if (len < sizeof(TimeSyncPacket)) return;
      TimeSyncPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      g_epoch = pkt.epoch; g_epochMs = millis();
      g_rtcEnabled = (pkt.rtcEnabled != 0);
      break;
    }
    case 0x07: {
      if (len < sizeof(ConfigPacket)) return;
      ConfigPacket pkt; memcpy(&pkt, data, sizeof(pkt));
      if (pkt.dryRunMs >= 5000UL && pkt.dryRunMs <= 60000UL) {
        g_dryRunMs = pkt.dryRunMs;
        EEPROM.write(EE_DRYRUN_MAGIC, EE_DRYRUN_MAGIC_VAL);
        eeWriteU32(EE_DRYRUN_MS, g_dryRunMs);
        EEPROM.commit();                       // survives reboot / power cut
        sendLogFmt("[RECV] dry-run timeout → %lus", (unsigned long)(g_dryRunMs / 1000UL));
      }
      break;
    }
    default: break;
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Wi-Fi link supervision + UDP receive
// ══════════════════════════════════════════════════════════════════════════

static void handleWifi(unsigned long now) {
  bool up = wifiIsUp();
  if (up && !g_wifiUp) {
    g_wifiUp = true;
    g_udp.stop(); g_udp.begin(UDP_PORT);  // fresh socket after every (re)connect
    Serial.printf("[RECV] Wi-Fi up  ch=%d  rssi=%ddBm  ip=%s\n", (int)WiFi.channel(),
                  (int)WiFi.RSSI(), WiFi.localIP().toString().c_str());
    sendStatusNow();                      // tell the sender straight away
  } else if (!up && g_wifiUp) {
    g_wifiUp = false;
    Serial.println("[RECV] Wi-Fi LOST — retrying");
  }
  if (!up && (now - g_lastWifiTry) >= WIFI_RETRY_MS) {
    g_lastWifiTry = now;
    WiFi.begin(AP_SSID, AP_PASS);         // re-scan; follows the sender's channel
  }
}

// Drain pending datagrams. Only the sender (the AP gateway) is trusted.
static void pollUdp() {
  for (uint8_t n = 0; n < 4; n++) {
    int len = g_udp.parsePacket();
    if (len <= 0) return;
    uint8_t buf[250];
    int got = g_udp.read(buf, sizeof(buf));
    if (got <= 0) continue;
    if (g_udp.remoteIP() != WiFi.gatewayIP()) continue;
    processPacket(buf, (uint8_t)got);
  }
}

// ══════════════════════════════════════════════════════════════════════════
//  Setup
// ══════════════════════════════════════════════════════════════════════════

void setup() {
  // Relay OFF must be first
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_RELAY, LOW);

  Serial.begin(115200);
  delay(100);
  Serial.println("\n[RECV] boot v5  Wi-Fi (UDP)");

  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, HIGH);  // off (active LOW)

  pinMode(PIN_WATER, INPUT_PULLUP);
  g_wetRaw = g_wet = waterPinWet();
  g_wetRawSince = millis();
  Serial.printf("[RECV] water at boot: %s  (pin=%s, %s)\n",
                g_wet ? "WET" : "DRY",
                digitalRead(PIN_WATER) == LOW ? "LOW" : "HIGH",
                WATER_ACTIVE_LOW ? "active-LOW" : "active-HIGH");

  pinMode(PIN_SERVO, OUTPUT);
  digitalWrite(PIN_SERVO, HIGH);          // GPIO0 idles HIGH; servo attaches only when moving

  EEPROM.begin(EE_SIZE);
  eeLoad();

  // Boot blink: 5× rapid = alive, 3× slow = RECEIVER identifier
  for (int i = 0; i < 5; i++) { digitalWrite(PIN_LED,LOW);delay(80);digitalWrite(PIN_LED,HIGH);delay(80); }
  delay(400);
  for (int i = 0; i < 3; i++) { digitalWrite(PIN_LED,LOW);delay(300);digitalWrite(PIN_LED,HIGH);delay(300); }

  // ── Wi-Fi: join the sender's access point ─────────────────────────────
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleepMode(WIFI_NONE_SLEEP);   // modem sleep drops incoming UDP
  WiFi.setOutputPower(20.5f);       // max power: 20.5 dBm
  wifi_set_phy_mode(PHY_MODE_11B);  // 802.11b for best range
  WiFi.begin(AP_SSID, AP_PASS);
  g_lastWifiTry = millis();
  Serial.print("[RECV] joining sender AP");
  unsigned long t = millis();
  while (!wifiIsUp() && millis() - t < AP_TIMEOUT) { delay(200); Serial.print('.'); }
  if (wifiIsUp()) {
    Serial.printf("\n[RECV] joined  ch=%d  rssi=%ddBm  ip=%s\n", (int)WiFi.channel(),
                  (int)WiFi.RSSI(), WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[RECV] sender AP not found yet — will keep retrying in loop()");
  }

  g_udp.begin(UDP_PORT);
  g_wifiUp = wifiIsUp();

  for (int i=0;i<3;i++){digitalWrite(PIN_LED,LOW);delay(80);digitalWrite(PIN_LED,HIGH);delay(80);}
  sendLogToSender("[RECV] ready  Wi-Fi/UDP  power=20.5dBm");
}

// ══════════════════════════════════════════════════════════════════════════
//  Main loop
// ══════════════════════════════════════════════════════════════════════════

void loop() {
  unsigned long now = millis();

  handleWifi(now);
  pollUdp();
  updateCurrentEMA(now);
  updateWaterSensor(now);

  // ── LED: solid = Wi-Fi up, off = down, brief OFF blink on each command ──
  if (g_rxFlag) { g_rxFlag = false; g_blinkUntil = now + BLINK_MS; }
  // signed subtraction handles millis() wrap-around (~49 days)
  bool blinking = (long)(now - g_blinkUntil) < 0;
  digitalWrite(PIN_LED, (g_wifiUp && !blinking) ? LOW : HIGH);

  // ── Command conflict: OFF always wins over ON ─────────────────────────
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
