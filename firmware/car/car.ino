/* ============================================================================
   Smart Hospital Assistant Robot  --  CAR firmware
   Board: ESP32 DevKit V1 (30-pin)   |   Arduino core for ESP32
   ----------------------------------------------------------------------------
   WHAT THE CAR DOES
     - Follows a line with the 5-channel IR array
     - Stops at every black marker (all five channels HIGH)
     - Stops the motors and waits for the PC to tell it what the marker is
     - Locks / unlocks / auto-locks the medicine compartment (SG90 on GPIO18)
     - Halts for obstacles (HC-SR04)
     - Spins 180 deg at the turn zone
     - Reports state over a small REST API

   DIVISION OF LABOUR (matches your build guide)
     The PC decodes every QR code. It then tells the car what to do with one
     of these calls while the car sits in SCANNING:
        /deliver   this marker is the TARGET bed  -> unlock + deliver
        /resume    this marker is the WRONG bed   -> drive to the next marker
        /spin      this marker is the TURN ZONE   -> 180 deg turnaround
        /home      this marker is the STATION      -> park, ready for next run
     Other endpoints:
        /go?bed=N  start a delivery run to bed N
        /abort     stop now, lock, and head home
        /status    JSON telemetry (the PC polls this)
        /lock      manual lock (only at STANDBY)

   HARDWARE NOTES (must match your wiring)
     - IR array powered from 3V3.  Polarity: WHITE = LOW, BLACK = HIGH.
       => a marker is "all five channels HIGH".
     - HC-SR04 ECHO reaches GPIO4 through a 1k/2k divider (never direct).
     - Servo signal: GPIO18 -> shifter LV3 -> HV3 -> servo orange.
     - Status LED on the pin silk-screened RX2 (that pin is GPIO16).
     - Active buzzer (3V, 30mA): until a transistor is fitted, wire it as
       GPIO19 -> 100 ohm -> buzzer(+), buzzer(-) -> GND. The resistor keeps
       the pin current under ~20 mA. Code is identical for the transistor
       version, so no change is needed when you upgrade the circuit.
     - NEVER drive the servo while the motors run. Only LOCKING, DELIVERING
       and AUTOLOCK touch the servo, and each stops the motors first.
     - Uses millis(), never delay(), so the web server stays responsive.
   ============================================================================ */

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

/* ---------- 1. USER CONFIG -------------------------------------------------- */
const char* WIFI_SSID = "";      // TODO: set your network name
const char* WIFI_PASS = "";      // TODO: set your password

// Laptop's Windows Mobile Hotspot: always 192.168.137.x, laptop at .1
IPAddress CAR_IP  (192, 168, 137, 51);   // dispenser .52, cam .53
IPAddress GATEWAY (192, 168, 137,  1);
IPAddress SUBNET  (255, 255, 255,  0);
IPAddress DNS1    (192, 168, 137,  1);

const int NUM_BEDS = 1;   // single bed for now

/* ---------- 2. PIN MAP ------------------------------------------------------ */
const int IR1 = 36, IR2 = 39, IR3 = 34, IR4 = 35, IR5 = 17;   // left..right
const int ENA = 14, IN1 = 27, IN2 = 26, IN3 = 25, IN4 = 32, ENB = 23;
const int TRIG = 13, ECHO = 4;
const int SERVO_PIN = 18, BUZZER = 19, LED_PIN = 16;   // LED_PIN = RX2 silk

/* ---------- 3. TUNING (adjust on the bench) -------------------------------- */
const int SPEED_CRUISE = 150;
const int SPEED_TURN   = 130;

const int LOCK_ANGLE   = 0;     // TODO: set your CLOSED angle (placeholder)
const int UNLOCK_ANGLE = 90;    // TODO: set your OPEN angle (placeholder)

const unsigned long CAMERA_SETTLE_MS = 1000;
const unsigned long SERVO_TRAVEL_MS  = 600;
const unsigned long AUTOLOCK_MS      = 30000;
const unsigned long MARKER_ON_MS     = 60;
const unsigned long MARKER_OFF_MS    = 300;
const unsigned long MARKER_COOLDOWN_MS = 2000;
const unsigned long SCAN_TIMEOUT_MS  = 60000;
const unsigned long LINE_LOST_MS     = 1500;

const unsigned long SPIN_BLIND_MS = 880;   // calibrate: 80% of measured 180 deg
const int SPIN_PWM      = 140;
const int SPIN_SLOW_PWM = 90;              // front-caster: slow re-acquire
const int CREEP_PWM     = 115;

const int OBSTACLE_CM = 25;

