/*
  Smart Hospital Medicine Robot - FIXED DISPENSER
  Board: ESP32 DevKit V1 (30 pin)
  Target: ESP32 Arduino Core 3.x ONLY

  NO EXTERNAL LIBRARIES REQUIRED.
    ESP32Servo has been removed. The gate servo is driven directly by LEDC on
    an explicitly pinned channel, so nothing else can steal its timer.

  NETWORK
    Joins the phone hotspot by DHCP. ESP-NOW receives drop requests from the
    car on the hotspot's channel. The numeric IP is only for the optional
    diagnostic webpage; the car does not depend on this IP.

  SERIAL MONITOR
    115200 baud, Newline or Both NL & CR. Type h for commands.
    The live component monitor prints one aligned row per sample showing every
    component side by side. Type p for a full vertical component report.
*/

#include <esp_arduino_version.h>

#if ESP_ARDUINO_VERSION_MAJOR < 3
#error "This sketch requires ESP32 Arduino Core 3.x. Update Boards Manager."
#endif

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_now.h>
#include <esp_system.h>
#include <Preferences.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// 1. USER CONFIGURATION
// ---------------------------------------------------------------------------

const char *WIFI_SSID = "froggy";
const char *WIFI_PASS = "Passwordki";

// Printed at every boot and exposed by /status. If this exact text is not in
// the Serial Monitor, this file was not flashed to that board.
const char FIRMWARE_BUILD[] = "DISP-V2.1-P2-12B-20260902";

// Calibrate with the servo horn/linkage disconnected first.
// Defaults deliberately avoid 0 and 180: those map to the extreme pulse
// widths where many SG90-class servos stall against a mechanical stop, which
// draws continuous current and can brown out the 5 V rail.
// Use the 'g' self-test and the '[' / ']' nudge commands to find real values.
int GATE_CLOSED_ANGLE = 100;
int GATE_OPEN_ANGLE   = 20;

// Time the gate remains open so one medicine package can fall.
unsigned long GATE_OPEN_HOLD_MS = 1500;
const unsigned long SERVO_TRAVEL_MS = 800;
const unsigned long RADIO_RETRY_MS = 5000;

// Safety limit for the bench-only manual open. Without this the gate could sit
// open indefinitely with the servo holding position.
const unsigned long MANUAL_OPEN_TIMEOUT_MS = 60000;

// ---------------------------------------------------------------------------
// 2. PIN MAP
// ---------------------------------------------------------------------------

const int DISPENSER_SERVO_PIN = 18;
const int STATUS_LED_PIN      = 2;  // onboard LED on most DevKit V1 boards
const int OPTIONAL_BUTTON_PIN = 32; // button to GND; INPUT_PULLUP

const bool OPTIONAL_BUTTON_ENABLED = false;

// ---------------------------------------------------------------------------
// 3. LEDC ALLOCATION FOR THE GATE SERVO
// ---------------------------------------------------------------------------
// ESP32 LEDC pairs channels onto timers: ch0/ch1 -> timer0, ch2/ch3 -> timer1,
// ch4/ch5 -> timer2, ch6/ch7 -> timer3. Channel 4 is pinned here so the same
// numbering is used on both boards and nothing can collide with it later.

const int SERVO_PWM_FREQUENCY  = 50;
const int SERVO_PWM_RESOLUTION = 16;
const int SERVO_LEDC_CHANNEL   = 4;   // timer 2
const int SERVO_MIN_PULSE_US   = 500;
const int SERVO_MAX_PULSE_US   = 2400;
const unsigned long SERVO_PERIOD_US = 20000;

// ---------------------------------------------------------------------------
// 4. RADIO PACKET - MUST MATCH car_glovebox.ino
// ---------------------------------------------------------------------------

const uint32_t RADIO_MAGIC = 0x4D454452; // "MEDR"
const uint8_t RADIO_VERSION = 2;

enum RadioMessageType : uint8_t {
  MSG_DISPENSER_HEARTBEAT = 1,
  MSG_DROP_REQUEST        = 2,
  MSG_DROP_ACK            = 3,
  MSG_DROP_DONE           = 4
};

