/* ============================================================================
   Smart Hospital Assistant Robot  --  CAR firmware  (v2.0)
   Board: ESP32 DevKit V1 (30-pin)   |   Arduino core for ESP32 3.x
   ----------------------------------------------------------------------------
   WHAT CHANGED FROM v1.x
     - No servo, no camera, no PC application, no QR. The glovebox servo is
       dead; the medicine box lid is now opened and closed by hand.
     - This board now hosts its OWN control website and talks to the
       dispenser directly over ESP-NOW -- no PC in the loop at all.
     - LID_SWITCH_FITTED is hard-set false below: per an explicit decision,
       no new hardware is being added. A CONFIRM action (web button, a
       second press of the physical START button, or serial 'k') stands in
       for the lid switch event described in the plan's section 5. Flipping
       that constant true and wiring GPIO16 is the only change needed if a
       real switch is added later -- every state name and transition stays
       identical either way.
     - The one safety property genuinely lost by skipping the switch: the
       car cannot detect the lid opening during travel. Say so in the report.

   RADIO PROTOCOL -- MUST MATCH THE DISPENSER EXACTLY, BYTE FOR BYTE
     This struct, enum, and broadcast MAC are copied from the kept
     dispenser.ino so the two boards can actually talk to each other. Do not
     change field order, types, or values here without changing them there.

   ESP-NOW CALLBACK SIGNATURES
     Arduino-ESP32 core 3.3.x changed esp_now_register_send_cb's expected
     signature to take a wifi_tx_info_t* instead of a raw MAC pointer. Using
     the old signature is a compile error on this core version, not a
     runtime problem -- see onDataSent() below.

   Uses millis() throughout the live mission; only on-demand bench/serial
   calibration routines (motor test, spin test) are allowed to block
   briefly, matching the same convention already used in the dispenser.
   ============================================================================ */

#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <ESPmDNS.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include "esp_system.h"
#include <stddef.h>


/* ============================================================================
   1. USER CONFIG
   ============================================================================ */

const char* WIFI_SSID = "froggy";
const char* WIFI_PASS = "Passwordki";

// Printed at every boot and exposed by /status. If this exact text is not in
// the Serial Monitor, this file was not flashed to that board.
const char FIRMWARE_BUILD[] = "CAR-V2.1-DIAG-LCD-BUZZ-550MS-20260902";

// The one decision this file is built around. See the header comment.
const bool LID_SWITCH_FITTED = false;

// Verified on the actual five-channel array: a sensor output is HIGH when it
// is over black and LOW when it is over white.
const bool IR_BLACK_IS_HIGH = true;

const bool BUZZER_ENABLED = false;   // flip true once the transistor driver is fitted


/* ============================================================================
   2. PIN MAP  (section 7)
   ============================================================================ */

const int IR1 = 36, IR2 = 39, IR3 = 34, IR4 = 35, IR5 = 17;   // far-L..far-R, IR3 = centre

const int ENA = 14, IN1 = 27, IN2 = 26, IN3 = 25, IN4 = 33, ENB = 23;

const int TRIG = 13, ECHO = 4;      // ECHO via 1k/2k divider, never direct

const int BUZZER = 19;              // needs the transistor driver -- see BUZZER_ENABLED

const int LCD_SDA = 21, LCD_SCL = 22;   // through the level shifter

const int START_BUTTON = 18;   // freed by the dead servo. Not a strapping pin.
const int LID_SWITCH    = 16;   // only read if LID_SWITCH_FITTED. Not a strapping pin.

// GPIO18 and GPIO16 are safe strapping-wise. Do NOT move either to
// GPIO0, 2, 5, 12 or 15 without checking the strapping table first.


/* ============================================================================
   3. PWM  (section 7 -- explicitly pinned, nothing auto-allocated)
   ============================================================================ */

const int PWM_FREQ = 1000;
const int PWM_RES  = 8;


/* ============================================================================
   4. TUNING  (section 18 -- calibrate every one of these on the real chassis)
   ============================================================================ */

int CRUISE_PWM      = 150;   // TODO: 140-160, tune on the real floor
int CREEP_PWM        = 105;
int SPIN_PWM          = 110;
int CORRECTION_LIMIT = 95;    // TODO: 80-110
int LEFT_TRIM        = 0;     // TODO: signed trim so both wheels track straight
int RIGHT_TRIM       = 0;

// Physical calibration: 1100 ms produced approximately 360 degrees, so the
// measured 180-degree starting value is half of that.
unsigned long SPIN_180_MS = 1100;

const unsigned long LINE_LOST_ARC_MS   = 500;    // phase 1: gentle arc
const unsigned long LINE_LOST_FAULT_MS = 1200;   // phase 2 ends here -> FAULT

const unsigned long MARKER_STOP_DEBOUNCE_MS = 80;    // stationary confirm time
const unsigned long MARKER_REARM_MS         = 300;   // normal-line time to re-arm
const unsigned long MARKER_CREEP_TIMEOUT_MS = 3000;  // departure creep safety cap

int OBSTACLE_CAUTION_CM = 15;   // TODO: raise until the 4cm stop is met without contact
const int OBSTACLE_STOP_CM   = 4;
const int OBSTACLE_RESUME_CM = 8;
const unsigned long OBSTACLE_RESUME_STABLE_MS = 1500;
const unsigned long SONAR_SAMPLE_MS = 50;
const unsigned long SONAR_ECHO_TIMEOUT_US = 6000;
const int SONAR_STICKY_MAX_TIMEOUTS = 5;

const unsigned long LOAD_COUNTDOWN_MS = 5000;
const unsigned long BED_WINDOW_MS     = 10000;
const unsigned long AT_STATION_DWELL_MS = 800;   // let the arrival message/beep actually show

const unsigned long ACK_RETRY_MS       = 1000;   // TODO: tune -- gap between DROP_REQUEST retries
const int            ACK_MAX_RETRIES    = 5;
const unsigned long GATE_CONFIRM_GRACE_MS = 5000;   // ack received, waiting on DONE
const unsigned long DISPENSER_ONLINE_TIMEOUT_MS = 3000;   // no heartbeat this long = offline

const unsigned long BTN_DEBOUNCE_MS = 50;

unsigned long monitorIntervalMs = 500;
bool monitorOn = true;


/* ============================================================================
   5. RADIO PROTOCOL  (copied from the kept dispenser.ino -- must match)
   ============================================================================ */

const uint32_t RADIO_MAGIC   = 0x4D454452;   // "MEDR"
const uint8_t  RADIO_VERSION = 2;

enum RadioMessageType : uint8_t {
  MSG_DISPENSER_HEARTBEAT = 1,
  MSG_DROP_REQUEST        = 2,
  MSG_DROP_ACK            = 3,
  MSG_DROP_DONE           = 4
};

struct __attribute__((packed)) RadioPacket {
  uint32_t magic;
  uint8_t  version;
  uint8_t  type;
  uint8_t  deviceState;   // dispenser's own DispenserState enum value
  uint8_t  resultCode;    // ACK: 0=new 1=already-safe(duplicate) 2=rejected/fault
  uint32_t requestId;     // HEARTBEAT reuses this field to carry lastCompletedId
};

constexpr size_t RADIO_PACKET_BYTES = 12;
static_assert(sizeof(RadioPacket) == RADIO_PACKET_BYTES,
              "RadioPacket layout changed: update BOTH car and dispenser");
static_assert(offsetof(RadioPacket, magic) == 0 &&
              offsetof(RadioPacket, version) == 4 &&
              offsetof(RadioPacket, type) == 5 &&
              offsetof(RadioPacket, deviceState) == 6 &&
              offsetof(RadioPacket, resultCode) == 7 &&
              offsetof(RadioPacket, requestId) == 8,
              "RadioPacket field offsets changed");

// Dispenser's own state numbering (from its DispenserState enum) -- the car
// only needs to recognise READY, everything else is display-only.
const uint8_t DISP_STATE_READY = 1;

const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };


/* ============================================================================
   6. MISSION STATES  (section 4)
   ============================================================================ */

enum MissionState : uint8_t {
  M_BOOT,
  M_WAIT_WIFI,
  M_WAIT_DISPENSER,
  M_REPOSITION,        // every boot: operator confirms placement before AT_DISPENSER
  M_AT_DISPENSER,
  M_WAIT_DROP_ACK,
  M_WAIT_GATE_DONE,
  M_WAIT_LID,
  M_LOAD_COUNTDOWN,
  M_DEPART_STATION,
  M_OUTBOUND,
  M_AT_BED,
  M_BED_TURN,
  M_DEPART_BED,
  M_RETURNING,
  M_AT_STATION,
  M_STATION_TURN,
  M_OBSTACLE_PAUSE,
  M_FAULT,
  M_EMERGENCY_STOP
};

