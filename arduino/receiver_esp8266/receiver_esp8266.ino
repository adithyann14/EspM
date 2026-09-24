/**
 * ════════════════════════════════════════════════════════════════════════
 *  RECEIVER  —  NodeMCU ESP8266  v2  (board: NodeMCU 1.0 / ESP-12E)
 *
 *  ── ESP-NOW packet types handled ─────────────────────────────────────
 *   0x01  CmdPacket       Motor ON / OFF
 *   0x04  TimerPacket     Stop-timer config from Sender
 *   0x05  SchedPacket     Full schedule sync from Sender
 *   0x06  TimeSyncPacket  Unix epoch + RTC flag
 *   0x07  OtaEnablePacket Enter OTA mode → AP "MCReceiver-OTA" for 5 min
 *
 *  ── OTA update ───────────────────────────────────────────────────────
 *   Triggered only by an authenticated 0x07 packet from the Sender.
 *   Receiver switches to WIFI_AP mode, serves ESP8266HTTPUpdateServer
 *   at http://192.168.4.1/update, stays for 5 min then reboots.
 *   Password must match OTA_HTTP_PASS defined below.
 *
 *  ── Wiring ───────────────────────────────────────────────────────────
 *   D1/GPIO5  Relay IN   (active HIGH via level shifter)
 *   D2/GPIO4  Water sensor BC547  (INPUT_PULLUP, active LOW) — dead/tuning
 *   D3/GPIO0  Servo signal  ⚠ boot pin
 *   D4/GPIO2  Built-in LED  (active LOW)
 *   D5/GPIO14 RTC SCL  (if HAS_RTC_HARDWARE) else LoRa dead code
 *   D6/GPIO12 RTC SDA  (if HAS_RTC_HARDWARE) else LoRa dead code
 *   A0        ACS712 current sensor
 *
 *  ── EEPROM layout (128 B, shared with Sender) ────────────────────────
 *   0 magic  1-4 timer total  5 autoRst  6 wasActive  7-10 remaining
 *  11 RTC flag  12-15 epoch  16 sched count  17-80 sched×8  81 motorWasOn
 *
 *  ── Dead code ────────────────────────────────────────────────────────
 *   LoRa SX1278  (D0/D4/D5–D8)
 *   DS3231 RTC   uncomment #define HAS_RTC_HARDWARE + wire to D5/D6
 *   Water sensor (D2) — wired, needs tuning before enable
 * ════════════════════════════════════════════════════════════════════════
 */

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266HTTPUpdateServer.h>
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

// ── LoRa SX1278 dead code ─────────────────────────────────────────────────
// #include <SPI.h>  #include <LoRa.h>
// #define LORA_RST D0  #define LORA_DIO0 D4  #define LORA_SCK D5
// #define LORA_MISO D6  #define LORA_MOSI D7  #define LORA_NSS D8

// ══ CREDENTIAL — must match Sender's OTA_HTTP_PASS ═══════════════════════
#define OTA_HTTP_PASS  "motor123"
#define OTA_HTTP_USER  "admin"
#define OTA_AP_SSID    "MCReceiver-OTA"
#define OTA_TIMEOUT_MS 300000UL   // 5 minutes
// ═════════════════════════════════════════════════════════════════════════

// ── Pins ──────────────────────────────────────────────────────────────────
#define PIN_RELAY  D1   // GPIO5
#define PIN_WATER  D2   // GPIO4  dead/tuning
#define PIN_SERVO  D3   // GPIO0
#define PIN_LED    2    // GPIO2  built-in, active LOW

// ── Sender AP ─────────────────────────────────────────────────────────────
static const char*    AP_SSID    = "MotorControl";
static const char*    AP_PASS    = "motor1234";
static const uint8_t  WIFI_CH    = 1;
static const uint16_t AP_TIMEOUT = 10000;

// ── EEPROM ────────────────────────────────────────────────────────────────
#define EE_SIZE          128
#define EE_MAGIC           0
#define EE_TIMER_TOTAL     1
#define EE_TIMER_AUTO      5
#define EE_TIMER_ACTIVE    6
#define EE_TIMER_REM       7
#define EE_RTC_FLAG       11
#define EE_EPOCH          12
#define EE_SCHED_COUNT    16
#define EE_SCHED_BASE     17
#define EE_MOTOR_WAS_ON   81
#define EE_MAGIC_VAL    0xBE