struct __attribute__((packed)) RadioPacket {
  uint32_t magic;
  uint8_t version;
  uint8_t type;
  uint8_t deviceState;
  uint8_t resultCode;
  uint32_t requestId;
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

const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// ---------------------------------------------------------------------------
// 5. DISPENSER STATE
// ---------------------------------------------------------------------------

enum DispenserState : uint8_t {
  DISP_WAIT_WIFI = 0,
  DISP_READY = 1,
  DISP_GATE_OPENING = 2,
  DISP_GATE_HOLD = 3,
  DISP_GATE_CLOSING = 4,
  DISP_MANUAL_OPEN = 5,
  DISP_FAULT = 6
};

DispenserState state = DISP_WAIT_WIFI;
unsigned long stateSince = 0;
char faultReason[33] = "NONE";

bool gateOpen = false;
bool servoPwmReady = false;
bool servoHolding = false;
int servoTargetAngle = -1;
int servoPulseUs = 0;
int servoNudgeAngle = 20;

uint32_t activeRequestId = 0;
uint32_t lastCompletedRequestId = 0;
uint32_t lastAcceptedRequestId = 0;
bool interruptedRequestPending = false;
uint32_t completedDropCount = 0;

portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool callbackRequestPending = false;
volatile uint32_t callbackRequestId = 0;
volatile uint32_t radioPacketsSeen = 0;
volatile uint32_t radioRawPacketsSeen = 0;
volatile uint32_t radioWrongLength = 0;
volatile uint32_t radioWrongProtocol = 0;
volatile int lastRadioRxLength = -1;
volatile int lastRadioTxLength = 0;
volatile uint32_t radioPacketsQueued = 0;
volatile uint32_t radioQueueErrors = 0;

bool radioReady = false;
unsigned long lastHeartbeatMs = 0;
unsigned long heartbeatsSent = 0;
unsigned long lastWiFiRetryMs = 0;
unsigned long lastRadioInitAttemptMs = 0;
bool wasWiFiConnected = false;
bool serverStarted = false;
bool requestPreferencesReady = false;

bool monitorEnabled = true;
unsigned long monitorIntervalMs = 500;
unsigned long lastMonitorMs = 0;
int monitorRowsSinceHeader = 0;

bool statusLedOn = false;
bool buttonPrevious = HIGH;
unsigned long lastButtonChangeMs = 0;

WebServer server(80);
Preferences requestPreferences;

void sendHeartbeat();
void printComponentReport();
void printMonitorHeader();
bool startLocalDrop(uint32_t requestId, const char *source);
void printNetworkDetails();

// ---------------------------------------------------------------------------
// 6. BASIC HELPERS
// ---------------------------------------------------------------------------

const char *stateName(DispenserState value) {
  switch (value) {
    case DISP_WAIT_WIFI:    return "WAIT_WIFI";
    case DISP_READY:        return "READY";
    case DISP_GATE_OPENING: return "GATE_OPENING";
    case DISP_GATE_HOLD:    return "GATE_HOLD";
    case DISP_GATE_CLOSING: return "GATE_CLOSING";
    case DISP_MANUAL_OPEN:  return "MANUAL_OPEN";
    case DISP_FAULT:        return "FAULT";
  }
  return "UNKNOWN";
}

void enterState(DispenserState next, const char *reason) {
  DispenserState previous = state;
  state = next;
  stateSince = millis();
  Serial.printf("[STATE] %s -> %s | %s\n",
                stateName(previous), stateName(next), reason ? reason : "");
}

void setDispenserFault(const char *reason) {
  strncpy(faultReason, reason ? reason : "UNKNOWN", sizeof(faultReason));
  faultReason[sizeof(faultReason) - 1] = '\0';
  enterState(DISP_FAULT, faultReason);
}

// ---------------------------------------------------------------------------
// 7. GATE SERVO - RAW LEDC, NO ESP32Servo
// ---------------------------------------------------------------------------

bool initServoPwm() {
  servoPwmReady = ledcAttachChannel(DISPENSER_SERVO_PIN, SERVO_PWM_FREQUENCY,
                                    SERVO_PWM_RESOLUTION, SERVO_LEDC_CHANNEL);
  if (servoPwmReady) {
    ledcWrite(DISPENSER_SERVO_PIN, 0);
    servoHolding = false;
    servoPulseUs = 0;
  }
  Serial.printf("[SERVO] Gate LEDC ch%d on GPIO%d at %d Hz / %d bit: %s\n",
                SERVO_LEDC_CHANNEL, DISPENSER_SERVO_PIN, SERVO_PWM_FREQUENCY,
                SERVO_PWM_RESOLUTION, servoPwmReady ? "OK" : "FAIL");
  return servoPwmReady;
}

int servoAngleToPulseUs(int angle) {
  angle = constrain(angle, 0, 180);
  return SERVO_MIN_PULSE_US +
         (int)(((long)(SERVO_MAX_PULSE_US - SERVO_MIN_PULSE_US) * angle) / 180);
}

void servoWriteAngle(int angle) {
  if (!servoPwmReady) {
    Serial.println("[SERVO] Write ignored: LEDC channel was never attached");
    return;
  }
  angle = constrain(angle, 0, 180);
  int pulseUs = servoAngleToPulseUs(angle);
  uint32_t maxDuty = (1UL << SERVO_PWM_RESOLUTION) - 1UL;
  uint32_t duty = (uint32_t)(((uint64_t)pulseUs * (maxDuty + 1UL)) /
                             SERVO_PERIOD_US);
  ledcWrite(DISPENSER_SERVO_PIN, duty);
  servoHolding = true;
  servoTargetAngle = angle;
  servoPulseUs = pulseUs;
  Serial.printf("[SERVO] angle=%d deg  pulse=%d us  duty=%lu/%lu\n",
                angle, pulseUs, (unsigned long)duty, (unsigned long)maxDuty);
}

void servoRelease() {
  if (!servoPwmReady) return;
  ledcWrite(DISPENSER_SERVO_PIN, 0);
  servoHolding = false;
  servoPulseUs = 0;
  Serial.println("[SERVO] Gate pulse train stopped (servo released)");
}

void commandGateOpen() {
  servoWriteAngle(GATE_OPEN_ANGLE);
  gateOpen = true;
  Serial.printf("[SERVO] GATE OPEN command: %d degrees\n", GATE_OPEN_ANGLE);
}

void commandGateClose() {
  servoWriteAngle(GATE_CLOSED_ANGLE);
  gateOpen = false;
  Serial.printf("[SERVO] GATE CLOSED command: %d degrees\n", GATE_CLOSED_ANGLE);
}

// ---------------------------------------------------------------------------
// 8. NON-VOLATILE DUPLICATE PROTECTION
// ---------------------------------------------------------------------------

bool persistAcceptedRequest(uint32_t requestId) {
  // Block this ID in RAM immediately too. Even if one NVS write fails and the
  // operator resets the fault without rebooting, a delayed duplicate cannot
  // be mistaken for a new request.
  lastAcceptedRequestId = requestId;
  interruptedRequestPending = true;
  if (!requestPreferencesReady) return false;

  size_t acceptedBytes = requestPreferences.putUInt("accepted", requestId);
  size_t pendingBytes = requestPreferences.putBool("pending", true);
  if (acceptedBytes != sizeof(uint32_t) || pendingBytes != sizeof(bool)) {
    Serial.printf("[NVS] ERROR: accept write failed (%u/%u bytes)\n",
                  (unsigned)acceptedBytes, (unsigned)pendingBytes);
    return false;
  }
  return true;
}

bool persistCompletedRequest(uint32_t requestId) {
  if (!requestPreferencesReady) return false;

  // Keep pending=true unless the completed ID was stored successfully. A
  // partial write therefore reboots into the safer interrupted-drop fault.
  size_t completedBytes = requestPreferences.putUInt("completed", requestId);
  if (completedBytes != sizeof(uint32_t)) {
    Serial.printf("[NVS] ERROR: completion ID write failed (%u bytes)\n",
                  (unsigned)completedBytes);
    return false;
  }
  size_t pendingBytes = requestPreferences.putBool("pending", false);
  if (pendingBytes != sizeof(bool)) {
    Serial.printf("[NVS] ERROR: pending-clear write failed (%u bytes)\n",
                  (unsigned)pendingBytes);
    return false;
  }
  lastCompletedRequestId = requestId;
  interruptedRequestPending = false;
  completedDropCount++;
  return true;
}

bool resetInterruptedFault(const char *source) {
  if (state != DISP_FAULT) {
    Serial.println("[FAULT] Reset refused: no dispenser fault is active");
    return false;
  }
  if (!servoPwmReady) {
    Serial.println("[FAULT] Reset refused: gate servo PWM is not attached");
    return false;
  }

  // Keep accepted != completed. A late retry of the interrupted request is
  // blocked, while a new request ID can be accepted after operator inspection.
  if (!requestPreferencesReady ||
      requestPreferences.putBool("pending", false) != sizeof(bool)) {
    setDispenserFault("NVS RESET WRITE");
    return false;
  }
  interruptedRequestPending = false;
  activeRequestId = 0;
  strncpy(faultReason, "NONE", sizeof(faultReason));
  faultReason[sizeof(faultReason) - 1] = '\0';
  enterState(WiFi.status() == WL_CONNECTED ? DISP_READY : DISP_WAIT_WIFI,
             source ? source : "operator fault reset");
  sendHeartbeat();
  return true;
}

// ---------------------------------------------------------------------------
// 9. ESP-NOW RADIO
// ---------------------------------------------------------------------------

void parseRadioPacket(const uint8_t *data, int len) {
  radioRawPacketsSeen++;
  lastRadioRxLength = len;
  if (len != (int)RADIO_PACKET_BYTES) {
    radioWrongLength++;
    return;
  }

  RadioPacket packet;
  memcpy(&packet, data, RADIO_PACKET_BYTES);
  if (packet.magic != RADIO_MAGIC || packet.version != RADIO_VERSION) {
    radioWrongProtocol++;
    return;
  }

  portENTER_CRITICAL(&radioMux);
  radioPacketsSeen++;
  portEXIT_CRITICAL(&radioMux);

  if (packet.type != MSG_DROP_REQUEST || packet.requestId == 0) return;

  portENTER_CRITICAL(&radioMux);
  callbackRequestId = packet.requestId;
  callbackRequestPending = true;
  portEXIT_CRITICAL(&radioMux);
}

void onRadioReceive(const esp_now_recv_info_t *info,
                    const uint8_t *data, int len) {
  (void)info;
  parseRadioPacket(data, len);
}

bool initializeEspNow() {
  lastRadioInitAttemptMs = millis();
  esp_now_deinit();
  if (esp_now_init() != ESP_OK) {
    Serial.println("[RADIO] ERROR: esp_now_init failed");
    radioReady = false;
    return false;
  }

  if (esp_now_register_recv_cb(onRadioReceive) != ESP_OK) {
    Serial.println("[RADIO] ERROR: receive callback registration failed");
    esp_now_deinit();
    radioReady = false;
    return false;
  }

  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = 0;
  peer.encrypt = false;
  peer.ifidx = WIFI_IF_STA;

  if (!esp_now_is_peer_exist(BROADCAST_MAC) &&
      esp_now_add_peer(&peer) != ESP_OK) {
    Serial.println("[RADIO] ERROR: could not add broadcast peer");
    radioReady = false;
    return false;
  }

  radioReady = true;
  Serial.printf("[RADIO] ESP-NOW ready on Wi-Fi channel %d\n", WiFi.channel());
  return true;
}

bool transmitPacket(uint8_t type, uint32_t requestId, uint8_t resultCode) {
  if (!radioReady) return false;

  RadioPacket packet = {};
  packet.magic = RADIO_MAGIC;
  packet.version = RADIO_VERSION;
  packet.type = type;
  packet.deviceState = (uint8_t)state;
  packet.resultCode = resultCode;
  packet.requestId = requestId;

  lastRadioTxLength = (int)RADIO_PACKET_BYTES;
  esp_err_t result = esp_now_send(BROADCAST_MAC,
                                  reinterpret_cast<const uint8_t *>(&packet),
                                  RADIO_PACKET_BYTES);
  if (result != ESP_OK) {
    radioQueueErrors++;
    Serial.printf("[RADIO] Send error: %d\n", (int)result);
    return false;
  }
  radioPacketsQueued++;
  return true;
}

void sendAck(uint32_t requestId, uint8_t resultCode) {
  bool queued = transmitPacket(MSG_DROP_ACK, requestId, resultCode);
  Serial.printf("[RADIO] ACK id=%lu result=%u queued=%s\n",
                (unsigned long)requestId, resultCode, queued ? "yes" : "no");
}

void sendDone(uint32_t requestId) {
  bool queued = transmitPacket(MSG_DROP_DONE, requestId, 0);
  Serial.printf("[RADIO] DONE id=%lu queued=%s\n",
                (unsigned long)requestId, queued ? "yes" : "no");
}

void sendHeartbeat() {
  if (!radioReady) return;
  // Carry the most recently completed ID in every heartbeat. This lets the
  // car confirm gate closure even if the one-time DONE packet is lost.
  bool queued = transmitPacket(MSG_DISPENSER_HEARTBEAT, lastCompletedRequestId, 0);
  heartbeatsSent++;
  Serial.printf("[PROTO TX] HEARTBEAT bytes=%u version=%u state=%u completed=%lu queued=%s\n",
                (unsigned)RADIO_PACKET_BYTES, RADIO_VERSION, (unsigned)state,
                (unsigned long)lastCompletedRequestId, queued ? "yes" : "NO");
}

void processRadioRequests() {
  bool pending;
  uint32_t requestId;

  portENTER_CRITICAL(&radioMux);
  pending = callbackRequestPending;
  requestId = callbackRequestId;
  callbackRequestPending = false;
  portEXIT_CRITICAL(&radioMux);

  if (!pending) return;

  Serial.printf("[RADIO] DROP request received id=%lu while state=%s\n",
                (unsigned long)requestId, stateName(state));

  // Retry during the currently active physical cycle: acknowledge it without
  // moving the gate a second time.
  if (requestId == activeRequestId && activeRequestId != 0) {
    Serial.printf("[RADIO] Active duplicate id=%lu; NO second drop\n",
                  (unsigned long)requestId);
    sendAck(requestId, 1);
    return;
  }

  // Completed duplicate: acknowledge and repeat DONE, but never move again.
  if (requestId == lastCompletedRequestId && requestId != 0) {
    Serial.printf("[RADIO] Completed duplicate id=%lu; NO second drop\n",
                  (unsigned long)requestId);
    sendAck(requestId, 1);
    sendDone(requestId);
    return;
  }

  // The board rebooted during this request. Its physical outcome is unknown,
  // so deliberately do not ACK the old ID. The operator must inspect/reset.
  if (requestId == lastAcceptedRequestId &&
      lastAcceptedRequestId != lastCompletedRequestId) {
    Serial.printf("[RADIO] BLOCKED interrupted request id=%lu\n",
                  (unsigned long)requestId);
    if (state == DISP_FAULT) sendAck(requestId, 2);
    return;
  }

  if (state != DISP_READY) {
    Serial.printf("[RADIO] Busy; request id=%lu not accepted\n",
                  (unsigned long)requestId);
    // No ACK: the car retries; once ready, a later retry can be accepted.
    return;
  }

  if (!servoPwmReady) {
    Serial.println("[RADIO] Refused: gate servo PWM channel is not attached");
    setDispenserFault("SERVO PWM FAILED");
    sendAck(requestId, 2);
    return;
  }

  activeRequestId = requestId;
  if (!persistAcceptedRequest(requestId)) {
    activeRequestId = 0;
    setDispenserFault("NVS ACCEPT WRITE");
    sendAck(requestId, 2);
    return;
  }
  commandGateOpen();
  enterState(DISP_GATE_OPENING, "new ESP-NOW drop request accepted");

  // ACK is sent after issuing the physical open command. The car still waits
  // for DONE (gate closed), then for the operator's lid confirmation, before
  // it starts its departure countdown.
  sendAck(requestId, 0); // result 0 = new request accepted
}

// ---------------------------------------------------------------------------
// 10. DISPENSER CYCLE
// ---------------------------------------------------------------------------

bool startLocalDrop(uint32_t requestId, const char *source) {
  if (state != DISP_READY) {
    Serial.printf("[DROP] Refused from %s: state=%s\n",
                  source, stateName(state));
    return false;
  }
  if (!servoPwmReady) {
    Serial.println("[DROP] Refused: gate servo PWM channel is not attached");
    return false;
  }

  if (requestId == 0) requestId = esp_random();
  if (requestId == 0) requestId = 1;
  activeRequestId = requestId;
  if (!persistAcceptedRequest(requestId)) {
    activeRequestId = 0;
    setDispenserFault("NVS ACCEPT WRITE");
    return false;
  }
  commandGateOpen();
  enterState(DISP_GATE_OPENING, source);
  Serial.printf("[DROP] Local/test cycle id=%lu\n", (unsigned long)requestId);
  return true;
}

void serviceDispenserCycle() {
  unsigned long elapsed = millis() - stateSince;

  switch (state) {
    case DISP_GATE_OPENING:
      if (elapsed >= SERVO_TRAVEL_MS) {
        enterState(DISP_GATE_HOLD, "gate reached open angle");
      }
      break;

    case DISP_GATE_HOLD:
      if (elapsed >= GATE_OPEN_HOLD_MS) {
        commandGateClose();
        enterState(DISP_GATE_CLOSING, "drop hold complete");
      }
      break;

    case DISP_GATE_CLOSING:
      if (elapsed >= SERVO_TRAVEL_MS) {
        if (!servoPwmReady) {
          setDispenserFault("SERVO PWM FAILED");
          break;
        }
        servoRelease();
        uint32_t completedId = activeRequestId;
        bool completionStored =
            completedId == 0 || persistCompletedRequest(completedId);
        activeRequestId = 0;
        if (!completionStored) {
          setDispenserFault("NVS COMPLETE WRITE");
        } else if (interruptedRequestPending && completedId == 0) {
          enterState(DISP_FAULT,
                     "gate closed but interrupted request still needs reset");
        } else {
          enterState(WiFi.status() == WL_CONNECTED ? DISP_READY : DISP_WAIT_WIFI,
                     "gate closed; cycle complete");
        }
        if (completedId != 0 && completionStored) sendDone(completedId);
      }
      break;

    // FIX 7: a bench-only manual open used to have no time limit, leaving the
    // gate up and the servo holding current indefinitely.
    case DISP_MANUAL_OPEN:
      if (elapsed >= MANUAL_OPEN_TIMEOUT_MS) {
        Serial.println("[SAFETY] Manual open timed out; closing the gate");
        commandGateClose();
        activeRequestId = 0;
        enterState(DISP_GATE_CLOSING, "manual open timeout");
      }
      break;

    case DISP_WAIT_WIFI:
    case DISP_READY:
    case DISP_FAULT:
      break;
  }
}

// ---------------------------------------------------------------------------
// 11. OPTIONAL DIAGNOSTIC WEBSITE
// ---------------------------------------------------------------------------

bool runGateSelfTest(const char *source);

const char DIAGNOSTIC_PAGE[] PROGMEM = R"HTML(
<!doctype html><html><head>
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Medicine Dispenser</title>
<style>body{font-family:Arial;max-width:600px;margin:20px auto;padding:14px}button{padding:12px;margin:5px;font-size:16px}pre{white-space:pre-wrap}</style>
</head><body><h1>Medicine Dispenser</h1><pre id="s">Loading...</pre>
<button onclick="cmd('/drop')">Run drop test</button>
<button onclick="cmd('/gate/test')">Gate self-test</button>
<button onclick="cmd('/gate/open')">Open gate</button>
<button onclick="cmd('/gate/close')">Close gate</button>
<button onclick="cmd('/fault/reset')">Reset inspected fault</button><pre id="r"></pre>
<script>
async function cmd(p){
 if(p=='/drop'&&!confirm('Physically run dispenser?'))return;
 if(p=='/gate/test'&&!confirm('This sweeps the gate servo. Keep hands clear. Continue?'))return;
 let x=await fetch(p,{method:'POST'});document.getElementById('r').textContent=await x.text();}
async function poll(){try{document.getElementById('s').textContent=await (await fetch('/status')).text();}catch(e){}setTimeout(poll,700)}poll();
</script></body></html>
)HTML";