MissionState state = M_BOOT;
MissionState stateBeforeObstacle = M_OUTBOUND;   // resume target after OBSTACLE_PAUSE

unsigned long tState = 0;         // when the current state was entered
char faultReason[40] = "NONE";

bool returning = false;   // false = OUTBOUND leg, true = RETURNING leg -- this
                            // is what tells the two identical markers apart


/* ============================================================================
   7. DROP-REQUEST / RADIO SESSION STATE
   ============================================================================ */

uint32_t activeRequestId = 0;
int      ackRetries      = 0;
unsigned long tLastAckAttempt = 0;
unsigned long tAckReceivedAt  = 0;
volatile bool ackReceivedFlag  = false;
volatile bool doneReceivedFlag = false;
volatile uint8_t lastAckResult = 0;

bool     dispenserOnline = false;
bool     espNowReady    = false;   // set from esp_now_init()'s actual return value
bool     espNowPeerOk   = false;   // set from esp_now_add_peer()'s actual return value
bool     espNowRecvCallbackOk = false;
bool     espNowSendCallbackOk = false;
volatile uint32_t rawPacketsSeen  = 0;   // ANY ESP-NOW packet, before any filtering
volatile uint32_t rawPacketsValid = 0;   // packets that also passed magic+version
volatile uint32_t rawPacketsWrongLength = 0;
volatile uint32_t rawPacketsWrongProtocol = 0;
volatile int lastRawPacketLength = -1;
volatile int lastTxPacketLength = 0;
volatile uint32_t txPacketsQueued = 0;
volatile uint32_t txQueueErrors = 0;
volatile uint32_t txDelivered = 0;
volatile uint32_t txDeliveryFailed = 0;
char lastRawSenderMac[18] = "none";       // last sender MAC seen, any packet, for eyeballing
unsigned long tLastDispenserHeartbeat = 0;
uint8_t  dispenserLastState = 0;
uint32_t dispenserLastCompletedId = 0;

// ESP-NOW callbacks run in the WiFi/system task, not the sketch's task, so
// the callback only ever sets flags/copies small values under a critical
// section -- exactly the pattern the dispenser uses, for the same reason.
portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;


/* ============================================================================
   8. SENSOR / OUTPUT STATE
   ============================================================================ */

long lastDistanceCm = 999;
long lastSonarRawCm = -1;
unsigned long sonarTimeoutCount = 0;
int  sonarStickyTimeouts = 0;
unsigned long tLastSonarSample = 0;
unsigned long tObstacleClearSince = 0;
unsigned long tObstaclePauseStarted = 0;
bool obstacleActive = false;
bool obstacleCaution = false;
int  closeReadingStreak = 0;

int lastLeftPWM = 0, lastRightPWM = 0;

int lineLostDirection = 1;    // +1 or -1, last non-zero correction sign
unsigned long tLineLost = 0;

unsigned long tMarkerStopCandidate = 0;
bool markerCandidate = false;
bool markerArmed = true;
unsigned long tNormalLineSince = 0;
unsigned long tMarkerCreepStart = 0;

// spin sub-phase (shared by BED_TURN and STATION_TURN)
enum { SP_BLIND, SP_SLOW, SP_DONE };
int spinPhase = 0;
unsigned long tSpinPhase = 0;

int  lastBtnReading = HIGH;
unsigned long tBtnChange = 0;
bool startPressHandled = false;

bool lcdPresent = false;
uint8_t lcdAddress = 0;
String lastLcdLine1 = "", lastLcdLine2 = "";

bool motorPwmReady = false;

unsigned long tLastMonitor = 0;
int monitorRowsSinceHeader = 0;


/* ============================================================================
   OBJECTS
   ============================================================================ */

WebServer server(80);
LiquidCrystal_I2C* lcd = nullptr;

bool lidSwitchClosed();


/* ============================================================================
   9. LOW-LEVEL: MOTORS
   ============================================================================ */

void motorsStop() {
  ledcWrite(ENA, 0);
  ledcWrite(ENB, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  lastLeftPWM = 0; lastRightPWM = 0;
}

// L298N dynamic braking: EN pins driven high while both direction inputs on
// each motor are equal. Marker confirmation holds this for only 80 ms, then
// the stationary mission state calls motorsStop() and returns to freewheel.
void motorsBrake() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
  ledcWrite(ENA, 255);
  ledcWrite(ENB, 255);
  lastLeftPWM = 0; lastRightPWM = 0;
}

void drive(int left, int right) {
  left  += LEFT_TRIM;
  right += RIGHT_TRIM;
  lastLeftPWM = left; lastRightPWM = right;

  if (left >= 0) { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); }
  else           { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); left = -left; }
  if (right >= 0){ digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW); }
  else           { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); right = -right; }

  if (left  > 255) left  = 255;
  if (right > 255) right = 255;
  ledcWrite(ENA, left);
  ledcWrite(ENB, right);
}


/* ============================================================================
   10. LOW-LEVEL: IR ARRAY
   ----------------------------------------------------------------------------
   Confirmed from the live test: centre sensor on black produced 11011 under
   the old inverted logic. Therefore this array reads raw HIGH over black and
   raw LOW over white. Normalised here so `true` always means black.
   ============================================================================ */

int readIR(bool b[5]) {
  const int pins[5] = {IR1, IR2, IR3, IR4, IR5};
  for (int i = 0; i < 5; i++) {
    int raw = digitalRead(pins[i]);
    b[i] = IR_BLACK_IS_HIGH ? (raw == HIGH) : (raw == LOW);
  }
  int n = 0;
  for (int i = 0; i < 5; i++) if (b[i]) n++;
  return n;
}

float linePosition(bool b[5]) {
  const int w[5] = {-2, -1, 0, 1, 2};
  int sum = 0, cnt = 0;
  for (int i = 0; i < 5; i++) if (b[i]) { sum += w[i]; cnt++; }
  return cnt ? (float)sum / cnt : 0;
}


/* ============================================================================
   11. LOW-LEVEL: ULTRASONIC  (section 11 -- three-distance obstacle design)
   ============================================================================ */

long pingCmRaw() {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long us = pulseIn(ECHO, HIGH, SONAR_ECHO_TIMEOUT_US);
  return us ? us / 58 : -1;   // -1 = timeout, distinct from a real reading
}

// Applies the sticky-close rule and returns the distance to ACT on (999 when
// genuinely clear). Call at most every SONAR_SAMPLE_MS.
long sonarUpdate() {
  long raw = pingCmRaw();
  lastSonarRawCm = raw;

  if (raw < 0) {
    sonarTimeoutCount++;
    // timeout. If we were already close, treat it as still-close for a
    // bounded number of samples (the sensor's own blind zone starts near
    // where OBSTACLE_CAUTION_CM already is) rather than reporting clear.
    if (lastDistanceCm <= OBSTACLE_CAUTION_CM && sonarStickyTimeouts < SONAR_STICKY_MAX_TIMEOUTS) {
      sonarStickyTimeouts++;
      return lastDistanceCm;   // hold the last close reading
    }
    sonarStickyTimeouts = 0;
    lastDistanceCm = 999;
    return 999;
  }

  sonarStickyTimeouts = 0;
  lastDistanceCm = raw;
  return raw;
}


/* ============================================================================
   12. LCD  (section 12 -- optional, auto-detected, degrades gracefully)
   ============================================================================ */

void lcdProbe() {
  lcdPresent = false;
  lcdAddress = 0;
  lastLcdLine1 = "";
  lastLcdLine2 = "";
  if (lcd != nullptr) {
    delete lcd;
    lcd = nullptr;
  }
  Wire.begin(LCD_SDA, LCD_SCL);
  // 0x27 and 0x3F are most common. The rest cover the normal PCF8574/
  // PCF8574A address ranges so a valid backpack is not falsely reported dead.
  const uint8_t candidates[] = {
    0x27, 0x3F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25,
    0x26, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E
  };
  for (uint8_t addr : candidates) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      lcd = new LiquidCrystal_I2C(addr, 16, 2);
      lcd->init();
      lcd->backlight();
      lcdPresent = true;
      lcdAddress = addr;
      Serial.printf("[LCD] found at 0x%02X\n", addr);
      return;
    }
  }
  Serial.println("[LCD] no backpack found in 0x20-0x27 or 0x38-0x3F -- writes skipped");
  lcdPresent = false;
  lcdAddress = 0;
}

void lcdShow(String line1, String line2 = "") {
  if (!lcdPresent) return;
  if (line1 == lastLcdLine1 && line2 == lastLcdLine2) return;
  lcd->clear();
  lcd->setCursor(0, 0); lcd->print(line1.substring(0, 16));
  lcd->setCursor(0, 1); lcd->print(line2.substring(0, 16));
  lastLcdLine1 = line1; lastLcdLine2 = line2;
}