static inline void eeWriteU32(int a,uint32_t v){
  EEPROM.write(a,v&0xFF);EEPROM.write(a+1,(v>>8)&0xFF);
  EEPROM.write(a+2,(v>>16)&0xFF);EEPROM.write(a+3,(v>>24)&0xFF);}
static inline uint32_t eeReadU32(int a){
  return (uint32_t)EEPROM.read(a)|((uint32_t)EEPROM.read(a+1)<<8)
        |((uint32_t)EEPROM.read(a+2)<<16)|((uint32_t)EEPROM.read(a+3)<<24);}

// ── ESP-NOW packets ────────────────────────────────────────────────────────
static uint8_t BCAST[6]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

typedef struct __attribute__((packed)){uint8_t type;uint8_t cmd;uint32_t seq;}CmdPacket;

typedef struct __attribute__((packed)){
  uint8_t type;float current;uint8_t motorOn;
  uint8_t waterOk;uint8_t stall;uint32_t seq;
}StatusPacket;

typedef struct __attribute__((packed)){
  uint8_t type;uint32_t totalSec;uint32_t remainingSec;
  uint8_t autoRestart;uint8_t active;
}TimerPacket;

typedef struct __attribute__((packed)){
  uint8_t type;uint8_t count;
  struct{uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad;}entries[8];
}SchedPacket;

typedef struct __attribute__((packed)){uint8_t type;uint32_t epoch;uint8_t rtcEnabled;}TimeSyncPacket;

typedef struct __attribute__((packed)){uint8_t type;char pass[32];}OtaEnablePacket;

// ── ACS712 ────────────────────────────────────────────────────────────────
static const float ACS_ZERO_V=1.65f,ACS_SENS=0.122f,RUN_THRESH=0.10f;
#define ACS_ALPHA  0.15f
#define ACS_MS     50UL

// ── Servo ─────────────────────────────────────────────────────────────────
#define SERVO_NEUTRAL 90
#define SERVO_START    0
#define SERVO_STOP   180
#define SERVO_HOLD_MS 600UL
#define SERVO_BACK_MS 400UL

// ── Timeouts ──────────────────────────────────────────────────────────────
#define WATER_TIMEOUT_MS  30000UL
#define STALL_TIMEOUT_MS   5000UL
#define STATUS_INTERVAL_MS 1000UL
#define TIMER_SAVE_MS     10000UL
#define BLINK_MS           150UL

// ── Servo FSM ─────────────────────────────────────────────────────────────
enum class ServoState:uint8_t{IDLE,PRESSING,RETURNING};
enum class ServoIntent:uint8_t{NONE,STARTING,STOPPING};
static Servo       motorServo;
static ServoState  servoState =ServoState::IDLE;
static ServoIntent servoIntent=ServoIntent::NONE;
static unsigned long servoTimer=0;

// ── Command/motor state ───────────────────────────────────────────────────
static volatile bool     g_pendingOn =false,g_pendingOff=false;
static volatile uint32_t g_lastSeq   =0;
static volatile bool     g_rxFlag    =false;
static bool              g_motorOn   =false;
static uint8_t           g_queuedCmd =0;

// ── Water ─────────────────────────────────────────────────────────────────
static bool          g_waterCheck=false;
static unsigned long g_waterStart=0;
static bool          g_waterOk   =false;

// ── ACS712 EMA ────────────────────────────────────────────────────────────
static float         g_current=0.0f;
static unsigned long g_acsMs  =0;

// ── Stall ─────────────────────────────────────────────────────────────────
static unsigned long g_stallTimer=0;
static bool          g_stallDetected=false;

// ── Status ────────────────────────────────────────────────────────────────
static uint32_t      g_statusSeq=0;
static unsigned long g_lastStatusMs=0;

// ── Blink ─────────────────────────────────────────────────────────────────
static unsigned long g_blinkUntil=0;

// ── Timer ─────────────────────────────────────────────────────────────────
static bool          g_timerActive  =false;
static uint32_t      g_timerTotal   =0,g_timerRemain=0;
static bool          g_timerAutoRst =false;
static unsigned long g_timerLastTick=0,g_timerLastSave=0;