String statusJson() {
  String json;
  json.reserve(720);
  json += "{";
  json += "\"build\":\"" + String(FIRMWARE_BUILD) + "\",";
  json += "\"protocol_version\":" + String(RADIO_VERSION) + ",";
  json += "\"packet_bytes\":" + String((unsigned)RADIO_PACKET_BYTES) + ",";
  json += "\"state\":\"" + String(stateName(state)) + "\",";
  json += "\"fault\":\"" + String(faultReason) + "\",";
  json += "\"gate_open\":" + String(gateOpen ? "true" : "false") + ",";
  json += "\"servo_pwm_ok\":" + String(servoPwmReady ? "true" : "false") + ",";
  json += "\"servo_holding\":" + String(servoHolding ? "true" : "false") + ",";
  json += "\"servo_angle\":" + String(servoTargetAngle) + ",";
  json += "\"servo_pulse_us\":" + String(servoPulseUs) + ",";
  json += "\"active_request_id\":" + String(activeRequestId) + ",";
  json += "\"last_completed_request_id\":" + String(lastCompletedRequestId) + ",";
  json += "\"last_accepted_request_id\":" + String(lastAcceptedRequestId) + ",";
  json += "\"completed_drops\":" + String(completedDropCount) + ",";
  json += "\"interrupted_request\":" + String(interruptedRequestPending ? "true" : "false") + ",";
  json += "\"nvs_ok\":" + String(requestPreferencesReady ? "true" : "false") + ",";
  json += "\"radio_ready\":" + String(radioReady ? "true" : "false") + ",";
  json += "\"radio_rx_last_bytes\":" + String(lastRadioRxLength) + ",";
  json += "\"radio_rx_raw\":" + String(radioRawPacketsSeen) + ",";
  json += "\"radio_rx_valid\":" + String(radioPacketsSeen) + ",";
  json += "\"radio_rx_wrong_length\":" + String(radioWrongLength) + ",";
  json += "\"radio_rx_wrong_protocol\":" + String(radioWrongProtocol) + ",";
  json += "\"radio_tx_last_bytes\":" + String(lastRadioTxLength) + ",";
  json += "\"radio_tx_queued\":" + String(radioPacketsQueued) + ",";
  json += "\"radio_tx_queue_errors\":" + String(radioQueueErrors) + ",";
  json += "\"heartbeats_sent\":" + String(heartbeatsSent) + ",";
  json += "\"ip\":\"" + WiFi.localIP().toString() + "\",";
  json += "\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  json += "}";
  return json;
}