void lcdHardwareTest() {
  Serial.println("[LCD TEST] Rescanning SDA=GPIO21 SCL=GPIO22...");
  lcdProbe();
  if (lcdPresent) {
    lcdShow("LCD TEST OK", "Adjust contrast");
    Serial.printf("[LCD TEST] PASS: response at 0x%02X; test text written\n", lcdAddress);
    Serial.println("[LCD TEST] Backlight but no letters = turn the contrast potentiometer slowly");
  } else {
    Serial.println("[LCD TEST] FAIL: no I2C response");
    Serial.println("[LCD TEST] Check 5V, common GND, SDA->GPIO21, SCL->GPIO22 and level shifter");
  }
}


/* ============================================================================
   13. BUZZER  (section 12 -- non-blocking pattern player)
   ----------------------------------------------------------------------------
   Patterns run their timing regardless of BUZZER_ENABLED, so the monitor and
   Serial log stay meaningful with the hardware muted; only the physical
   digitalWrite is gated.
   ============================================================================ */

struct BuzzPattern {
  const int* onMs;
  const int* offMs;
  int steps;
  bool repeat;
};

const int P_SINGLE_ON[]  = {80};              const int P_SINGLE_OFF[]  = {0};
const int P_TWO_ON[]     = {80, 80};          const int P_TWO_OFF[]     = {100, 0};
const int P_ARRIVAL_ON[] = {120, 120};        const int P_ARRIVAL_OFF[] = {100, 0};
const int P_WARN_ON[]    = {150, 150, 150};   const int P_WARN_OFF[]    = {120, 120, 0};
const int P_LONG_ON[]    = {800};             const int P_LONG_OFF[]    = {0};

int buzzStep = -1; unsigned long tBuzzStep = 0; bool buzzOn = false;
const int* curOnMs = nullptr; const int* curOffMs = nullptr; int curSteps = 0;

void buzzPlay(const int* onMs, const int* offMs, int steps) {
  curOnMs = onMs; curOffMs = offMs; curSteps = steps;
  buzzStep = 0; tBuzzStep = millis(); buzzOn = true;
  if (BUZZER_ENABLED) digitalWrite(BUZZER, HIGH);
}

void buzzService() {
  if (buzzStep < 0 || buzzStep >= curSteps) return;
  unsigned long now = millis();
  unsigned long due = buzzOn ? curOnMs[buzzStep] : curOffMs[buzzStep];
  if (now - tBuzzStep < due) return;

  tBuzzStep = now;
  if (buzzOn) {
    if (BUZZER_ENABLED) digitalWrite(BUZZER, LOW);
    buzzOn = false;
    if (curOffMs[buzzStep] == 0) {
      buzzStep++;
      if (buzzStep < curSteps) {
        buzzOn = true;
        if (BUZZER_ENABLED) digitalWrite(BUZZER, HIGH);
      } else {
        buzzStep = -1;
      }
    }
  } else {
    buzzStep++;
    if (buzzStep < curSteps) {
      buzzOn = true;
      if (BUZZER_ENABLED) digitalWrite(BUZZER, HIGH);
    } else {
      buzzStep = -1;
    }
  }
}

void beepSingle()  { buzzPlay(P_SINGLE_ON, P_SINGLE_OFF, 1); }
void beepTwo()     { buzzPlay(P_TWO_ON, P_TWO_OFF, 2); }
void beepArrival() { buzzPlay(P_ARRIVAL_ON, P_ARRIVAL_OFF, 2); }
void beepWarn()    { buzzPlay(P_WARN_ON, P_WARN_OFF, 3); }
void beepLong()    { buzzPlay(P_LONG_ON, P_LONG_OFF, 1); }

void buzzerHardwareTest() {
  Serial.println("[BUZZER TEST] Phase 1/2: steady HIGH for active buzzer/module (700 ms)");
  noTone(BUZZER);
  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, HIGH);
  delay(700);
  digitalWrite(BUZZER, LOW);
  delay(400);

  Serial.println("[BUZZER TEST] Phase 2/2: 2000 Hz tone for passive buzzer/piezo (1000 ms)");
  tone(BUZZER, 2000);
  delay(1000);
  noTone(BUZZER);
  digitalWrite(BUZZER, LOW);
  Serial.println("[BUZZER TEST] Finished. Report: phase 1, phase 2, or neither");
}


/* ============================================================================
   14. RADIO SEND HELPERS
   ============================================================================ */

bool espnowSend(const RadioPacket &pkt) {
  lastTxPacketLength = (int)RADIO_PACKET_BYTES;
  if (!espNowReady || !espNowPeerOk) {
    txQueueErrors++;
    Serial.printf("[ESP-NOW TX] refused: NOW=%s PEER=%s bytes=%u\n",
                  espNowReady ? "OK" : "FAIL", espNowPeerOk ? "OK" : "FAIL",
                  (unsigned)RADIO_PACKET_BYTES);
    return false;
  }

  esp_err_t result = esp_now_send(BROADCAST_MAC,
                                  reinterpret_cast<const uint8_t*>(&pkt),
                                  RADIO_PACKET_BYTES);
  if (result == ESP_OK) {
    txPacketsQueued++;
    return true;
  }
  txQueueErrors++;
  Serial.printf("[ESP-NOW TX] queue error=%d bytes=%u\n",
                (int)result, (unsigned)RADIO_PACKET_BYTES);
  return false;
}

uint32_t newRequestId() {
  // Same generation approach the dispenser itself uses for local test IDs
  // (esp_random()), so both boards share one philosophy and a specific
  // reserved-bit convention was never needed.
  uint32_t id = esp_random();
  return id ? id : 1;
}

void sendDropRequest(uint32_t id) {
  RadioPacket pkt = {};
  pkt.magic = RADIO_MAGIC; pkt.version = RADIO_VERSION;
  pkt.type = MSG_DROP_REQUEST;
  pkt.requestId = id;
  bool queued = espnowSend(pkt);
  Serial.printf("[radio] DROP_REQUEST id=%lu try=%d/%d bytes=%u proto=%u queued=%s\n",
                (unsigned long)id, ackRetries + 1, ACK_MAX_RETRIES,
                (unsigned)RADIO_PACKET_BYTES, RADIO_VERSION,
                queued ? "yes" : "NO");
}


/* ============================================================================
   15. RADIO RECEIVE  (mirrors the dispenser's own callback -> flag pattern)
   ============================================================================ */

void onDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  // Count and log this BEFORE any filtering. If rawPacketsSeen never moves
  // at all, nothing is reaching the radio -- a different problem entirely
  // from "arrives but gets rejected by the length/magic/version check".
  portENTER_CRITICAL(&radioMux);
  rawPacketsSeen++;
  portEXIT_CRITICAL(&radioMux);
  if (info && info->src_addr) {
    snprintf(lastRawSenderMac, sizeof(lastRawSenderMac), "%02X:%02X:%02X:%02X:%02X:%02X",
             info->src_addr[0], info->src_addr[1], info->src_addr[2],
             info->src_addr[3], info->src_addr[4], info->src_addr[5]);
  }
  lastRawPacketLength = len;
  Serial.printf("[esp-now RAW] len=%d expected=%u from=%s\n",
                len, (unsigned)RADIO_PACKET_BYTES, lastRawSenderMac);

  if (len != (int)RADIO_PACKET_BYTES) {
    rawPacketsWrongLength++;
    Serial.print("  -> dropped: wrong length; first bytes=");
    int dumpLength = min(len, 24);
    for (int i = 0; i < dumpLength; i++) Serial.printf("%02X", data[i]);
    if (len > dumpLength) Serial.print("...");
    Serial.println();
    return;
  }
  RadioPacket pkt;
  memcpy(&pkt, data, RADIO_PACKET_BYTES);
  if (pkt.magic != RADIO_MAGIC || pkt.version != RADIO_VERSION) {
    rawPacketsWrongProtocol++;
    Serial.printf("  -> dropped: magic=0x%08lX version=%d (expected 0x%08lX / %d)\n",
                  (unsigned long)pkt.magic, pkt.version, (unsigned long)RADIO_MAGIC, RADIO_VERSION);
    return;
  }
  portENTER_CRITICAL(&radioMux);
  rawPacketsValid++;
  portEXIT_CRITICAL(&radioMux);

  portENTER_CRITICAL(&radioMux);
  if (pkt.type == MSG_DISPENSER_HEARTBEAT) {
    tLastDispenserHeartbeat = millis();
    dispenserLastState = pkt.deviceState;
    dispenserLastCompletedId = pkt.requestId;   // heartbeat reuses this field
    if (activeRequestId != 0 && pkt.requestId == activeRequestId) {
      doneReceivedFlag = true;   // catches a lost DONE packet
    }
  } else if (pkt.type == MSG_DROP_ACK) {
    if (activeRequestId != 0 && pkt.requestId == activeRequestId) {
      ackReceivedFlag = true;
      lastAckResult = pkt.resultCode;
    }
  } else if (pkt.type == MSG_DROP_DONE) {
    if (activeRequestId != 0 && pkt.requestId == activeRequestId) {
      doneReceivedFlag = true;
    }
  }
  portEXIT_CRITICAL(&radioMux);
}