/* ---------- 4. PWM (LEDC) --------------------------------------------------- */
const int PWM_FREQ = 1000, PWM_RES = 8, CH_ENA = 0, CH_ENB = 1;

/* ---------- 5. STATE -------------------------------------------------------- */
enum State { STANDBY, LOCKING, DRIVING, SCANNING, DELIVERING,
             AUTOLOCK, SPINNING, FAULT };
State state = STANDBY;

int  targetBed = 0;
bool returning = false;
String lastEvent = "boot";

// commands set by the web handlers, consumed by the loop
volatile bool cmdDeliver = false;
volatile bool cmdResume  = false;
volatile bool cmdSpin    = false;
volatile bool cmdHome    = false;
volatile bool cmdAbort   = false;

// timers
unsigned long tState = 0, tUnlock = 0, tAllBlack = 0, tLastClear = 0, tAllWhite = 0;
bool cooldownActive = false; unsigned long cooldownStart = 0;

// spin sub-phases
enum { SP_CREEP = 0, SP_BLIND, SP_SLOW, SP_SETTLE, SP_DONE };
int spinPhase = 0; unsigned long spinPhaseT = 0;

long lastDistanceCm = 999;

Servo lockServo;
WebServer server(80);

/* ---------- 6. LOW-LEVEL HELPERS ------------------------------------------- */
void motorsStop() {
  ledcWrite(CH_ENA, 0); ledcWrite(CH_ENB, 0);
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}

void drive(int left, int right) {
  if (left >= 0) { digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW); }
  else           { digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH); left = -left; }
  if (right >= 0){ digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW); }
  else           { digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH); right = -right; }
  if (left  > 255) left  = 255;
  if (right > 255) right = 255;
  ledcWrite(CH_ENA, left); ledcWrite(CH_ENB, right);
}

void ledOn()  { digitalWrite(LED_PIN, HIGH); }
void ledOff() { digitalWrite(LED_PIN, LOW);  }

// Short blocking chirp. Only ever called when the car is stopped, so the tiny
// block is harmless. Never call inside the 30 s countdown loop body directly.
void chirp(int ms) {
  digitalWrite(BUZZER, HIGH);
  unsigned long t = millis();
  while (millis() - t < (unsigned long)ms) {}
  digitalWrite(BUZZER, LOW);
}

int readIR(bool b[5]) {
  b[0] = digitalRead(IR1); b[1] = digitalRead(IR2); b[2] = digitalRead(IR3);
  b[3] = digitalRead(IR4); b[4] = digitalRead(IR5);
  int n = 0; for (int i = 0; i < 5; i++) if (b[i]) n++;
  return n;
}

float linePosition(bool b[5]) {
  const int w[5] = {-2, -1, 0, 1, 2};
  int sum = 0, cnt = 0;
  for (int i = 0; i < 5; i++) if (b[i]) { sum += w[i]; cnt++; }
  return cnt ? (float)sum / cnt : 0;
}

long pingCm() {
  digitalWrite(TRIG, LOW);  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  long us = pulseIn(ECHO, HIGH, 25000);
  return us ? us / 58 : 999;
}

void servoLock()   { lockServo.write(LOCK_ANGLE);   }
void servoUnlock() { lockServo.write(UNLOCK_ANGLE); }

/* ---------- 7. WEB API ------------------------------------------------------ */
const char* stateName() {
  switch (state) {
    case STANDBY:    return "STANDBY";
    case LOCKING:    return "LOCKING";
    case DRIVING:    return "DRIVING";
    case SCANNING:   return "SCANNING";
    case DELIVERING: return "DELIVERING";
    case AUTOLOCK:   return "AUTOLOCK";
    case SPINNING:   return "SPINNING";
    case FAULT:      return "FAULT";
  }
  return "?";
}

void handleStatus() {
  String j = "{";
  j += "\"state\":\"" + String(stateName()) + "\",";
  j += "\"targetBed\":" + String(targetBed) + ",";
  j += "\"returning\":" + String(returning ? "true" : "false") + ",";
  j += "\"distanceCm\":" + String(lastDistanceCm) + ",";
  j += "\"lastEvent\":\"" + lastEvent + "\"}";
  server.send(200, "application/json", j);
}

void handleGo() {
  if (state != STANDBY)          { server.send(409, "text/plain", "busy");     return; }
  if (!server.hasArg("bed"))     { server.send(400, "text/plain", "need bed"); return; }
  int bed = server.arg("bed").toInt();
  if (bed < 1 || bed > NUM_BEDS) { server.send(400, "text/plain", "bad bed");  return; }
  targetBed = bed; returning = false;
  lastEvent = "go bed " + String(bed);
  state = LOCKING; tState = millis();
  server.send(200, "text/plain", "ok");
}