void configureWebServer() {
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", DIAGNOSTIC_PAGE);
  });

  server.on("/status", HTTP_GET, []() {
    server.send(200, "application/json", statusJson());
  });

  server.on("/drop", HTTP_POST, []() {
    if (startLocalDrop(0, "diagnostic webpage")) {
      server.send(202, "text/plain", "Drop test accepted");
    } else {
      server.send(409, "text/plain", "Dispenser is busy, faulted, or servo PWM failed");
    }
  });

  server.on("/gate/test", HTTP_POST, []() {
    if (state != DISP_READY && state != DISP_FAULT) {
      server.send(409, "text/plain", "Self-test refused: a gate cycle is running");
      return;
    }
    if (runGateSelfTest("web page")) {
      server.send(200, "text/plain", "Gate self-test finished; read the Serial Monitor");
    } else {
      server.send(500, "text/plain", "Gate self-test failed; read the Serial Monitor");
    }
  });

  server.on("/gate/open", HTTP_POST, []() {
    if (state != DISP_READY && state != DISP_MANUAL_OPEN) {
      server.send(409, "text/plain", "Gate is busy");
      return;
    }
    if (!servoPwmReady) {
      server.send(500, "text/plain", "Gate servo PWM channel is not attached");
      return;
    }
    commandGateOpen();
    enterState(DISP_MANUAL_OPEN, "manual webpage open");
    server.send(200, "text/plain",
                "Gate opening; it auto-closes after 60 s if left open");
  });

  server.on("/gate/close", HTTP_POST, []() {
    commandGateClose();
    activeRequestId = 0;
    enterState(DISP_GATE_CLOSING, "manual webpage close");
    server.send(200, "text/plain", "Gate closing");
  });

  server.on("/fault/reset", HTTP_POST, []() {
    if (state != DISP_FAULT) {
      server.send(409, "text/plain", "No dispenser fault is active");
      return;
    }
    if (resetInterruptedFault("diagnostic webpage reset after inspection")) {
      server.send(200, "text/plain",
                  "Fault reset. The interrupted request remains blocked; start a new car mission.");
    } else {
      server.send(500, "text/plain",
                  "Fault reset failed: see the Serial Monitor");
    }
  });

  server.onNotFound([]() {
    server.send(404, "text/plain", "Not found");
  });
}