// Core 3.3.x signature: wifi_tx_info_t*, not a raw MAC pointer. Using the
// old signature here is a compile error on this core version.
void onDataSent(const wifi_tx_info_t *txInfo, esp_now_send_status_t status) {
  (void)txInfo;
  if (status == ESP_NOW_SEND_SUCCESS) txDelivered++;
  else txDeliveryFailed++;
}


/* ============================================================================
   16. STATE NAME / LCD-BUZZER TABLE HELPERS
   ============================================================================ */

const char* stateName(MissionState s) {
  switch (s) {
    case M_BOOT:            return "BOOT";
    case M_WAIT_WIFI:       return "WAIT_WIFI";
    case M_WAIT_DISPENSER:  return "WAIT_DISPENSER";
    case M_REPOSITION:      return "REPOSITION_CAR";
    case M_AT_DISPENSER:    return "AT_DISPENSER";
    case M_WAIT_DROP_ACK:   return "WAIT_DROP_ACK";
    case M_WAIT_GATE_DONE:  return "WAIT_GATE_DONE";
    case M_WAIT_LID:        return "WAIT_LID";
    case M_LOAD_COUNTDOWN:  return "LOAD_COUNTDOWN";
    case M_DEPART_STATION:  return "DEPART_STATION";
    case M_OUTBOUND:        return "OUTBOUND";
    case M_AT_BED:           return "AT_BED";
    case M_BED_TURN:         return "BED_TURN";
    case M_DEPART_BED:       return "DEPART_BED";
    case M_RETURNING:        return "RETURNING";
    case M_AT_STATION:       return "AT_STATION";
    case M_STATION_TURN:     return "STATION_TURN";
    case M_OBSTACLE_PAUSE:   return "OBSTACLE_PAUSE";
    case M_FAULT:            return "FAULT";
    case M_EMERGENCY_STOP:   return "EMERGENCY_STOP";
  }
  return "?";
}

void enterState(MissionState next) {
  Serial.printf("[state] %s -> %s\n", stateName(state), stateName(next));
  state = next;
  tState = millis();
}

void enterFault(const char* reason) {
  strncpy(faultReason, reason, sizeof(faultReason) - 1);
  faultReason[sizeof(faultReason) - 1] = '\0';
  motorsStop();
  enterState(M_FAULT);
  Serial.printf("*** FAULT: %s ***\n", faultReason);
}


/* ============================================================================
   17. WEB API  (section 14)
   ============================================================================ */

void handleStatus() {
  bool b[5]; int black = readIR(b);
  String j = "{";
  j += "\"state\":\"" + String(stateName(state)) + "\",";
  j += "\"fault\":\"" + String(faultReason) + "\",";
  j += "\"build\":\"" + String(FIRMWARE_BUILD) + "\",";
  j += "\"protocolVersion\":" + String(RADIO_VERSION) + ",";
  j += "\"packetBytes\":" + String((unsigned)RADIO_PACKET_BYTES) + ",";
  j += "\"dispenserOnline\":" + String(dispenserOnline ? "true" : "false") + ",";
  j += "\"espNowReady\":" + String(espNowReady ? "true" : "false") + ",";
  j += "\"espNowPeerOk\":" + String(espNowPeerOk ? "true" : "false") + ",";
  j += "\"espNowRecvCallbackOk\":" + String(espNowRecvCallbackOk ? "true" : "false") + ",";
  j += "\"espNowSendCallbackOk\":" + String(espNowSendCallbackOk ? "true" : "false") + ",";
  j += "\"dispenserState\":" + String(dispenserLastState) + ",";
  j += "\"rxLastBytes\":" + String(lastRawPacketLength) + ",";
  j += "\"rxRaw\":" + String(rawPacketsSeen) + ",";
  j += "\"rxValid\":" + String(rawPacketsValid) + ",";
  j += "\"rxWrongLength\":" + String(rawPacketsWrongLength) + ",";
  j += "\"rxWrongProtocol\":" + String(rawPacketsWrongProtocol) + ",";
  j += "\"txLastBytes\":" + String(lastTxPacketLength) + ",";
  j += "\"txQueued\":" + String(txPacketsQueued) + ",";
  j += "\"txDelivered\":" + String(txDelivered) + ",";
  j += "\"txFailed\":" + String(txQueueErrors + txDeliveryFailed) + ",";
  j += "\"lidFitted\":" + String(LID_SWITCH_FITTED ? "true" : "false") + ",";
  j += "\"returning\":" + String(returning ? "true" : "false") + ",";
  j += "\"distanceCm\":" + String(lastDistanceCm) + ",";
  j += "\"motorL\":" + String(lastLeftPWM) + ",";
  j += "\"motorR\":" + String(lastRightPWM) + ",";
  j += "\"ir\":\"" + String(b[0])+String(b[1])+String(b[2])+String(b[3])+String(b[4]) + "\",";
  j += "\"black\":" + String(black) + ",";
  j += "\"ip\":\"" + WiFi.localIP().toString() + "\"";
  j += "}";
  server.send(200, "application/json", j);
}

void handleStart() {
  if (state == M_REPOSITION) {
    returning = false;
    enterState(M_AT_DISPENSER);
    server.send(200, "text/plain", "ok, station placement confirmed");
  } else if (state == M_AT_DISPENSER) {
    if (!dispenserOnline) { server.send(409, "text/plain", "dispenser offline"); return; }
    if (LID_SWITCH_FITTED && lidSwitchClosed()) {
      server.send(409, "text/plain", "open the manual lid before requesting a drop");
      return;
    }
    activeRequestId = newRequestId();
    ackRetries = 0; ackReceivedFlag = false; doneReceivedFlag = false;
    tLastAckAttempt = 0;
    enterState(M_WAIT_DROP_ACK);
    server.send(200, "text/plain", "ok");
  } else if (state == M_WAIT_LID && !LID_SWITCH_FITTED) {
    // second "press" in fallback mode == lid confirm
    enterState(M_LOAD_COUNTDOWN);
    server.send(200, "text/plain", "ok, lid confirmed");
  } else {
    server.send(409, "text/plain", "not valid right now");
  }
}

void handleLidConfirm() {
  if (state == M_WAIT_LID && !LID_SWITCH_FITTED) {
    enterState(M_LOAD_COUNTDOWN);
    server.send(200, "text/plain", "ok");
  } else {
    server.send(409, "text/plain", "not waiting on lid confirm");
  }
}

void handleAbort() {
  motorsStop();
  enterState(M_EMERGENCY_STOP);
  server.send(200, "text/plain", "ok");
}

void handleFaultReset() {
  if (state == M_FAULT || state == M_EMERGENCY_STOP) {
    strncpy(faultReason, "NONE", sizeof(faultReason));
    returning = false;
    enterState(M_AT_DISPENSER);
    server.send(200, "text/plain", "ok");
  } else {
    server.send(400, "text/plain", "no fault active");
  }
}