void handleDeliver() { cmdDeliver = true; server.send(200, "text/plain", "ok"); }
void handleResume()  { cmdResume  = true; server.send(200, "text/plain", "ok"); }
void handleSpin()    { cmdSpin    = true; server.send(200, "text/plain", "ok"); }
void handleHome()    { cmdHome    = true; server.send(200, "text/plain", "ok"); }
void handleAbort()   { cmdAbort   = true; server.send(200, "text/plain", "ok"); }
void handleLock()    { if (state == STANDBY) { servoLock(); lastEvent = "manual lock"; }
                       server.send(200, "text/plain", "ok"); }
void handleRoot()    { server.send(200, "text/plain",
   "Car online. /status /go?bed=N /deliver /resume /spin /home /abort /lock"); }

void setupServer() {
  server.on("/",        handleRoot);
  server.on("/status",  handleStatus);
  server.on("/go",      handleGo);
  server.on("/deliver", handleDeliver);
  server.on("/resume",  handleResume);
  server.on("/spin",    handleSpin);
  server.on("/home",    handleHome);
  server.on("/abort",   handleAbort);
  server.on("/lock",    handleLock);
  server.begin();
}

/* ---------- 8. SETUP -------------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(IR1, INPUT); pinMode(IR2, INPUT); pinMode(IR3, INPUT);
  pinMode(IR4, INPUT); pinMode(IR5, INPUT);

  pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT); pinMode(IN4, OUTPUT);
  ledcSetup(CH_ENA, PWM_FREQ, PWM_RES); ledcAttachPin(ENA, CH_ENA);
  ledcSetup(CH_ENB, PWM_FREQ, PWM_RES); ledcAttachPin(ENB, CH_ENB);
  motorsStop();

  pinMode(TRIG, OUTPUT); pinMode(ECHO, INPUT); digitalWrite(TRIG, LOW);
  pinMode(BUZZER, OUTPUT); digitalWrite(BUZZER, LOW);
  pinMode(LED_PIN, OUTPUT); ledOff();

  lockServo.setPeriodHertz(50);
  lockServo.attach(SERVO_PIN, 500, 2400);
  servoUnlock();                       // at the station, lock OPEN

  WiFi.mode(WIFI_STA);
  WiFi.config(CAR_IP, GATEWAY, SUBNET, DNS1);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));
  }
  ledOff();
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nCar IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi FAILED - check SSID/PASS/IP. Continuing offline.");
  }

  setupServer();
  state = STANDBY; tState = millis(); tLastClear = millis();
  lastEvent = "ready";
  Serial.println("Car ready. State: STANDBY");
}

/* ---------- 9. MARKER DETECTION -------------------------------------------- */
bool markerDetected(int blackCount) {
  unsigned long now = millis();
  if (cooldownActive) {
    if (now - cooldownStart >= MARKER_COOLDOWN_MS) cooldownActive = false;
    else { if (blackCount < 5) tLastClear = now; return false; }
  }
  if (blackCount == 5) {
    if (tAllBlack == 0) tAllBlack = now;
    bool clearedEnough = (tAllBlack - tLastClear) >= MARKER_OFF_MS;
    if (clearedEnough && (now - tAllBlack) >= MARKER_ON_MS) {
      tAllBlack = 0; return true;
    }
  } else {
    tAllBlack = 0; tLastClear = now;
  }
  return false;
}

/* ---------- 10. SPIN (non-blocking). Returns true when complete. ----------- */
bool spinStep(int blackCount, bool b[5]) {
  unsigned long now = millis();
  switch (spinPhase) {
    case SP_CREEP:
      drive(CREEP_PWM, CREEP_PWM);
      if (blackCount < 5) { spinPhase = SP_BLIND; spinPhaseT = now; }
      break;
    case SP_BLIND:
      drive(SPIN_PWM, -SPIN_PWM);
      if (now - spinPhaseT >= SPIN_BLIND_MS) { spinPhase = SP_SLOW; spinPhaseT = now; }
      break;
    case SP_SLOW:
      drive(SPIN_SLOW_PWM, -SPIN_SLOW_PWM);
      if (b[2]) { spinPhase = SP_SETTLE; spinPhaseT = now; motorsStop(); }
      if (now - spinPhaseT > 4000UL) return true;   // safety -> caller faults
      break;
    case SP_SETTLE:
      motorsStop();
      if (now - spinPhaseT >= 200) spinPhase = SP_DONE;
      break;
    case SP_DONE:
      return true;
  }
  return false;
}