// ---------------------------------------------------------------------------
// 12. WIFI
// ---------------------------------------------------------------------------

void printNetworkDetails() {
  Serial.println("\n[WIFI] CONNECTED");
  Serial.printf("[WIFI] SSID: %s\n", WiFi.SSID().c_str());
  Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
  Serial.printf("[WIFI] Gateway: %s\n", WiFi.gatewayIP().toString().c_str());
  Serial.printf("[WIFI] Channel: %d\n", WiFi.channel());
  Serial.printf("[WIFI] RSSI: %d dBm\n", WiFi.RSSI());
  Serial.printf("[WIFI] STA MAC: %s\n", WiFi.macAddress().c_str());
  Serial.printf("[WEB] Optional diagnostics: http://%s\n\n",
                WiFi.localIP().toString().c_str());
}

void onWiFiConnected() {
  wasWiFiConnected = true;
  printNetworkDetails();
  bool radioInitialized = initializeEspNow();

  // FIX 4: release the previous listening socket before re-binding.
  if (serverStarted) {
    server.stop();
    Serial.println("[WEB] Previous listening socket closed before restart");
  }
  server.begin();
  serverStarted = true;
  Serial.println("[WEB] Diagnostic server listening on port 80");

  MDNS.end();
  if (MDNS.begin("medicine-dispenser")) {
    Serial.println("[MDNS] Try http://medicine-dispenser.local if supported");
  } else {
    Serial.println("[MDNS] Name unavailable; use the printed numeric IP");
  }

  if (state == DISP_WAIT_WIFI && radioInitialized) {
    enterState(DISP_READY, "Wi-Fi and ESP-NOW initialized");
  } else if (state == DISP_WAIT_WIFI) {
    Serial.println("[STATE] Staying in WAIT_WIFI until ESP-NOW initializes");
  }
  sendHeartbeat();
}