void handleDispenserTest() {
  // Deliberately permissive: this is a bench/setup diagnostic whose whole
  // point is testing the radio link on its own, so it works from anywhere
  // except an already-active drop/travel sequence (where firing a second,
  // untracked request would confuse the real mission's bookkeeping).
  bool midMission = (state == M_WAIT_DROP_ACK || state == M_WAIT_GATE_DONE ||
                     state == M_WAIT_LID || state == M_LOAD_COUNTDOWN ||
                     state == M_DEPART_STATION || state == M_OUTBOUND ||
                     state == M_AT_BED || state == M_BED_TURN ||
                     state == M_DEPART_BED || state == M_RETURNING ||
                     state == M_AT_STATION || state == M_STATION_TURN ||
                     state == M_OBSTACLE_PAUSE);
  if (midMission) { server.send(409, "text/plain", "refused: a mission is already running"); return; }
  if (!dispenserOnline) { server.send(409, "text/plain", "dispenser offline -- no heartbeat seen recently"); return; }
  uint32_t id = newRequestId();
  RadioPacket pkt = {};
  pkt.magic = RADIO_MAGIC; pkt.version = RADIO_VERSION;
  pkt.type = MSG_DROP_REQUEST; pkt.requestId = id;
  espnowSend(pkt);
  server.send(200, "text/plain", "test request sent, id " + String(id));
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Hospital car</title>
<style>body{font:16px sans-serif;max-width:480px;margin:20px auto;padding:0 16px}
button{display:block;width:100%;margin:8px 0;padding:14px;font-size:17px;border-radius:8px;border:1px solid #ccc}
.start{background:#1769aa;color:#fff}.stop{background:#b3261e;color:#fff}
pre{white-space:pre-wrap;background:#f4f4f4;padding:10px;border-radius:6px;font-size:13px}</style>
<h2>Hospital car</h2>
<pre id="s">loading...</pre>
<div id="msg" style="min-height:24px;font-weight:bold"></div>
<button class="start" onclick="cmd('/start')">START</button>
<button onclick="cmd('/lid/confirm')">CONFIRM LID CLOSED</button>
<button class="stop" onclick="cmd('/abort')">EMERGENCY STOP</button>
<button onclick="cmd('/fault/reset')">RESET FAULT</button>
<button onclick="if(confirm('Run a dispenser test drop?'))cmd('/dispenser/test')">Dispenser test (setup only)</button>
<script>
async function cmd(p){
  let r=await fetch(p,{method:'POST'});
  let t=await r.text();
  let m=document.getElementById('msg');
  m.style.color = r.ok ? '#0f7a4f' : '#b3261e';
  m.textContent = (r.ok ? 'OK: ' : 'REJECTED ('+r.status+'): ') + t;
}
async function poll(){try{document.getElementById('s').textContent=
  JSON.stringify(JSON.parse(await(await fetch('/status')).text()),null,1);}catch(e){}setTimeout(poll,500);}
poll();
</script>
)HTML";

void handleRoot() { server.send_P(200, "text/html", PAGE); }

void setupServer() {
  server.on("/",               handleRoot);
  server.on("/status",         handleStatus);
  server.on("/start",          HTTP_POST, handleStart);
  server.on("/lid/confirm",    HTTP_POST, handleLidConfirm);
  server.on("/abort",          HTTP_POST, handleAbort);
  server.on("/fault/reset",    HTTP_POST, handleFaultReset);
  server.on("/dispenser/test", HTTP_POST, handleDispenserTest);
  server.begin();
}


/* ============================================================================
   18. MARKER DETECTION  (section 10 -- stop first, then confirm stationary)
   ============================================================================ */

// Returns true exactly once, the instant a marker is confirmed.
bool serviceMarkerDetection(int black) {
  unsigned long now = millis();

  if (!markerArmed) {
    if (black < 5) {
      if (tNormalLineSince == 0) tNormalLineSince = now;
      if (now - tNormalLineSince >= MARKER_REARM_MS) markerArmed = true;
    } else {
      tNormalLineSince = 0;
    }
    return false;
  }

  if (black == 5) {
    if (!markerCandidate) {
      // See it for the first time: brake NOW, then confirm stationary. Merely
      // setting EN=0 made this chassis coast too far beyond the bed marker.
      motorsBrake();
      markerCandidate = true;
      tMarkerStopCandidate = now;
      return false;
    }
    if (now - tMarkerStopCandidate >= MARKER_STOP_DEBOUNCE_MS) {
      markerCandidate = false;
      markerArmed = false;   // disarm until MARKER_REARM_MS of normal line
      tNormalLineSince = 0;
      return true;
    }
    return false;
  }

  // Was a candidate but black dropped before confirming -- false trigger.
  markerCandidate = false;
  return false;
}


/* ============================================================================
   19. LINE FOLLOWING  (section 10, with two-phase lost-line recovery)
   ============================================================================ */

void serviceLineFollow(int black, bool b[5]) {
  if (black > 0 && black < 5) {
    float pos = linePosition(b);
    int corr = (int)(pos * (CORRECTION_LIMIT * 0.5f));
    if (corr != 0) lineLostDirection = (corr > 0) ? 1 : -1;
    int basePwm = obstacleCaution ? CREEP_PWM : CRUISE_PWM;
    drive(basePwm + corr, basePwm - corr);
    tLineLost = 0;
    return;
  }

  if (black == 5) {
    // Marker territory -- serviceMarkerDetection() owns motor control here;
    // keep creeping only if we're in the post-turn/post-stop creep phase
    // (handled by the DEPART_* states, not here).
    return;
  }

  // black == 0: line lost.
  if (tLineLost == 0) tLineLost = millis();
  unsigned long lost = millis() - tLineLost;

  if (lost < LINE_LOST_ARC_MS) {
    int corr = lineLostDirection * (CORRECTION_LIMIT / 2);
    drive(CRUISE_PWM + corr, CRUISE_PWM - corr);
  } else if (lost < LINE_LOST_FAULT_MS) {
    drive(lineLostDirection * CREEP_PWM, -lineLostDirection * CREEP_PWM);
  } else {
    enterFault("LINE LOST");
  }
}


/* ============================================================================
   20. OBSTACLE HANDLING  (section 11)
   ============================================================================ */

// Returns true if the car should be fully stopped right now.
bool serviceObstacle() {
  unsigned long now = millis();
  if (now - tLastSonarSample < SONAR_SAMPLE_MS) return obstacleActive;
  tLastSonarSample = now;

  long d = sonarUpdate();
  obstacleCaution = d <= OBSTACLE_CAUTION_CM;

  if (d <= OBSTACLE_STOP_CM) {
    closeReadingStreak++;
    if (closeReadingStreak >= 2) {
      if (!obstacleActive) {
        obstacleActive = true;
        stateBeforeObstacle = state;
        motorsStop();
        lcdShow("OBSTACLE!", "Distance:" + String(d) + "cm");
        beepWarn();
      }
      tObstacleClearSince = 0;
      return true;
    }
  } else {
    closeReadingStreak = 0;
  }

  if (obstacleActive) {
    if (d >= OBSTACLE_RESUME_CM) {
      if (tObstacleClearSince == 0) tObstacleClearSince = now;
      if (now - tObstacleClearSince >= OBSTACLE_RESUME_STABLE_MS) {
        obstacleActive = false;
        obstacleCaution = d <= OBSTACLE_CAUTION_CM;
        return false;
      }
    } else {
      tObstacleClearSince = 0;
    }
    return true;   // still paused, not yet stably clear
  }

  return false;
}


/* ============================================================================
   21. SPIN  (BED_TURN / STATION_TURN -- non-blocking, shared logic)
   ============================================================================ */

bool serviceSpin() {
  unsigned long now = millis();
  switch (spinPhase) {
    case SP_BLIND:
      drive(SPIN_PWM, -SPIN_PWM);
      if (now - tSpinPhase >= SPIN_180_MS) { spinPhase = SP_SLOW; tSpinPhase = now; }
      break;
    case SP_SLOW: {
      bool b[5]; readIR(b);
      drive(SPIN_PWM / 2, -(SPIN_PWM / 2));
      if (b[2]) { motorsStop(); spinPhase = SP_DONE; }
      if (now - tSpinPhase > 4000) { spinPhase = SP_DONE; return true; }   // safety
      break;
    }
    case SP_DONE:
      return true;
  }
  return false;
}


/* ============================================================================
   22. BUTTON / START-AS-CONFIRM
   ============================================================================ */

void pollStartButton() {
  int reading = digitalRead(START_BUTTON);
  if (reading != lastBtnReading) {
    tBtnChange = millis();
    lastBtnReading = reading;
    if (reading == HIGH) startPressHandled = false;
  }
  if (reading == LOW && !startPressHandled &&
      millis() - tBtnChange >= BTN_DEBOUNCE_MS) {
    startPressHandled = true;
    if (state == M_AT_DISPENSER && dispenserOnline) {
      if (LID_SWITCH_FITTED && lidSwitchClosed()) {
        Serial.println("[START] Refused: open the manual lid before requesting a drop");
        return;
      }
      activeRequestId = newRequestId();
      ackRetries = 0; ackReceivedFlag = false; doneReceivedFlag = false;
      tLastAckAttempt = 0;
      enterState(M_WAIT_DROP_ACK);
    } else if (state == M_WAIT_LID && !LID_SWITCH_FITTED) {
      enterState(M_LOAD_COUNTDOWN);
    } else if (state == M_REPOSITION) {
      returning = false;
      enterState(M_AT_DISPENSER);
    } else if (state == M_FAULT || state == M_EMERGENCY_STOP) {
      strncpy(faultReason, "NONE", sizeof(faultReason));
      returning = false;
      enterState(M_AT_DISPENSER);
    }
  }
}

bool lidSwitchClosed() {
  if (!LID_SWITCH_FITTED) return false;   // never consulted when false
  return digitalRead(LID_SWITCH) == LOW;
}


/* ============================================================================
   23. SERIAL CONSOLE  (section 17)
   ============================================================================ */

unsigned long tIrDebug = 0;
const unsigned long IR_DEBUG_INTERVAL_MS = 200;

void printHelp() {
  Serial.println();
  Serial.println("---- Car serial console ----");
  Serial.println(" h help   p full report   i toggle monitor   w wifi details");
  Serial.println(" +/- monitor speed");
  Serial.println(" s  = START (same as the web/physical button)");
  Serial.println(" k  = lid confirm (fallback mode)");
  Serial.println(" a  = emergency stop     r = fault reset");
  Serial.println(" m  = both motors 2s fwd (WHEELS OFF GROUND)");
  Serial.println(" 1  = left motor only     2 = right motor only");
  Serial.println(" t  = one spin test (180 deg, for SPIN_180_MS calibration)");
  Serial.println(" b  = buzzer test: active HIGH, then passive 2kHz tone");
  Serial.println(" l  = rescan LCD and force an LCD TEST OK screen");
  Serial.println("-----------------------------");
}

void printFullReport() {
  bool b[5]; int black = readIR(b);
  Serial.println();
  Serial.println("================ CAR FULL COMPONENT REPORT ================");
  Serial.printf("firmware        %s\n", FIRMWARE_BUILD);
  Serial.printf("protocol        magic=0x%08lX version=%u packet=%u bytes (compile checked)\n",
                (unsigned long)RADIO_MAGIC, RADIO_VERSION,
                (unsigned)RADIO_PACKET_BYTES);
  Serial.printf("state           %s   since=%lums   fault=%s\n",
                stateName(state), millis() - tState, faultReason);
  Serial.printf("wifi            %s ssid=%s ip=%s ch=%d rssi=%d\n",
                WiFi.status()==WL_CONNECTED?"up":"down", WiFi.SSID().c_str(),
                WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI());
  Serial.printf("esp-now RX      init=%s peer=%s recvCb=%s sendCb=%s lastBytes=%d sender=%s\n",
                espNowReady?"OK":"FAIL", espNowPeerOk?"OK":"FAIL",
                espNowRecvCallbackOk?"OK":"FAIL", espNowSendCallbackOk?"OK":"FAIL",
                lastRawPacketLength, lastRawSenderMac);
  Serial.printf("                raw=%lu valid=%lu wrongLength=%lu wrongProtocol=%lu\n",
                (unsigned long)rawPacketsSeen, (unsigned long)rawPacketsValid,
                (unsigned long)rawPacketsWrongLength,
                (unsigned long)rawPacketsWrongProtocol);
  Serial.printf("esp-now TX      lastBytes=%d queued=%lu queueErrors=%lu delivered=%lu deliveryFail=%lu\n",
                lastTxPacketLength, (unsigned long)txPacketsQueued,
                (unsigned long)txQueueErrors, (unsigned long)txDelivered,
                (unsigned long)txDeliveryFailed);
  Serial.printf("dispenser       online=%s state=%u lastCompleted=%lu\n",
                dispenserOnline?"yes":"no", dispenserLastState, (unsigned long)dispenserLastCompletedId);
  Serial.printf("ir raw levels   %d%d%d%d%d (HIGH=black, LOW=white)\n",
                digitalRead(IR1), digitalRead(IR2), digitalRead(IR3),
                digitalRead(IR4), digitalRead(IR5));
  Serial.printf("ir normalized   %d%d%d%d%d black=%d markerArmed=%d\n",
                b[0],b[1],b[2],b[3],b[4], black, markerArmed);
  Serial.printf("sonar           raw=%ldcm acted=%ldcm timeouts=%lu sticky=%d caution=%d stop=%d\n",
                lastSonarRawCm, lastDistanceCm, sonarTimeoutCount,
                sonarStickyTimeouts, obstacleCaution, obstacleActive);
  Serial.printf("motors          PWMattach=%s L=%d R=%d ENA=GPIO%d ENB=GPIO%d\n",
                motorPwmReady ? "OK" : "FAIL", lastLeftPWM, lastRightPWM, ENA, ENB);
  Serial.printf("glovebox/lid    MANUAL; servo intentionally absent; switchFitted=%d closed=%d\n",
                LID_SWITCH_FITTED, lidSwitchClosed());
  Serial.printf("start button    %s\n", digitalRead(START_BUTTON)==LOW?"DOWN":"up");
  Serial.printf("buzzer          configured=%s requestedOutput=%s patternStep=%d GPIO%d\n",
                BUZZER_ENABLED ? "ENABLED" : "MUTED",
                buzzOn ? "ON" : "off", buzzStep, BUZZER);
  Serial.printf("lcd             %s address=0x%02X last='%s' / '%s'\n",
                lcdPresent?"present":"absent", lcdAddress,
                lastLcdLine1.c_str(), lastLcdLine2.c_str());
  Serial.println("nvs             n/a (car stores no persistent state in V2)");
  Serial.println("===========================================================");
}

void motorTest(int which) {
  Serial.println("MOTOR TEST: wheels OFF the ground. Starting in 2s...");
  delay(2000);
  if (which == 0) drive(150, 150);
  else if (which == 1) drive(150, 0);
  else drive(0, 150);
  delay(2000);
  motorsStop();
  Serial.println("Stopped.");
}

void spinTest() {
  Serial.println("SPIN TEST: 180 deg blind spin, wheels on the floor.");
  unsigned long t0 = millis();
  drive(SPIN_PWM, -SPIN_PWM);
  while (millis() - t0 < SPIN_180_MS) delay(10);
  motorsStop();
  Serial.printf("Done. Measure the actual angle turned; adjust SPIN_180_MS=%lu accordingly.\n", SPIN_180_MS);
}

void handleSerialConsole() {
  if (Serial.available()) {
    char c = Serial.read();
    switch (c) {
      case 'h': case 'H': printHelp(); break;
      case 'p': case 'P': printFullReport(); break;
      case 'i': case 'I': monitorOn = !monitorOn; Serial.println(monitorOn?"[monitor] on":"[monitor] off"); break;
      case 'w': case 'W': Serial.printf("[wifi] ip=%s ch=%d rssi=%d\n", WiFi.localIP().toString().c_str(), WiFi.channel(), WiFi.RSSI()); break;
      case '+': monitorIntervalMs = min(2000UL, monitorIntervalMs + 100); break;
      case '-': monitorIntervalMs = max(100UL, monitorIntervalMs - 100); break;
      case 's': case 'S':
        if (state == M_REPOSITION) {
          returning = false; enterState(M_AT_DISPENSER);
        } else if (state == M_AT_DISPENSER && dispenserOnline) {
          if (LID_SWITCH_FITTED && lidSwitchClosed()) {
            Serial.println("[s] refused: open the manual lid before requesting a drop");
            break;
          }
          activeRequestId = newRequestId(); ackRetries = 0; ackReceivedFlag = false; doneReceivedFlag = false;
          tLastAckAttempt = 0; enterState(M_WAIT_DROP_ACK);
        } else if (state == M_WAIT_LID && !LID_SWITCH_FITTED) {
          enterState(M_LOAD_COUNTDOWN);
        } else Serial.println("[s] not valid right now");
        break;
      case 'k': case 'K':
        if (state == M_WAIT_LID) enterState(M_LOAD_COUNTDOWN);
        else Serial.println("[k] not waiting on lid confirm");
        break;
      case 'a': case 'A': motorsStop(); enterState(M_EMERGENCY_STOP); break;
      case 'r': case 'R':
        if (state == M_FAULT || state == M_EMERGENCY_STOP) {
          strncpy(faultReason, "NONE", sizeof(faultReason)); returning = false; enterState(M_AT_DISPENSER);
        } else Serial.println("[r] nothing to reset");
        break;
      case 'm': case 'M': motorTest(0); break;
      case '1': motorTest(1); break;
      case '2': motorTest(2); break;
      case 't': case 'T': spinTest(); break;
      case 'b': case 'B':
        if (lastLeftPWM != 0 || lastRightPWM != 0) {
          Serial.println("[BUZZER TEST] Refused while motors are moving");
        } else {
          buzzerHardwareTest();
        }
        break;
      case 'l': case 'L':
        if (lastLeftPWM != 0 || lastRightPWM != 0) {
          Serial.println("[LCD TEST] Refused while motors are moving");
        } else {
          lcdHardwareTest();
        }
        break;
      case '\n': case '\r': break;
      default: Serial.println("Unknown key. h for help.");
    }
  }
  if (millis() - tIrDebug >= IR_DEBUG_INTERVAL_MS) {
    tIrDebug = millis();
    bool b[5]; int black = readIR(b);
    Serial.printf("IR:%d%d%d%d%d black=%d dist=%ldcm state=%s L=%d R=%d\n",
                  b[0],b[1],b[2],b[3],b[4], black, lastDistanceCm, stateName(state), lastLeftPWM, lastRightPWM);
  }
}


/* ============================================================================
   24. MONITOR  (section 17)
   ============================================================================ */

void printMonitorHeader() {
  Serial.println();
  Serial.println("[MON CAR] self-describing live row: every fitted sensor, output, link and protocol counter");
}

void printMonitorRow() {
  if (monitorRowsSinceHeader == 0) printMonitorHeader();
  monitorRowsSinceHeader = (monitorRowsSinceHeader + 1) % 20;
  bool b[5]; int black = readIR(b);
  unsigned long heartbeatAge = tLastDispenserHeartbeat == 0
                                 ? 999999UL
                                 : (millis() - tLastDispenserHeartbeat) / 1000;
  const char *obstacleState = obstacleActive ? "STOP" : (obstacleCaution ? "SLOW" : "clear");
  const char *lidState = !LID_SWITCH_FITTED ? "manual" : (lidSwitchClosed() ? "SHUT" : "OPEN");
  const char *buzzerState = !BUZZER_ENABLED ? "MUTED" : (buzzOn ? "ON" : "off");

  Serial.printf(
    "[MON CAR] t=%lus state=%s fault=%s wifi=%s rssi=%d ch=%d "
    "now=%s peer=%s cb=%s/%s rxB=%d raw=%lu valid=%lu badLen=%lu badProto=%lu "
    "txB=%d queued=%lu delivered=%lu txFail=%lu disp=%s dispState=%u hbAge=%lus "
    "IRraw=%d%d%d%d%d IRblack=%d%d%d%d%d black=%d armed=%s "
    "sonarRaw=%ldcm sonar=%ldcm timeouts=%lu obs=%s "
    "motorPWM=%s L=%d R=%d lid=%s button=%s buzzer=%s reqBuzz=%s step=%d "
    "lcd=%s@0x%02X nvs=n/a req=%lu\n",
    millis()/1000, stateName(state), faultReason,
    WiFi.status()==WL_CONNECTED?"up":"down", WiFi.status()==WL_CONNECTED?WiFi.RSSI():0, WiFi.channel(),
    espNowReady?"OK":"FAIL", espNowPeerOk?"OK":"FAIL",
    espNowRecvCallbackOk?"R":"-", espNowSendCallbackOk?"S":"-",
    lastRawPacketLength, (unsigned long)rawPacketsSeen, (unsigned long)rawPacketsValid,
    (unsigned long)rawPacketsWrongLength, (unsigned long)rawPacketsWrongProtocol,
    lastTxPacketLength, (unsigned long)txPacketsQueued, (unsigned long)txDelivered,
    (unsigned long)(txQueueErrors + txDeliveryFailed),
    dispenserOnline?"up":"down", dispenserLastState, heartbeatAge,
    digitalRead(IR1), digitalRead(IR2), digitalRead(IR3), digitalRead(IR4), digitalRead(IR5),
    b[0],b[1],b[2],b[3],b[4], black, markerArmed?"yes":"no",
    lastSonarRawCm, lastDistanceCm, sonarTimeoutCount, obstacleState,
    motorPwmReady?"OK":"FAIL", lastLeftPWM, lastRightPWM, lidState,
    digitalRead(START_BUTTON)==LOW?"DOWN":"up", buzzerState,
    buzzOn?"ON":"off", buzzStep,
    lcdPresent?"yes":"NO", lcdAddress, (unsigned long)activeRequestId);
}


/* ============================================================================
   25. SETUP
   ============================================================================ */

void setup() {
  Serial.begin(115200);
  delay(300);

  Serial.println("\n\n====================================================");
  Serial.println(" SMART HOSPITAL ROBOT - CAR BOOT");
  Serial.println("====================================================");
  Serial.printf("[PROTO] build=%s magic=0x%08lX version=%u packet=%u bytes\n",
                FIRMWARE_BUILD, (unsigned long)RADIO_MAGIC, RADIO_VERSION,
                (unsigned)RADIO_PACKET_BYTES);
  Serial.println("[GLOVEBOX] MANUAL LID: no car servo is present in this V2 firmware");
  Serial.printf("[BUZZER] mission output is %s (set BUZZER_ENABLED=true only with the proper driver)\n",
                BUZZER_ENABLED ? "ENABLED" : "MUTED");

  esp_reset_reason_t rr = esp_reset_reason();
  bool suspectReset = (rr == ESP_RST_BROWNOUT || rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_PANIC);
  Serial.print("Boot reason: ");
  switch (rr) {
    case ESP_RST_POWERON:  Serial.println("power-on (normal)"); break;
    case ESP_RST_BROWNOUT: Serial.println("*** BROWNOUT -- the 5V rail sagged ***"); break;
    case ESP_RST_TASK_WDT: case ESP_RST_WDT: Serial.println("*** WATCHDOG RESET ***"); break;
    case ESP_RST_PANIC:    Serial.println("*** PANIC/CRASH ***"); break;
    default: Serial.printf("other (%d)\n", (int)rr); break;
  }
  if (suspectReset) Serial.println("Unexpected reset -- inspect before trusting the next run.");

  pinMode(IR1, INPUT); pinMode(IR2, INPUT); pinMode(IR3, INPUT); pinMode(IR4, INPUT); pinMode(IR5, INPUT);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  motorPwmReady = ledcAttach(ENA, PWM_FREQ, PWM_RES) && ledcAttach(ENB, PWM_FREQ, PWM_RES);
  motorsStop();
  Serial.printf("Motor ENA/ENB attach: %s\n", motorPwmReady ? "OK" : "FAIL");
  if (!motorPwmReady) { enterFault("PWM INIT FAILED"); }

  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT); digitalWrite(TRIG, LOW);
  pinMode(BUZZER, OUTPUT); digitalWrite(BUZZER, LOW);
  pinMode(START_BUTTON, INPUT_PULLUP);
  if (LID_SWITCH_FITTED) pinMode(LID_SWITCH, INPUT_PULLUP);

  lcdProbe();
  lcdShow("SYSTEM BOOT", "Checking parts");
  beepSingle();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  enterState(M_WAIT_WIFI);
  lcdShow("WIFI RETRY", "Hotspot: froggy");

  Serial.print("WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(300); Serial.print("."); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Car IP: "); Serial.println(WiFi.localIP());
    if (MDNS.begin("car")) Serial.println("mDNS up: http://car.local/");
  } else {
    Serial.println("WiFi FAILED -- ESP-NOW still works if the dispenser is on the same channel.");
  }

  espNowReady = (esp_now_init() == ESP_OK);
  if (!espNowReady) {
    Serial.println("*** esp_now_init FAILED -- car can NEVER hear the dispenser like this ***");
  } else {
    espNowRecvCallbackOk = (esp_now_register_recv_cb(onDataRecv) == ESP_OK);
    espNowSendCallbackOk = (esp_now_register_send_cb(onDataSent) == ESP_OK);
    if (!espNowRecvCallbackOk) Serial.println("*** receive callback registration FAILED ***");
    if (!espNowSendCallbackOk) Serial.println("*** send callback registration FAILED ***");
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, BROADCAST_MAC, 6);
    peer.channel = 0; peer.encrypt = false; peer.ifidx = WIFI_IF_STA;
    espNowPeerOk = (esp_now_add_peer(&peer) == ESP_OK);
    if (!espNowPeerOk) Serial.println("*** esp_now_add_peer FAILED -- car cannot SEND drop requests ***");
  }
  // Both flags now print on EVERY monitor row (see printMonitorRow) and in
  // /status and 'p', not just here -- this exact line has been scrolling
  // out of the Serial buffer during long debugging sessions.

  setupServer();

  if (state != M_FAULT) {
    enterState(M_WAIT_DISPENSER);
    lcdShow("DISP OFFLINE", "Retrying...");
  }

  printHelp();
  printFullReport();
  printMonitorHeader();
  Serial.println("Car ready.");
}