/* ---------- 11. MAIN LOOP --------------------------------------------------- */
void loop() {
  server.handleClient();

  bool b[5];
  int black = readIR(b);
  lastDistanceCm = pingCm();
  unsigned long now = millis();

  // global abort: stop, lock, head home
  if (cmdAbort) {
    cmdAbort = false;
    motorsStop(); servoLock(); ledOff();
    returning = true; lastEvent = "abort -> returning";
    cooldownActive = true; cooldownStart = now;
    tLastClear = now; tAllBlack = 0;
    state = DRIVING; tState = now;
  }

  switch (state) {

    case STANDBY:
      motorsStop(); ledOff();
      break;

    case LOCKING:
      motorsStop(); servoLock();
      if (now - tState >= SERVO_TRAVEL_MS) {
        state = DRIVING; tState = now;
        tLastClear = now; tAllBlack = 0;
        lastEvent = "departed";
      }
      break;

    case DRIVING: {
      if (lastDistanceCm <= OBSTACLE_CM) {   // obstacle -> hold
        motorsStop(); lastEvent = "obstacle"; break;
      }
      if (markerDetected(black)) {           // reached a marker
        motorsStop();
        lastEvent = "marker: scanning";
        cmdDeliver = cmdResume = cmdSpin = cmdHome = false;
        state = SCANNING; tState = now;
        break;
      }
      // line following
      if (black == 5) {
        drive(SPEED_CRUISE, SPEED_CRUISE); tAllWhite = 0;
      } else if (black == 0) {
        if (tAllWhite == 0) tAllWhite = now;
        if (now - tAllWhite >= LINE_LOST_MS) {
          motorsStop(); lastEvent = "line lost";
          state = FAULT; tState = now; break;
        }
        drive(SPEED_CRUISE, SPEED_CRUISE);
      } else {
        tAllWhite = 0;
        float pos = linePosition(b);
        int corr = (int)(pos * (SPEED_TURN * 0.5));
        drive(SPEED_CRUISE + corr, SPEED_CRUISE - corr);
      }
      break;
    }

    case SCANNING:
      motorsStop();
      // The PC has pulled /capture, decoded, and now tells us what this is:
      if (cmdDeliver) {                       // target bed
        cmdDeliver = false;
        state = DELIVERING; tState = now; tUnlock = now;
        servoUnlock(); ledOn();
        lastEvent = "delivering"; chirp(150);
      } else if (cmdResume) {                 // wrong bed
        cmdResume = false;
        cooldownActive = true; cooldownStart = now;
        tLastClear = now; tAllBlack = 0;
        state = DRIVING; tState = now; lastEvent = "resume";
      } else if (cmdSpin) {                    // turn zone
        cmdSpin = false;
        spinPhase = SP_CREEP; spinPhaseT = now;
        state = SPINNING; tState = now; lastEvent = "spinning";
      } else if (cmdHome) {                    // station, run complete
        cmdHome = false;
        motorsStop(); servoUnlock();
        returning = false; targetBed = 0;
        state = STANDBY; tState = now; lastEvent = "home; ready";
      } else if (now - tState >= SCAN_TIMEOUT_MS) {
        lastEvent = "scan timeout"; state = FAULT; tState = now;
      }
      break;

    case DELIVERING: {
      motorsStop();
      unsigned long elapsed = now - tUnlock;
      static int lastWarn = -1;
      long remain = (long)AUTOLOCK_MS - (long)elapsed;
      int warn = -1;
      if      (remain <= 3000)  warn = 3;
      else if (remain <= 5000)  warn = 5;
      else if (remain <= 10000) warn = 10;
      if (warn != -1 && warn != lastWarn) { chirp(80); lastWarn = warn; }
      if (elapsed >= AUTOLOCK_MS) {
        lastWarn = -1;
        state = AUTOLOCK; tState = now; lastEvent = "auto-locking";
      }
      break;
    }

    case AUTOLOCK:
      motorsStop(); servoLock(); ledOff();
      if (now - tState >= SERVO_TRAVEL_MS) {
        state = DRIVING; tState = now;
        tLastClear = now; tAllBlack = 0;
        lastEvent = "heading to turn zone";
      }
      break;

    case SPINNING: {
      if (spinStep(black, b)) {
        if (spinPhase != SP_DONE) {           // safety timeout fired
          motorsStop(); lastEvent = "spin failed";
          state = FAULT; tState = now; break;
        }
        returning = true;
        cooldownActive = true; cooldownStart = now;
        tLastClear = now; tAllBlack = 0;
        state = DRIVING; tState = now; lastEvent = "returning";
      }
      break;
    }

    case FAULT:
      motorsStop(); ledOn();                  // steady LED = fault; wait /abort/reset
      break;
  }
}