void maintainWiFi() {
  bool connected = WiFi.status() == WL_CONNECTED;

  if (connected && !wasWiFiConnected) {
    onWiFiConnected();
    return;
  }

  if (connected && !radioReady &&
      millis() - lastRadioInitAttemptMs >= RADIO_RETRY_MS) {
    Serial.println("[RADIO] Retrying ESP-NOW initialization...");
    if (initializeEspNow() && state == DISP_WAIT_WIFI) {
      enterState(DISP_READY, "ESP-NOW retry succeeded");
      sendHeartbeat();
    }
  }

  if (!connected && wasWiFiConnected) {
    wasWiFiConnected = false;
    radioReady = false;
    Serial.println("[WIFI] CONNECTION LOST; gate cycle remains locally safe");
    if (state == DISP_READY) enterState(DISP_WAIT_WIFI, "hotspot disconnected");
  }

  if (!connected && millis() - lastWiFiRetryMs >= 10000) {
    lastWiFiRetryMs = millis();
    Serial.printf("[WIFI] Retrying hotspot '%s'...\n", WIFI_SSID);
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASS);
  }
}

// ---------------------------------------------------------------------------
// 13. BUTTON, LED, HEARTBEAT
// ---------------------------------------------------------------------------

void serviceStatusLed() {
  static unsigned long lastToggleMs = 0;
  unsigned long interval = state == DISP_READY ? 1000 : 180;

  if (millis() - lastToggleMs >= interval) {
    lastToggleMs = millis();
    statusLedOn = !statusLedOn;
    digitalWrite(STATUS_LED_PIN, statusLedOn ? HIGH : LOW);
  }
}

void serviceOptionalButton() {
  if (!OPTIONAL_BUTTON_ENABLED) return;
  bool reading = digitalRead(OPTIONAL_BUTTON_PIN);

  if (reading != buttonPrevious && millis() - lastButtonChangeMs >= 40) {
    lastButtonChangeMs = millis();
    buttonPrevious = reading;
    if (reading == LOW) {
      Serial.println("[BUTTON] Manual drop pressed");
      startLocalDrop(0, "optional physical button");
    }
  }
}

void serviceHeartbeat() {
  if (radioReady && millis() - lastHeartbeatMs >= 1000) {
    lastHeartbeatMs = millis();
    sendHeartbeat();
  }
}

// ---------------------------------------------------------------------------
// 14. LIVE COMPONENT MONITOR
// ---------------------------------------------------------------------------

void printMonitorHeader() {
  Serial.println();
  Serial.println("[MON DISP] self-describing live row: every fitted output, input, link and protocol counter");
  monitorRowsSinceHeader = 0;
}

void printMonitorRow() {
  bool buttonPressed = OPTIONAL_BUTTON_ENABLED
                         ? (digitalRead(OPTIONAL_BUTTON_PIN) == LOW)
                         : false;

  Serial.printf(
    "[MON DISP] t=%lums state=%s fault=%s wifi=%s rssi=%d ch=%d "
    "radio=%s rxB=%d raw=%lu valid=%lu badLen=%lu badProto=%lu "
    "txB=%d queued=%lu txErr=%lu HB=%lu gateCmd=%s servoPWM=%s signal=%s "
    "angle=%d pulse=%dus LED=%s button=%s/raw-%s nvs=%s "
    "active=%lu accepted=%lu completed=%lu drops=%lu interrupted=%s\n",
    millis(), stateName(state), faultReason,
    WiFi.status() == WL_CONNECTED ? "up" : "down",
    WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0, WiFi.channel(),
    radioReady ? "up" : "down", lastRadioRxLength,
    (unsigned long)radioRawPacketsSeen, (unsigned long)radioPacketsSeen,
    (unsigned long)radioWrongLength, (unsigned long)radioWrongProtocol,
    lastRadioTxLength, (unsigned long)radioPacketsQueued,
    (unsigned long)radioQueueErrors, heartbeatsSent,
    gateOpen ? "OPEN" : "CLOSED", servoPwmReady ? "OK" : "FAIL",
    servoHolding ? "PULSING" : "released", servoTargetAngle, servoPulseUs,
    statusLedOn ? "ON" : "off",
    OPTIONAL_BUTTON_ENABLED ? (buttonPressed ? "DOWN" : "up") : "disabled",
    digitalRead(OPTIONAL_BUTTON_PIN) == LOW ? "LOW" : "HIGH",
    requestPreferencesReady ? "OK" : "FAIL",
    (unsigned long)activeRequestId, (unsigned long)lastAcceptedRequestId,
    (unsigned long)lastCompletedRequestId, (unsigned long)completedDropCount,
    interruptedRequestPending ? "YES" : "no");

  monitorRowsSinceHeader++;
  if (monitorRowsSinceHeader >= 20) printMonitorHeader();
}

void serviceMonitor() {
  if (!monitorEnabled) return;
  if (millis() - lastMonitorMs < monitorIntervalMs) return;
  lastMonitorMs = millis();
  printMonitorRow();
}