// ── Scheduler ─────────────────────────────────────────────────────────────
struct SchedEntry{uint8_t startH,startM,stopH,stopM,days,autoRestart,enabled,pad;};
static uint8_t    g_schedCount=0;
static SchedEntry g_scheds[8]={};
static int        g_lastActedMin=-1;
static unsigned long g_schedLastCheck=0;

// ── RTC ───────────────────────────────────────────────────────────────────
static bool     g_rtcEnabled=false;
static uint32_t g_epoch=0;
static unsigned long g_epochMs=0;
static uint32_t currentEpoch(){return g_epoch+(uint32_t)((millis()-g_epochMs)/1000UL);}
static uint8_t  epochDayBit(uint32_t e){return(uint8_t)(((e/86400UL)+4UL)%7UL);}

// ── OTA mode ──────────────────────────────────────────────────────────────
static bool          g_otaMode    =false;
static unsigned long g_otaStartMs =0;
static ESP8266WebServer          otaServer(80);
static ESP8266HTTPUpdateServer   otaUpdater;

// ═════════════════════════════════════════════════════════════════════════
//  EEPROM
// ═════════════════════════════════════════════════════════════════════════
static void eeSave(){
  EEPROM.write(EE_MAGIC,EE_MAGIC_VAL);
  eeWriteU32(EE_TIMER_TOTAL,g_timerTotal);
  EEPROM.write(EE_TIMER_AUTO,g_timerAutoRst?1:0);
  EEPROM.write(EE_TIMER_ACTIVE,g_timerActive?1:0);
  eeWriteU32(EE_TIMER_REM,g_timerRemain);
  EEPROM.write(EE_RTC_FLAG,g_rtcEnabled?1:0);
  eeWriteU32(EE_EPOCH,currentEpoch());
  EEPROM.write(EE_SCHED_COUNT,g_schedCount);
  for(uint8_t i=0;i<8;i++){
    int b=EE_SCHED_BASE+i*8;
    EEPROM.write(b+0,g_scheds[i].startH);EEPROM.write(b+1,g_scheds[i].startM);
    EEPROM.write(b+2,g_scheds[i].stopH); EEPROM.write(b+3,g_scheds[i].stopM);
    EEPROM.write(b+4,g_scheds[i].days);  EEPROM.write(b+5,g_scheds[i].autoRestart);
    EEPROM.write(b+6,g_scheds[i].enabled);EEPROM.write(b+7,0);
  }
  EEPROM.write(EE_MOTOR_WAS_ON,g_motorOn?1:0);
  EEPROM.commit();
}
static void eeLoad(){
  if(EEPROM.read(EE_MAGIC)!=EE_MAGIC_VAL){Serial.println("[RECV] EEPROM blank");return;}
  g_timerTotal  =eeReadU32(EE_TIMER_TOTAL);
  g_timerAutoRst=EEPROM.read(EE_TIMER_AUTO)!=0;
  bool wasActive=EEPROM.read(EE_TIMER_ACTIVE)!=0;
  g_timerRemain =eeReadU32(EE_TIMER_REM);
  g_rtcEnabled  =EEPROM.read(EE_RTC_FLAG)!=0;
  g_epoch       =eeReadU32(EE_EPOCH);g_epochMs=millis();
  g_schedCount  =min((uint8_t)8,EEPROM.read(EE_SCHED_COUNT));
  for(uint8_t i=0;i<8;i++){
    int b=EE_SCHED_BASE+i*8;
    g_scheds[i].startH=EEPROM.read(b+0);g_scheds[i].startM=EEPROM.read(b+1);
    g_scheds[i].stopH =EEPROM.read(b+2);g_scheds[i].stopM =EEPROM.read(b+3);
    g_scheds[i].days  =EEPROM.read(b+4);g_scheds[i].autoRestart=EEPROM.read(b+5);
    g_scheds[i].enabled=EEPROM.read(b+6);
  }
  bool motorWasOn=EEPROM.read(EE_MOTOR_WAS_ON)!=0;
  Serial.printf("[RECV] EEPROM: timer=%lus(%s) sched=%d motorWasOn=%d\n",
    (unsigned long)g_timerTotal,wasActive?"active":"idle",(int)g_schedCount,(int)motorWasOn);
  if(wasActive&&g_timerAutoRst&&g_timerRemain>0){
    g_timerActive=true;g_timerLastTick=millis();
    Serial.printf("[RECV] auto-restart timer: %lus remain\n",(unsigned long)g_timerRemain);
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  ACS712 EMA
// ═════════════════════════════════════════════════════════════════════════
static void updateCurrentEMA(unsigned long now){
  if(now-g_acsMs<ACS_MS)return;g_acsMs=now;
  float v=(analogRead(A0)/1023.0f)*3.3f;
  float s=(v-ACS_ZERO_V)/ACS_SENS;
  g_current=ACS_ALPHA*s+(1.0f-ACS_ALPHA)*g_current;
}

// ═════════════════════════════════════════════════════════════════════════
//  Status broadcast
// ═════════════════════════════════════════════════════════════════════════
static void sendStatusNow(){
  StatusPacket p;p.type=0x02;p.current=g_current;p.motorOn=g_motorOn?1:0;
  p.waterOk=(g_waterOk||!g_waterCheck)?1:0;
  p.stall=g_stallDetected?1:0;p.seq=++g_statusSeq;
  esp_now_send(BCAST,(uint8_t*)&p,sizeof(p));
  g_lastStatusMs=millis();
}
static void sendStatusPeriodic(unsigned long now){
  if(now-g_lastStatusMs<STATUS_INTERVAL_MS)return;
  sendStatusNow();
  Serial.printf("[RECV] I=%.2fA relay=%d water=%s stall=%d timer=%lus\n",
    g_current,(int)g_motorOn,g_waterOk?"OK":(g_waterCheck?"WAIT":"IDLE"),
    (int)g_stallDetected,(unsigned long)g_timerRemain);
}

// ═════════════════════════════════════════════════════════════════════════
//  Servo / relay helpers
// ═════════════════════════════════════════════════════════════════════════
static void activateMotor(){
  if(servoState!=ServoState::IDLE||g_motorOn)return;
  servoIntent=ServoIntent::STARTING;motorServo.write(SERVO_START);
  servoState=ServoState::PRESSING;servoTimer=millis();
  Serial.println("[RECV] servo→0° (relay fires after hold)");
}
static void deactivateMotor(){
  if(servoState!=ServoState::IDLE||!g_motorOn)return;
  servoIntent=ServoIntent::STOPPING;motorServo.write(SERVO_STOP);
  servoState=ServoState::PRESSING;servoTimer=millis();
  g_waterCheck=false;g_waterOk=false;
  Serial.println("[RECV] servo→180° (relay cuts after hold)");
}

// ═════════════════════════════════════════════════════════════════════════
//  Servo FSM — relay changes here only
// ═════════════════════════════════════════════════════════════════════════
static void handleServo(unsigned long now){
  switch(servoState){
    case ServoState::PRESSING:
      if(now-servoTimer>=SERVO_HOLD_MS){
        if(servoIntent==ServoIntent::STARTING){
          digitalWrite(PIN_RELAY,HIGH);g_motorOn=true;
          g_waterCheck=true;g_waterStart=now;g_waterOk=false;
          g_stallTimer=0;g_stallDetected=false;
          EEPROM.write(EE_MOTOR_WAS_ON,1);EEPROM.commit();
          Serial.println("[RECV] ✔ RELAY ON");sendStatusNow();
        }else if(servoIntent==ServoIntent::STOPPING){
          digitalWrite(PIN_RELAY,LOW);g_motorOn=false;
          g_stallTimer=0;g_stallDetected=false;
          EEPROM.write(EE_MOTOR_WAS_ON,0);EEPROM.commit();
          Serial.println("[RECV] ✔ RELAY OFF");sendStatusNow();
        }
        motorServo.write(SERVO_NEUTRAL);
        servoState=ServoState::RETURNING;servoTimer=now;
      }break;
    case ServoState::RETURNING:
      if(now-servoTimer>=SERVO_BACK_MS){
        servoState=ServoState::IDLE;servoIntent=ServoIntent::NONE;
        Serial.println("[RECV] servo→90° neutral");
        if(g_queuedCmd==1){g_queuedCmd=0;activateMotor();}
        else if(g_queuedCmd==2){g_queuedCmd=0;deactivateMotor();}
      }break;
    default:break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Water sensor
// ═════════════════════════════════════════════════════════════════════════
static void handleWaterSensor(unsigned long now){
  if(!g_waterCheck||g_waterOk)return;
  if(digitalRead(PIN_WATER)==LOW){
    g_waterOk=true;g_waterCheck=false;
    Serial.println("[RECV] ✔ water detected");sendStatusNow();return;
  }
  if(now-g_waterStart>=WATER_TIMEOUT_MS){
    Serial.println("[RECV] ✗ WATER TIMEOUT 30s — auto cut-off");
    g_waterCheck=false;
    if(servoState==ServoState::IDLE)deactivateMotor();else g_queuedCmd=2;
    sendStatusNow();
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Stall detection
// ═════════════════════════════════════════════════════════════════════════
static void handleStallDetect(unsigned long now){
  if(!g_motorOn){g_stallTimer=0;g_stallDetected=false;return;}
  if(fabsf(g_current)<RUN_THRESH){
    if(!g_stallTimer)g_stallTimer=now;
    if((now-g_stallTimer>STALL_TIMEOUT_MS)&&!g_stallDetected){
      g_stallDetected=true;
      Serial.println("[RECV] ⚠ STALL — relay ON but I≈0 for 5s");sendStatusNow();
    }
  }else{
    if(g_stallDetected){g_stallDetected=false;Serial.println("[RECV] stall cleared");sendStatusNow();}
    g_stallTimer=0;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Timer tick
// ═════════════════════════════════════════════════════════════════════════
static void handleTimerTick(unsigned long now){
  if(!g_timerActive)return;
  uint32_t el=(uint32_t)((now-g_timerLastTick)/1000UL);if(!el)return;
  g_timerLastTick=now-((now-g_timerLastTick)%1000UL);
  if(el>=g_timerRemain){
    g_timerRemain=0;g_timerActive=false;
    Serial.println("[RECV] ⏰ timer expired → Motor OFF");
    if(servoState==ServoState::IDLE)deactivateMotor();else g_queuedCmd=2;
    EEPROM.write(EE_TIMER_ACTIVE,0);eeWriteU32(EE_TIMER_REM,0);EEPROM.commit();
  }else{
    g_timerRemain-=el;
    if(now-g_timerLastSave>=TIMER_SAVE_MS){
      g_timerLastSave=now;eeWriteU32(EE_TIMER_REM,g_timerRemain);EEPROM.commit();
    }
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Scheduler tick
// ═════════════════════════════════════════════════════════════════════════
static void handleScheduler(unsigned long now){
  if(now-g_schedLastCheck<15000UL)return;g_schedLastCheck=now;
  if(!g_epoch)return;
  uint32_t ep=currentEpoch();
  uint16_t tod=(uint16_t)((ep%86400UL)/60UL);uint8_t day=epochDayBit(ep);
  if((int)tod==g_lastActedMin)return;
  for(uint8_t i=0;i<g_schedCount;i++){
    SchedEntry&s=g_scheds[i];if(!s.enabled||!(s.days&(1<<day)))continue;
    uint16_t st=s.startH*60+s.startM,sp=s.stopH*60+s.stopM;
    if(tod==st){Serial.printf("[SCHED] %d→ON\n",i);
      if(servoState==ServoState::IDLE)activateMotor();else g_queuedCmd=1;
      g_lastActedMin=(int)tod;break;}
    if(tod==sp){Serial.printf("[SCHED] %d→OFF\n",i);
      if(servoState==ServoState::IDLE)deactivateMotor();else g_queuedCmd=2;
      g_lastActedMin=(int)tod;break;}
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  OTA mode — called when 0x07 packet is received with correct password
// ═════════════════════════════════════════════════════════════════════════
static void enterOtaMode(const char* pass){
  Serial.println("[RECV] entering OTA mode");
  // Move servo to neutral and hold relay state before switching Wi-Fi
  motorServo.write(SERVO_NEUTRAL);

  WiFi.mode(WIFI_AP);
  WiFi.softAP(OTA_AP_SSID, pass);   // password from packet (same as OTA_HTTP_PASS)
  Serial.printf("[RECV] OTA AP: \"%s\"  IP: %s\n",
    OTA_AP_SSID, WiFi.softAPIP().toString().c_str());

  otaUpdater.setup(&otaServer, "/update", OTA_HTTP_USER, OTA_HTTP_PASS);
  otaServer.begin();

  g_otaMode    = true;
  g_otaStartMs = millis();
  Serial.println("[RECV] OTA ready — connect phone to MCReceiver-OTA");
  Serial.println("[RECV]   then POST .bin to http://192.168.4.1/update");

  // Blink LED rapidly to show OTA mode
  for(int i=0;i<6;i++){digitalWrite(PIN_LED,LOW);delay(120);digitalWrite(PIN_LED,HIGH);delay(120);}
}

// ═════════════════════════════════════════════════════════════════════════
//  ESP-NOW callbacks — set flags only, no delays, no Serial in cb
// ═════════════════════════════════════════════════════════════════════════
// Pending OTA pass (set in callback, processed in loop)
static char g_pendingOtaPass[33] = {0};
static volatile bool g_pendingOta = false;

void espnowOnSend(uint8_t*,uint8_t){}

void espnowOnRecv(uint8_t*,uint8_t* data,uint8_t len){
  if(len<1)return;
  switch(data[0]){
    case 0x01:{
      if(len<sizeof(CmdPacket))return;
      CmdPacket p;memcpy(&p,data,sizeof(p));
      if(p.seq==g_lastSeq)return;g_lastSeq=p.seq;g_rxFlag=true;
      if(p.cmd==0x01)g_pendingOn =true;
      if(p.cmd==0x02)g_pendingOff=true;
      break;}
    case 0x04:{
      if(len<sizeof(TimerPacket))return;
      TimerPacket p;memcpy(&p,data,sizeof(p));
      g_timerTotal=p.totalSec;g_timerRemain=p.remainingSec;
      g_timerAutoRst=(p.autoRestart!=0);
      if(p.active&&!g_timerActive){g_timerActive=true;g_timerLastTick=millis();}
      else if(!p.active){g_timerActive=false;}
      eeSave();break;}
    case 0x05:{
      if(len<2)return;
      SchedPacket p;memcpy(&p,data,min((size_t)len,sizeof(p)));
      g_schedCount=min((uint8_t)8,p.count);
      for(uint8_t i=0;i<g_schedCount;i++){
        g_scheds[i].startH    =p.entries[i].startH;g_scheds[i].startM=p.entries[i].startM;
        g_scheds[i].stopH     =p.entries[i].stopH; g_scheds[i].stopM =p.entries[i].stopM;
        g_scheds[i].days      =p.entries[i].days;  g_scheds[i].autoRestart=p.entries[i].autoRestart;
        g_scheds[i].enabled   =p.entries[i].enabled;
      }
      eeSave();break;}
    case 0x06:{
      if(len<sizeof(TimeSyncPacket))return;
      TimeSyncPacket p;memcpy(&p,data,sizeof(p));
      g_epoch=p.epoch;g_epochMs=millis();g_rtcEnabled=(p.rtcEnabled!=0);
      break;}
    case 0x07:{
      // OTA enable packet — verify password then set flag for main loop
      if(len<sizeof(OtaEnablePacket))return;
      OtaEnablePacket p;memcpy(&p,data,sizeof(p));p.pass[31]='\0';
      if(strcmp(p.pass,OTA_HTTP_PASS)==0){
        strncpy(g_pendingOtaPass,p.pass,32);
        g_pendingOta=true;
      }
      // Wrong password: silently ignore
      break;}
    default:break;
  }
}

// ═════════════════════════════════════════════════════════════════════════
//  Setup
// ═════════════════════════════════════════════════════════════════════════
void setup(){
  pinMode(PIN_RELAY,OUTPUT);digitalWrite(PIN_RELAY,LOW);  // relay OFF first

  Serial.begin(115200);delay(100);Serial.println("\n[RECV] boot v2");

  pinMode(PIN_LED,OUTPUT);digitalWrite(PIN_LED,HIGH);
  pinMode(PIN_WATER,INPUT);
  Serial.printf("[RECV] water at boot: %s\n",digitalRead(PIN_WATER)==LOW?"WET":"DRY");

  motorServo.attach(PIN_SERVO,500,2400);
  motorServo.write(SERVO_NEUTRAL);delay(500);
  Serial.println("[RECV] servo @ 90°");

  EEPROM.begin(EE_SIZE);eeLoad();

  // Boot blink: 5× rapid then 3× slow = receiver identifier
  for(int i=0;i<5;i++){digitalWrite(PIN_LED,LOW);delay(80);digitalWrite(PIN_LED,HIGH);delay(80);}
  delay(400);
  for(int i=0;i<3;i++){digitalWrite(PIN_LED,LOW);delay(300);digitalWrite(PIN_LED,HIGH);delay(300);}

  WiFi.persistent(false);WiFi.mode(WIFI_STA);
  WiFi.begin(AP_SSID,AP_PASS);WiFi.setOutputPower(20.5f);wifi_set_phy_mode(PHY_MODE_11B);
  Serial.print("[RECV] connecting to AP");
  unsigned long t=millis();
  while(WiFi.status()!=WL_CONNECTED&&millis()-t<AP_TIMEOUT){delay(200);Serial.print('.');}
  if(WiFi.status()==WL_CONNECTED){
    Serial.printf("\n[RECV] joined  IP=%s  ch=%d\n",
      WiFi.localIP().toString().c_str(),(int)WiFi.channel());
  }else{
    Serial.println("\n[RECV] AP not found — forcing ch="+String(WIFI_CH));
    WiFi.disconnect();wifi_set_channel(WIFI_CH);
  }

  int tries=3;
  while(esp_now_init()!=0&&tries-->0){Serial.println("[RECV] ESP-NOW retry");delay(500);}
  if(tries<0){Serial.println("[RECV] ESP-NOW FAILED — reboot");delay(1000);ESP.restart();}
  esp_now_set_self_role(ESP_NOW_ROLE_COMBO);
  esp_now_register_send_cb(espnowOnSend);
  esp_now_register_recv_cb(espnowOnRecv);
  esp_now_add_peer(BCAST,ESP_NOW_ROLE_COMBO,WIFI_CH,nullptr,0);
  for(int i=0;i<3;i++){digitalWrite(PIN_LED,LOW);delay(80);digitalWrite(PIN_LED,HIGH);delay(80);}
  Serial.println("[RECV] ready");
}

// ═════════════════════════════════════════════════════════════════════════
//  Main loop
// ═════════════════════════════════════════════════════════════════════════
void loop(){
  unsigned long now=millis();

  // ── OTA mode: dedicated loop ──────────────────────────────────────────
  if(g_otaMode){
    otaServer.handleClient();
    // Slow blink to show OTA is active
    if((now/1000)%2==0)digitalWrite(PIN_LED,LOW);else digitalWrite(PIN_LED,HIGH);
    if(now-g_otaStartMs>OTA_TIMEOUT_MS){
      Serial.println("[RECV] OTA timeout — rebooting");delay(500);ESP.restart();
    }
    return;  // skip all normal operations while in OTA mode
  }

  // ── Process pending OTA request (from ESP-NOW callback) ───────────────
  if(g_pendingOta){
    g_pendingOta=false;
    // Save state before entering OTA — relay stays as-is
    eeSave();
    enterOtaMode(g_pendingOtaPass);
    return;
  }

  updateCurrentEMA(now);

  if(g_rxFlag){g_rxFlag=false;g_blinkUntil=now+BLINK_MS;digitalWrite(PIN_LED,LOW);}
  if(now>=g_blinkUntil)digitalWrite(PIN_LED,HIGH);

  // OFF wins over ON
  if(g_pendingOff&&g_pendingOn)g_pendingOn=false;

  if(g_pendingOn){
    g_pendingOn=false;
    if(servoState==ServoState::IDLE&&!g_motorOn)activateMotor();
    else if(servoState!=ServoState::IDLE)g_queuedCmd=1;
  }
  if(g_pendingOff){
    g_pendingOff=false;
    if(servoState==ServoState::IDLE&&g_motorOn)deactivateMotor();
    else if(servoState!=ServoState::IDLE)g_queuedCmd=2;
  }

  handleServo(now);
  handleWaterSensor(now);
  handleStallDetect(now);
  handleTimerTick(now);
  handleScheduler(now);
  sendStatusPeriodic(now);
}