/* ============================================================================
   26. MAIN LOOP
   ============================================================================ */

void loop() {
  server.handleClient();
  handleSerialConsole();
  pollStartButton();
  buzzService();

  bool b[5]; int black = readIR(b);
  unsigned long now = millis();

  dispenserOnline = (now - tLastDispenserHeartbeat) < DISPENSER_ONLINE_TIMEOUT_MS && tLastDispenserHeartbeat != 0;

  if (LID_SWITCH_FITTED) {
    bool movingState = (state == M_DEPART_STATION || state == M_OUTBOUND ||
                        state == M_BED_TURN || state == M_DEPART_BED ||
                        state == M_RETURNING || state == M_STATION_TURN ||
                        state == M_OBSTACLE_PAUSE);
    if (state == M_LOAD_COUNTDOWN && !lidSwitchClosed()) {
      motorsStop();
      enterState(M_WAIT_LID);
      lcdShow("LID OPEN", "Close the lid");
    } else if (movingState && !lidSwitchClosed()) {
      enterFault("LID OPEN");
    }
  }

  // Obstacle monitoring is only meaningful during actual travel -- never
  // during a spin (section 11), never in the stationary/setup states.
  bool obstacleRelevant = (state == M_DEPART_STATION || state == M_OUTBOUND ||
                           state == M_DEPART_BED || state == M_RETURNING ||
                           state == M_OBSTACLE_PAUSE);
  bool paused = obstacleRelevant ? serviceObstacle() : false;

  if (paused && state != M_OBSTACLE_PAUSE) {
    tObstaclePauseStarted = now;
    enterState(M_OBSTACLE_PAUSE);
  } else if (!paused && state == M_OBSTACLE_PAUSE) {
    // "Pause the active movement timer while obstructed so an interrupted
    // state resumes with the correct time still to run" (section 11).
    // tState alone does not protect the timers that actually matter here --
    // shift each of them forward by exactly how long we were paused, so a
    // resumed DEPART_* creep-timeout or an in-progress line-lost recovery
    // does not have the paused duration silently counted against it.
    unsigned long pausedFor = now - tObstaclePauseStarted;
    tMarkerCreepStart += pausedFor;
    if (tLineLost != 0) tLineLost += pausedFor;
    enterState(stateBeforeObstacle);
  }

  switch (state) {

    case M_BOOT: case M_WAIT_WIFI:
      break;   // handled synchronously in setup()

    case M_WAIT_DISPENSER:
      if (dispenserOnline && dispenserLastState == DISP_STATE_READY) {
        enterState(M_REPOSITION);
        lcdShow("REPOSITION CAR", "Press START");
      } else {
        lcdShow("DISP OFFLINE", "Retrying...");
      }
      break;

    case M_REPOSITION:
      motorsStop();
      // Waits for the START button/console 'r' path (pollStartButton /
      // handleSerialConsole) -- see section 22/23. Every boot requires this
      // explicit confirm, since the car cannot know its own physical
      // position after a reset (section 16).
      break;

    case M_AT_DISPENSER:
      motorsStop();
      if (!dispenserOnline) {
        lcdShow("DISP OFFLINE", "Retrying...");
      } else {
        lcdShow("AT DISPENSER", "Press START");
      }
      break;

    case M_WAIT_DROP_ACK: {
      motorsStop();
      lcdShow("DISPENSING", "Try " + String(ackRetries + 1) + "/" + String(ACK_MAX_RETRIES));

      if (doneReceivedFlag) {   // retroactive proof: done/heartbeat arrived
        beepSingle();           // even though we may never have seen an ACK
        enterState(M_WAIT_LID);
        beepTwo();               // section 12: WAIT_LID gets two short beeps
        break;
      }
      if (ackReceivedFlag) {
        beepSingle();
        if (lastAckResult == 2) {
          enterFault("DISPENSER REJECTED REQUEST");
          break;
        }
        tAckReceivedAt = now;
        enterState(M_WAIT_GATE_DONE);
        break;
      }
      if (now - tLastAckAttempt >= ACK_RETRY_MS) {
        if (ackRetries >= ACK_MAX_RETRIES) {
          enterFault("NO DISPENSER ACK");
          break;
        }
        sendDropRequest(activeRequestId);
        tLastAckAttempt = now;
        ackRetries++;
      }
      break;
    }

    case M_WAIT_GATE_DONE:
      motorsStop();
      lcdShow("DISPENSING", "Gate closing");
      if (doneReceivedFlag) {
        enterState(M_WAIT_LID);
        beepTwo();               // section 12: WAIT_LID gets two short beeps
      } else if (now - tAckReceivedAt >= GATE_CONFIRM_GRACE_MS) {
        enterFault("GATE UNCONFIRMED");
      }
      break;

    case M_WAIT_LID:
      motorsStop();
      lcdShow("MED DROPPED", "Close the lid");
      if (LID_SWITCH_FITTED && lidSwitchClosed()) {
        enterState(M_LOAD_COUNTDOWN);
      }
      // Fallback: CONFIRM arrives via handleLidConfirm() / 'k' / second
      // START press, all of which call enterState(M_LOAD_COUNTDOWN)
      // directly. No timeout here on purpose -- see section 16.
      break;

    case M_LOAD_COUNTDOWN: {
      motorsStop();
      unsigned long elapsed = now - tState;
      long remain = (long)LOAD_COUNTDOWN_MS - (long)elapsed;
      lcdShow("MEDICINE LOADED", "Moving in: " + String(max(0L, remain / 1000)) + "s");
      static int lastBeep = -1;
      int sec = (int)(remain / 1000);
      if (remain <= 3000 && sec != lastBeep) { beepSingle(); lastBeep = sec; }
      if (elapsed >= LOAD_COUNTDOWN_MS) {
        lastBeep = -1;
        returning = false;
        tMarkerCreepStart = now;
        enterState(M_DEPART_STATION);
      }
      break;
    }

    case M_DEPART_STATION:
    case M_DEPART_BED: {
      lcdShow(state == M_DEPART_STATION ? "LEAVING DISP" : "LEAVING BED 1", "Finding line");
      drive(CREEP_PWM, CREEP_PWM + 15);
      // BUG FIX: "black < 5" also matched black == 0 -- completely blind,
      // not yet back on the line. That let the car hand off to OUTBOUND/
      // RETURNING while still off the line, which then ran the line-lost
      // recovery on its very first tick using lineLostDirection's untested
      // default (rightward) -- a confident-looking arc that had nothing to
      // do with where the line actually was. Require a GENUINE partial-line
      // read (1-4 black) before handing off, so line-following only ever
      // starts once the array has actually proven it can see the line.
      if (black > 0 && black < 5) {
        markerArmed = false;   // still cooling down from the marker we just left
        tNormalLineSince = now;
        enterState(state == M_DEPART_STATION ? M_OUTBOUND : M_RETURNING);
      } else if (now - tMarkerCreepStart >= MARKER_CREEP_TIMEOUT_MS) {
        enterFault("MARKER NOT CLEARED");
      }
      // black == 0: still creeping straight (the drive() call above already
      // covers this case) through the blind gap until the line reappears.
      break;
    }

    case M_OUTBOUND:
    case M_RETURNING: {
      lcdShow(state == M_OUTBOUND ? "GOING TO BED 1" : "RETURNING",
              "Dist:" + String(lastDistanceCm) + "cm IR:" + String(black));
      if (serviceMarkerDetection(black)) {
        if (state == M_OUTBOUND) {
          enterState(M_AT_BED);
          beepArrival();
        } else {
          enterState(M_AT_STATION);
        }
        break;
      }
      serviceLineFollow(black, b);
      break;
    }

    case M_AT_BED: {
      motorsStop();
      unsigned long elapsed = now - tState;
      long remain = (long)BED_WINDOW_MS - (long)elapsed;
      lcdShow("ARRIVED AT BED 1", "Take med: " + String(max(0L, remain/1000)) + "s");
      static int lastBeep = -1;
      int sec = (int)(remain/1000);
      if (remain <= 3000 && sec != lastBeep) { beepSingle(); lastBeep = sec; }
      if (elapsed >= BED_WINDOW_MS) {
        lastBeep = -1;
        spinPhase = SP_BLIND; tSpinPhase = now;
        enterState(M_BED_TURN);
      }
      break;
    }

    case M_BED_TURN:
    case M_STATION_TURN:
      lcdShow("TURNING 180", "Please clear");
      if (serviceSpin()) {
        if (state == M_BED_TURN) {
          returning = true;
          tMarkerCreepStart = now;
          enterState(M_DEPART_BED);
        } else {
          enterState(M_AT_DISPENSER);
        }
      }
      break;

    case M_AT_STATION: {
      motorsStop();
      lcdShow("AT DISPENSER", "Repositioning");
      static bool stationArrivalBeeped = false;
      if (!stationArrivalBeeped) { beepArrival(); stationArrivalBeeped = true; }
      if (now - tState >= AT_STATION_DWELL_MS) {
        stationArrivalBeeped = false;
        spinPhase = SP_BLIND; tSpinPhase = now;
        enterState(M_STATION_TURN);
      }
      break;
    }

    case M_OBSTACLE_PAUSE:
      // entry/exit handled above, before the switch
      break;

    case M_FAULT:
      motorsStop();
      lcdShow("FAULT", String(faultReason));
      static unsigned long tLastFaultBeep = 0;
      if (now - tLastFaultBeep >= 5000) { beepWarn(); tLastFaultBeep = now; }
      break;

    case M_EMERGENCY_STOP:
      motorsStop();
      lcdShow("EMERGENCY STOP", "Manual reset");
      break;
  }

  if (monitorOn && now - tLastMonitor >= monitorIntervalMs) {
    tLastMonitor = now;
    printMonitorRow();
  }
}