void printComponentReport() {
  uint32_t packets;
  portENTER_CRITICAL(&radioMux);
  packets = radioPacketsSeen;
  portEXIT_CRITICAL(&radioMux);

  Serial.println();
  Serial.println("=============== DISPENSER COMPONENT REPORT ===============");
  Serial.printf("FIRMWARE     : %s\n", FIRMWARE_BUILD);
  Serial.printf("PROTOCOL     : magic=0x%08lX version=%u packet=%u bytes (compile checked)\n",
                (unsigned long)RADIO_MAGIC, RADIO_VERSION,
                (unsigned)RADIO_PACKET_BYTES);
  Serial.printf("UPTIME       : %lu ms\n", millis());
  Serial.printf("STATE        : %s   since %lu ms   fault=%s\n",
                stateName(state), millis() - stateSince, faultReason);
  Serial.println("----------------------------------------------------------");

  Serial.printf("GATE SERVO   : PWM=%s  GPIO%d  LEDC ch%d  %d Hz  %d bit\n",
                servoPwmReady ? "OK" : "FAIL", DISPENSER_SERVO_PIN,
                SERVO_LEDC_CHANNEL, SERVO_PWM_FREQUENCY, SERVO_PWM_RESOLUTION);
  Serial.printf("               logical=%s  signal=%s  angle=%d deg  pulse=%d us\n",
                gateOpen ? "OPEN" : "CLOSED",
                servoHolding ? "PULSING" : "released",
                servoTargetAngle, servoPulseUs);
  Serial.printf("               closedAngle=%d (%d us)  openAngle=%d (%d us)  hold=%lu ms  nudge=%d\n",
                GATE_CLOSED_ANGLE, servoAngleToPulseUs(GATE_CLOSED_ANGLE),
                GATE_OPEN_ANGLE, servoAngleToPulseUs(GATE_OPEN_ANGLE),
                GATE_OPEN_HOLD_MS, servoNudgeAngle);
  Serial.println("----------------------------------------------------------");

  Serial.printf("STATUS LED   : GPIO%d  %s\n", STATUS_LED_PIN,
                statusLedOn ? "ON" : "off");
  Serial.printf("TEST BUTTON  : GPIO%d  enabled=%s  level=%s\n",
                OPTIONAL_BUTTON_PIN, OPTIONAL_BUTTON_ENABLED ? "YES" : "no",
                digitalRead(OPTIONAL_BUTTON_PIN) == LOW ? "LOW/pressed" : "HIGH/idle");
  Serial.println("----------------------------------------------------------");

  Serial.printf("WIFI         : %s  ssid=%s  ip=%s  rssi=%d  ch=%d\n",
                WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
                WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(),
                WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0,
                WiFi.channel());
  Serial.printf("WEB SERVER   : %s\n", serverStarted ? "LISTENING :80" : "not started");
  Serial.printf("ESP-NOW RX   : %s lastBytes=%d raw=%lu valid=%lu wrongLength=%lu wrongProtocol=%lu\n",
                radioReady ? "UP" : "DOWN", lastRadioRxLength,
                (unsigned long)radioRawPacketsSeen, (unsigned long)packets,
                (unsigned long)radioWrongLength,
                (unsigned long)radioWrongProtocol);
  Serial.printf("ESP-NOW TX   : lastBytes=%d queued=%lu queueErrors=%lu heartbeats=%lu\n",
                lastRadioTxLength, (unsigned long)radioPacketsQueued,
                (unsigned long)radioQueueErrors, heartbeatsSent);
  Serial.println("----------------------------------------------------------");

  Serial.printf("NVS          : %s  namespace=meddisp\n",
                requestPreferencesReady ? "OK" : "FAIL");
  Serial.printf("REQUEST IDs  : active=%lu  accepted=%lu  completed=%lu\n",
                (unsigned long)activeRequestId,
                (unsigned long)lastAcceptedRequestId,
                (unsigned long)lastCompletedRequestId);
  Serial.printf("DUPLICATES   : interruptedPending=%s  completedDropCount=%lu\n",
                interruptedRequestPending ? "YES" : "no",
                (unsigned long)completedDropCount);
  Serial.println("==========================================================\n");
}

// ---------------------------------------------------------------------------
// 15. GATE SELF-TEST AND CALIBRATION HELPERS
// ---------------------------------------------------------------------------

void reportServoStep(const char *label, int angle, unsigned long holdMs) {
  servoWriteAngle(angle);
  unsigned long started = millis();
  while (millis() - started < holdMs) {
    delay(20);
  }
  Serial.printf("[GATE TEST] %-10s angle=%3d  pulse=%4d us  signal=%s\n",
                label, angle, servoPulseUs, servoHolding ? "PULSING" : "OFF");
}

bool runGateSelfTest(const char *source) {
  Serial.println();
  Serial.println("============ GATE SERVO SELF-TEST ============");
  Serial.printf("[GATE TEST] Requested by: %s\n", source ? source : "?");

  if (!servoPwmReady) {
    Serial.println("[GATE TEST] FAIL: LEDC channel was never attached.");
    Serial.printf("[GATE TEST] Check that GPIO%d is free and that channel %d\n",
                  DISPENSER_SERVO_PIN, SERVO_LEDC_CHANNEL);
    Serial.println("[GATE TEST] is not used by anything else in this sketch.");
    Serial.println("==============================================\n");
    return false;
  }

  Serial.println("[GATE TEST] Keep hands clear. Four positions, 900 ms each.");
  Serial.println("[GATE TEST] If nothing moves, check in this order:");
  Serial.println("[GATE TEST]   1. servo signal wire on GPIO18, not 3.3V or GND");
  Serial.println("[GATE TEST]   2. servo 5V from the external regulated supply");
  Serial.println("[GATE TEST]   3. servo GND joined to the ESP32 ground");
  Serial.println("[GATE TEST]   4. horn not jammed against a mechanical stop");

  int midAngle = (GATE_CLOSED_ANGLE + GATE_OPEN_ANGLE) / 2;
  reportServoStep("CLOSED", GATE_CLOSED_ANGLE, 900);
  reportServoStep("MIDPOINT", midAngle, 900);
  reportServoStep("OPEN", GATE_OPEN_ANGLE, 900);
  reportServoStep("CLOSED", GATE_CLOSED_ANGLE, 900);

  gateOpen = false;
  servoRelease();
  Serial.println("[GATE TEST] Sequence complete; servo released.");
  Serial.println("==============================================\n");
  return true;
}

void nudgeServo(int delta) {
  if (state != DISP_READY && state != DISP_FAULT && state != DISP_MANUAL_OPEN) {
    Serial.println("[GATE] Nudge refused while a gate cycle is running");
    return;
  }
  if (!servoPwmReady) {
    Serial.println("[GATE] Nudge refused: LEDC channel not attached");
    return;
  }
  servoNudgeAngle = constrain(servoNudgeAngle + delta, 0, 180);
  servoWriteAngle(servoNudgeAngle);
  Serial.printf("[GATE] Nudge angle now %d deg (%d us). When the gate is in\n",
                servoNudgeAngle, servoPulseUs);
  Serial.println("[GATE] the position you want, copy this number into");
  Serial.println("[GATE] GATE_CLOSED_ANGLE or GATE_OPEN_ANGLE.");
}

// ---------------------------------------------------------------------------
// 16. SERIAL MONITOR COMMANDS
// ---------------------------------------------------------------------------

void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  Serial.printf("[BOOT] ESP reset reason code: %d", (int)reason);
  if (reason == ESP_RST_BROWNOUT) Serial.print(" (BROWNOUT - CHECK 5V/SERVO POWER)");
  if (reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT) Serial.print(" (WATCHDOG)");
  Serial.println();
}

void printSerialHelp() {
  Serial.println("\n========= DISPENSER SERIAL COMMANDS =========");
  Serial.println("h  help");
  Serial.println("p  full vertical component report");
  Serial.println("i  toggle the live side-by-side component monitor");
  Serial.println("+  faster monitor   -  slower monitor");
  Serial.println("w  print Wi-Fi details");
  Serial.println("-- gate --");
  Serial.println("g  gate servo self-test (sweeps closed/mid/open/closed)");
  Serial.println("d  run one full local drop cycle");
  Serial.println("o  manually open gate (auto-closes after 60 s)");
  Serial.println("c  manually close gate    x  emergency close gate");
  Serial.println("n  release servo (stop the pulse train)");
  Serial.println("[  nudge servo -5 deg   ]  nudge servo +5 deg");
  Serial.println("-- faults --");
  Serial.println("r  reset an inspected interrupted-request fault");
  Serial.println("=============================================\n");
}

