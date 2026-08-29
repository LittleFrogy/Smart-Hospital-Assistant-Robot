/* ============================================================================
   Smart Hospital Assistant Robot  --  DISPENSER firmware
   Board: ESP32 DevKit V1 (30-pin)   |   Arduino core for ESP32
   ----------------------------------------------------------------------------
   WHAT THE DISPENSER DOES
     - Waits for the PC (or an optional button) to trigger a drop
     - Opens the SG90 gate, holds briefly, closes it again
     - Reports ready / busy over a small REST API
     - Blinks the onboard LED as a heartbeat so you can see it is alive

   ENDPOINTS
     /drop     open the gate, hold, close   (ignored while already busy)
     /status   JSON: {"state":"ready"|"busy"}
     /         plain-text help

   HARDWARE NOTES (must match your wiring)
     - This unit is stationary: power it from a 5V 2A USB adapter into VIN,
       not a LiPo.
     - Servo: GPIO18 -> servo orange (signal), red -> 5V, brown -> GND.
       No level shifter on this board (SG90 triggers fine on 3.3V PWM).
     - Onboard blue LED is GPIO2 (heartbeat). No wiring needed.
     - Optional manual button: GPIO4 -> button -> GND, uses INPUT_PULLUP.
     - Uses millis(), never delay(), so the web server stays responsive.
   ============================================================================ */

#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>

/* ---------- 1. USER CONFIG -------------------------------------------------- */
const char* WIFI_SSID = "";      // TODO: set your network name
const char* WIFI_PASS = "";      // TODO: set your password

// Static IP. MUST differ from the car (192.168.0.51) and the cam.
IPAddress DISP_IP (192, 168, 0, 52);
IPAddress GATEWAY (192, 168, 0, 1);
IPAddress SUBNET  (255, 255, 255, 0);
IPAddress DNS1    (192, 168, 0, 1);

/* ---------- 2. PIN MAP ------------------------------------------------------ */
const int SERVO_PIN = 18;   // gate servo signal
const int LED_PIN   = 2;    // onboard blue LED (heartbeat)
const int BUTTON    = 4;    // optional manual drop button -> GND

/* ---------- 3. TUNING (set the angles to YOUR gate) ------------------------ */
const int GATE_CLOSED = 0;     // TODO: set your CLOSED angle (placeholder)
const int GATE_OPEN   = 90;    // TODO: set your OPEN angle (placeholder)

const unsigned long DROP_HOLD_MS = 800;    // how long the gate stays open
const unsigned long BTN_DEBOUNCE_MS = 50;

/* ---------- 4. STATE -------------------------------------------------------- */
enum State { READY, DROPPING };
State state = READY;

unsigned long tDrop = 0;         // when the current drop began
volatile bool cmdDrop = false;   // set by /drop or the button

// heartbeat
unsigned long tBlink = 0;
bool ledState = false;

// button debounce
int  lastBtnReading = HIGH;
unsigned long tBtnChange = 0;

Servo gateServo;
WebServer server(80);

/* ---------- 5. SERVO HELPERS ----------------------------------------------- */
void gateClose() { gateServo.write(GATE_CLOSED); }
void gateOpen()  { gateServo.write(GATE_OPEN);   }

/* ---------- 6. WEB API ------------------------------------------------------ */
void handleStatus() {
  String s = (state == READY) ? "ready" : "busy";
  server.send(200, "application/json", "{\"state\":\"" + s + "\"}");
}

void handleDrop() {
  if (state != READY) { server.send(409, "text/plain", "busy"); return; }
  cmdDrop = true;
  server.send(200, "text/plain", "ok");
}

void handleRoot() {
  server.send(200, "text/plain", "Dispenser online. /drop  /status");
}

void setupServer() {
  server.on("/",       handleRoot);
  server.on("/drop",   handleDrop);
  server.on("/status", handleStatus);
  server.begin();
}

/* ---------- 7. SETUP -------------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LOW);
  pinMode(BUTTON, INPUT_PULLUP);

  gateServo.setPeriodHertz(50);
  gateServo.attach(SERVO_PIN, 500, 2400);
  gateClose();                         // start closed

  WiFi.mode(WIFI_STA);
  WiFi.config(DISP_IP, GATEWAY, SUBNET, DNS1);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print(".");
    digitalWrite(LED_PIN, !digitalRead(LED_PIN));   // fast blink while connecting
  }
  digitalWrite(LED_PIN, LOW);
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nDispenser IP: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi FAILED - check SSID/PASS/IP. Continuing offline.");
  }

  setupServer();
  state = READY;
  Serial.println("Dispenser ready. State: READY");
}

/* ---------- 8. BUTTON (optional) ------------------------------------------- */
void pollButton() {
  int reading = digitalRead(BUTTON);       // LOW = pressed (INPUT_PULLUP)
  if (reading != lastBtnReading) {
    tBtnChange = millis();
    lastBtnReading = reading;
  }
  if (reading == LOW && (millis() - tBtnChange) > BTN_DEBOUNCE_MS) {
    if (state == READY) cmdDrop = true;    // same effect as /drop
  }
}

/* ---------- 9. HEARTBEAT ---------------------------------------------------- */
void heartbeat() {
  unsigned long now = millis();
  // solid ON while dropping; slow 1 Hz blink while ready
  if (state == DROPPING) { digitalWrite(LED_PIN, HIGH); return; }
  if (now - tBlink >= 1000) {
    tBlink = now; ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }
}

/* ---------- 10. MAIN LOOP --------------------------------------------------- */
void loop() {
  server.handleClient();
  pollButton();
  heartbeat();
  unsigned long now = millis();

  switch (state) {

    case READY:
      if (cmdDrop) {
        cmdDrop = false;
        gateOpen();
        tDrop = now;
        state = DROPPING;
        Serial.println("drop: gate open");
      }
      break;

    case DROPPING:
      if (now - tDrop >= DROP_HOLD_MS) {
        gateClose();
        state = READY;
        Serial.println("drop: gate closed, ready");
      }
      break;
  }
}