void handleSerial() {
  while (Serial.available()) {
    char command = tolower(Serial.read());
    if (command == '\n' || command == '\r' || command == ' ') continue;

    switch (command) {
      case 'h': printSerialHelp(); break;
      case 'p': printComponentReport(); break;
      case 'i':
        monitorEnabled = !monitorEnabled;
        Serial.printf("[MONITOR] Live component monitor: %s\n",
                      monitorEnabled ? "ON" : "OFF");
        if (monitorEnabled) printMonitorHeader();
        break;
      case '+':
        if (monitorIntervalMs > 100) monitorIntervalMs -= 100;
        Serial.printf("[MONITOR] Interval now %lu ms\n", monitorIntervalMs);
        break;
      case '-':
        if (monitorIntervalMs < 2000) monitorIntervalMs += 100;
        Serial.printf("[MONITOR] Interval now %lu ms\n", monitorIntervalMs);
        break;
      case 'w': printNetworkDetails(); break;

      case 'g':
        if (state == DISP_READY || state == DISP_FAULT) {
          runGateSelfTest("Serial Monitor");
        } else {
          Serial.printf("[GATE TEST] Refused in state %s\n", stateName(state));
        }
        break;
      case 'd': startLocalDrop(0, "Serial Monitor drop test"); break;
      case 'o':
        if (state == DISP_READY || state == DISP_MANUAL_OPEN) {
          commandGateOpen();
          enterState(DISP_MANUAL_OPEN, "Serial manual open");
        } else {
          Serial.printf("[SERVO] Open refused: state=%s\n", stateName(state));
        }
        break;
      case 'c':
      case 'x':
        commandGateClose();
        activeRequestId = 0;
        enterState(DISP_GATE_CLOSING,
                   command == 'x' ? "Serial emergency close" : "Serial close");
        break;
      case 'n':
        if (state == DISP_READY || state == DISP_FAULT) servoRelease();
        else Serial.println("[SERVO] Release refused while a cycle is running");
        break;
      case '[': nudgeServo(-5); break;
      case ']': nudgeServo(5); break;

      case 'r': resetInterruptedFault("Serial reset after operator inspection"); break;

      default:
        Serial.printf("[SERIAL] Unknown command '%c'. Type h.\n", command);
        break;
    }
  }
}

// ---------------------------------------------------------------------------
// 17. SETUP AND LOOP
// ---------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n\n====================================================");
  Serial.println(" SMART HOSPITAL ROBOT - DISPENSER BOOT");
  Serial.println("====================================================");
  Serial.printf("[PROTO] build=%s magic=0x%08lX version=%u packet=%u bytes\n",
                FIRMWARE_BUILD, (unsigned long)RADIO_MAGIC, RADIO_VERSION,
                (unsigned)RADIO_PACKET_BYTES);
  printResetReason();

  pinMode(STATUS_LED_PIN, OUTPUT);
  digitalWrite(STATUS_LED_PIN, LOW);
  pinMode(OPTIONAL_BUTTON_PIN, INPUT_PULLUP);

  initServoPwm();

  // Establish a safe known gate position. Disconnect the horn on first test.
  servoWriteAngle(GATE_CLOSED_ANGLE);
  gateOpen = false;
  servoNudgeAngle = GATE_CLOSED_ANGLE;
  delay(SERVO_TRAVEL_MS);
  servoRelease();
  Serial.printf("[BOOT] Gate set CLOSED at %d degrees (%d us)\n",
                GATE_CLOSED_ANGLE, servoAngleToPulseUs(GATE_CLOSED_ANGLE));
  Serial.printf("[BOOT] Open angle=%d (%d us), hold=%lums, button enabled=%s\n",
                GATE_OPEN_ANGLE, servoAngleToPulseUs(GATE_OPEN_ANGLE),
                GATE_OPEN_HOLD_MS, OPTIONAL_BUTTON_ENABLED ? "YES" : "NO");

  requestPreferencesReady = requestPreferences.begin("meddisp", false);
  if (requestPreferencesReady) {
    lastAcceptedRequestId = requestPreferences.getUInt("accepted", 0);
    lastCompletedRequestId = requestPreferences.getUInt("completed", 0);
    interruptedRequestPending = requestPreferences.getBool("pending", false);
  } else {
    lastAcceptedRequestId = 0;
    lastCompletedRequestId = 0;
    interruptedRequestPending = true;
    Serial.println("[NVS] ERROR: Preferences initialization failed");
  }
  Serial.printf("[RECOVERY] accepted=%lu completed=%lu pending=%s\n",
                (unsigned long)lastAcceptedRequestId,
                (unsigned long)lastCompletedRequestId,
                interruptedRequestPending ? "YES" : "NO");

  if (!servoPwmReady) {
    setDispenserFault("SERVO PWM FAILED");
  } else if (!requestPreferencesReady) {
    setDispenserFault("NVS INIT FAILED");
  } else if (interruptedRequestPending) {
    setDispenserFault("DROP INTERRUPTED");
    Serial.println("[RECOVERY] Power reset during a drop; inspect medicine path");
  }

  configureWebServer();

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  Serial.printf("[WIFI] Connecting by DHCP to hotspot '%s'", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long connectStarted = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - connectStarted < 20000) {
    Serial.print('.');
    delay(400);
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    onWiFiConnected();
  } else {
    Serial.println("[WIFI] Initial connection failed. Keep hotspot ON and verify credentials.");
    if (state != DISP_FAULT) {
      enterState(DISP_WAIT_WIFI, "initial hotspot timeout; automatic retry active");
    }
  }

  printSerialHelp();
  printComponentReport();
  printMonitorHeader();
}

void loop() {
  maintainWiFi();
  if (WiFi.status() == WL_CONNECTED) server.handleClient();
  processRadioRequests();
  serviceDispenserCycle();
  serviceOptionalButton();
  serviceHeartbeat();
  serviceStatusLed();
  handleSerial();
  serviceMonitor();
  delay(1);
}
