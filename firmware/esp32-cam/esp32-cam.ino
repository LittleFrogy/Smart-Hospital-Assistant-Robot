/* ============================================================================
   Smart Hospital Assistant Robot  --  ESP32-CAM firmware
   Board: AI-Thinker ESP32-CAM   |   Arduino core for ESP32
   ----------------------------------------------------------------------------
   WHAT THIS SERVES

     http://<cam-ip>/            phone-friendly live view page
     http://robotcam.local/      same page, no IP needed (mDNS)
     http://<cam-ip>/capture     one still JPEG   (what the PC decoder pulls)
     http://<cam-ip>/status      JSON diagnostics
     http://<cam-ip>/flash?on=1  on-board white LED, on=0 to turn it off
     http://<cam-ip>:81/stream   raw MJPEG stream (the page embeds this)

   The camera does NO QR decoding. The PC pulls /capture and decodes with
   pyzbar. This board only takes pictures.

   ----------------------------------------------------------------------------
   WHAT YOU MUST CHANGE -- section 1 only, nothing else in this file

     WIFI_SSID and WIFI_PASS  ->  your Wi-Fi name and password

   That is it. Everything else has a working default.

   NETWORK

     Everything runs on the home router. Each board takes a fixed last octet:

        router (gateway)  192.168.0.1
        car ESP32         192.168.0.51
        dispenser ESP32   192.168.0.52
        this camera       192.168.0.53

     Run ipconfig on the laptop and check Default Gateway matches 192.168.0.1.
     If your router uses a different subnet, change the third octet here and in
     car.ino, dispenser.ino and pc/app.py.

     The network must be 2.4 GHz. The ESP32 cannot see a 5 GHz network at all.
     Most home routers broadcast both bands under one name, which is fine.

     The board also answers to http://robotcam.local/ regardless of address.

     On another network later, only the third octet changes -- a Windows laptop
     hotspot is always 192.168.137.x with the laptop at .1. A PHONE hotspot
     picks its own subnet, so set USE_STATIC_IP to false there instead.
     If a static address fails, this sketch retries on DHCP by itself.

   ----------------------------------------------------------------------------
   FLASHING (the board has NO USB port)

     CP2102 USB-TTL adapter, jumper set to 5V:

        CAM 5V   -> adapter 5V
        CAM GND  -> adapter GND
        CAM U0T  -> adapter RXD
        CAM U0R  -> adapter TXD
        CAM IO0  -> CAM GND     (ONLY while flashing -- REMOVE afterwards)

     Board:  "AI Thinker ESP32-CAM"      Upload speed: 115200
     Hold RST, press Upload, release RST when "Connecting..." appears.
     REMOVE the IO0->GND jumper when done, then press RST to run.

   ----------------------------------------------------------------------------
   HARDWARE NOTES

     - Runtime wiring is 5V and GND only. Everything else stays unconnected.
     - GPIO16 must NOT be connected (PSRAM chip select).
     - A 470uF capacitor across 5V/GND within 20mm of the module is mandatory,
       or the board brown-outs and reboot-loops when Wi-Fi transmits.
     - GPIO4 is the on-board white flash LED. It is extremely bright and gets
       hot. Only pulse it; never leave it on.
     - Mount rigidly, facing the QR codes, slightly downward. Motion blur is
       the top cause of decode failures -- the car must be fully stopped.
   ============================================================================ */

#include "esp_camera.h"
#include "img_converters.h"     // frame2jpg(), for sensors with no JPEG encoder
#include <WiFi.h>
#include <ESPmDNS.h>
#include "esp_http_server.h"

/* ---------- 1. USER CONFIG -- THE ONLY SECTION YOU EDIT -------------------- */
const char* WIFI_SSID = "";      // TODO: your Wi-Fi name  (2.4 GHz)
const char* WIFI_PASS = "";      // TODO: your Wi-Fi password

// Reachable as http://robotcam.local/ so you never have to type the IP.
const char* MDNS_NAME = "robotcam";

// Set for the home router (192.168.0.x, gateway 192.168.0.1). The last octet
// is the only thing that identifies this board: car .51, dispenser .52,
// cam .53.
//
// Moving to a different network later? Only the third octet changes:
//    Windows laptop hotspot    192.168.137.x   (always, gateway .1)
//    phone hotspot             varies -- set USE_STATIC_IP false instead
//
// If the static address fails, this sketch retries on DHCP by itself and says
// so on serial, so a wrong subnet is never a silent dead board.
const bool USE_STATIC_IP = true;

IPAddress CAM_IP  (192, 168, 0, 53);
IPAddress GATEWAY (192, 168, 0,  1);   // the router
IPAddress SUBNET  (255, 255, 255, 0);
IPAddress DNS1    (192, 168, 0,  1);

/* ---------- 2. IMAGE TUNING ------------------------------------------------ */

// ---- NO DECOUPLING CAPACITOR FITTED YET? LEAVE THIS TRUE. ----
// The module idles around 180 mA and spikes past 310 mA in microseconds when
// the Wi-Fi radio transmits. Without a 470uF capacitor across 5V/GND to supply
// that spike locally, the rail sags and the board resets -- which looks exactly
// like a dead board.
//
// This flag cuts the spike at the source instead: half the pixel clock, a
// smaller frame, and much lower Wi-Fi transmit power. It is enough to run
// reliably off a CP2102's USB 5V with no capacitor at all.
//
// Cost: 400x296 instead of 640x480, so QR codes must be closer or larger.
// Set it to false once you have the capacitor -- QR range depends on the
// full frame size.
const bool LOW_POWER_BENCH = true;

// VGA (640x480) is the sweet spot for QR at 15-25 cm. Larger is slower and no
// more accurate. Ignored while LOW_POWER_BENCH is true.
const framesize_t FRAME_SIZE = FRAMESIZE_VGA;

// Set these once the camera is bolted to the car and you can see which way up
// the picture comes out. 1 = flip, 0 = normal.
const int CAM_VFLIP   = 0;
const int CAM_HMIRROR = 0;

// Grayscale strips colour noise and can help QR decoding in a badly lit ward.
// It also makes the nurse's live view grey, so it is off by default.
const bool QR_GRAYSCALE = false;

/* ---------- 3. AI-THINKER PIN MAP  (do not change for this board) ---------- */
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define FLASH_GPIO_NUM     4     // on-board white LED

/* ---------- 4. RUNTIME STATE ----------------------------------------------- */
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

volatile uint32_t framesServed = 0;
volatile uint32_t grabFailures = 0;
bool hasPsram = false;
framesize_t activeFrameSize = FRAMESIZE_VGA;   // what the sensor actually runs at

// True when the sensor has a hardware JPEG encoder (OV2640 and friends).
// False means we captured RGB565 and must encode each frame in software.
bool jpegNative = true;

// Sensor PIDs the esp32-camera driver knows about. Only the OV-series parts
// with a JPEG block are usable at full speed; the rest need software encoding.
const char* sensorName(uint16_t pid) {
  switch (pid) {
    case 0x26:   return "OV2640  (JPEG in hardware -- the right part)";
    case 0x36:   return "OV3660  (JPEG in hardware)";
    case 0x56:   return "OV5640  (JPEG in hardware)";
    case 0x76:   return "OV7670  (NO JPEG -- software encoding)";
    case 0x77:   return "OV7725  (NO JPEG -- software encoding)";
    case 0x9b:   return "GC0308  (NO JPEG -- software encoding)";
    case 0x232a: return "GC032A  (NO JPEG -- software encoding)";
    default:     return "unknown";
  }
}

const char* frameSizeName(framesize_t f) {
  switch (f) {
    case FRAMESIZE_QQVGA: return "160x120";
    case FRAMESIZE_QVGA:  return "320x240";
    case FRAMESIZE_CIF:   return "400x296";
    case FRAMESIZE_VGA:   return "640x480";
    case FRAMESIZE_SVGA:  return "800x600";
    case FRAMESIZE_XGA:   return "1024x768";
    case FRAMESIZE_SXGA:  return "1280x1024";
    case FRAMESIZE_UXGA:  return "1600x1200";
    default:              return "?";
  }
}

/* ---------- 5. THE LIVE VIEW PAGE  (served at / on port 80) ---------------- */
/* Phones are the target here, so: no external files, no libraries, big touch
   targets, and a snapshot fallback because some iOS builds refuse to keep an
   MJPEG <img> alive. */
static const char PAGE_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0f172a">
<title>Robot Camera</title>
<style>
*{box-sizing:border-box}
body{margin:0;background:#0f172a;color:#e2e8f0;
     font:15px/1.45 system-ui,-apple-system,Segoe UI,Roboto,Arial,sans-serif}
header{padding:14px 16px 10px}
h1{margin:0;font-size:17px;font-weight:650}
.sub{color:#94a3b8;font-size:13px;margin-top:2px}
#wrap{position:relative;background:#000;margin:0 auto;max-width:900px;overflow:hidden}
#v{display:block;width:100%;height:auto;transform-origin:center center}
#tag{position:absolute;left:10px;top:10px;background:rgba(15,23,42,.72);
     border:1px solid #334155;border-radius:999px;padding:4px 11px;font-size:12px;
     color:#cbd5e1}
#dot{display:inline-block;width:7px;height:7px;border-radius:50%;
     background:#ef4444;margin-right:6px;vertical-align:middle}
#dot.live{background:#22c55e}
.bar{display:flex;flex-wrap:wrap;gap:8px;padding:12px 16px;max-width:900px;margin:0 auto}
button{flex:1 1 auto;min-width:104px;min-height:46px;font:inherit;font-size:14px;
       border-radius:11px;border:1px solid #475569;background:#1e293b;color:#e2e8f0;
       cursor:pointer;-webkit-tap-highlight-color:transparent}
button:active{background:#334155}
button.on{background:#2563eb;border-color:#2563eb;color:#fff}
table{width:100%;max-width:900px;margin:4px auto 28px;padding:0 16px;
      border-collapse:collapse;font-size:13px}
td{padding:5px 0;border-bottom:1px solid #1e293b}
td:first-child{color:#94a3b8;width:46%}
.bad{color:#f87171}
</style></head><body>

<header>
  <h1>Smart Hospital Assistant Robot</h1>
  <div class="sub">Camera live view</div>
</header>

<div id="wrap">
  <img id="v" alt="camera">
  <div id="tag"><span id="dot"></span><span id="lbl">connecting</span></div>
</div>

<div class="bar">
  <button id="bMode" onclick="toggleMode()">Snapshot mode</button>
  <button id="bPlay" onclick="togglePlay()">Pause</button>
  <button id="bFlash" onclick="flash()">Flash</button>
  <button id="bRot" onclick="rotate()">Rotate</button>
</div>

<table id="stats"></table>

<script>
var host = location.hostname;
var streaming = true;   // true = MJPEG on :81, false = poll /capture
var playing   = true;
var rot = 0;
var n = 0;
// Frame rate is measured server-side, from the delta in /status frames. An
// MJPEG <img> only fires onload once when the stream opens, so counting load
// events here would read zero in stream mode.
var lastFrames = null, lastStatT = 0, fps = '--';
var v = document.getElementById('v');

function setLabel(t, live){
  document.getElementById('lbl').textContent = t;
  document.getElementById('dot').className = live ? 'live' : '';
}

function start(){
  if(!playing){ v.removeAttribute('src'); setLabel('paused', false); return; }
  if(streaming){
    setLabel('stream', true);
    v.src = 'http://' + host + ':81/stream?t=' + Date.now();
  } else {
    setLabel('snapshot', true);
    shot();
  }
}

function shot(){
  if(!playing || streaming) return;
  v.src = '/capture?n=' + (n++);
}

v.onload = function(){
  if(!streaming) setTimeout(shot, 80);
};
v.onerror = function(){
  if(!playing) return;
  setLabel('reconnecting', false);
  setTimeout(start, 900);
};

function toggleMode(){
  streaming = !streaming;
  document.getElementById('bMode').textContent =
    streaming ? 'Snapshot mode' : 'Stream mode';
  v.removeAttribute('src');
  setTimeout(start, 150);
}

function togglePlay(){
  playing = !playing;
  document.getElementById('bPlay').textContent = playing ? 'Pause' : 'Resume';
  document.getElementById('bPlay').className   = playing ? '' : 'on';
  start();
}

function rotate(){
  rot = (rot + 90) % 360;
  v.style.transform = 'rotate(' + rot + 'deg)';
  v.style.padding = (rot % 180) ? '18% 0' : '0';
}

function flash(){
  var b = document.getElementById('bFlash');
  b.className = 'on';
  fetch('/flash?on=1');
  setTimeout(function(){ fetch('/flash?on=0'); b.className = ''; }, 900);
}

function stats(){
  fetch('/status').then(function(r){ return r.json(); }).then(function(s){
    var now = Date.now();
    if(lastFrames !== null && now > lastStatT){
      fps = ((s.frames - lastFrames) * 1000 / (now - lastStatT)).toFixed(1);
    }
    lastFrames = s.frames; lastStatT = now;

    document.getElementById('stats').innerHTML =
      row('Resolution', s.framesize) +
      row('JPEG', s.jpeghw ? 'hardware'
                           : '<span class="bad">software (slow sensor)</span>') +
      row('PSRAM', s.psram ? 'yes' : '<span class="bad">NOT DETECTED</span>') +
      row('Signal', s.rssi + ' dBm' + quality(s.rssi)) +
      row('Free heap', (s.heap/1024).toFixed(0) + ' kB') +
      row('Frames served', s.frames) +
      row('Grab failures', s.fails ? '<span class="bad">'+s.fails+'</span>' : '0') +
      row('Frame rate', fps + ' fps') +
      row('Uptime', s.uptime + ' s');
  }).catch(function(){});
}
function row(a,b){ return '<tr><td>'+a+'</td><td>'+b+'</td></tr>'; }
function quality(r){
  if(r > -60) return '  (strong)';
  if(r > -70) return '  (ok)';
  return '  (weak - move closer to the router)';
}

start(); stats(); setInterval(stats, 2000);
</script>
</body></html>)HTML";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

/* ---------- 5b. one JPEG frame, however this sensor can manage it ---------- */
/* On an OV2640 the frame buffer already holds JPEG and we hand it straight
   out. On a sensor with no JPEG block we captured RGB565, so frame2jpg()
   compresses it on the CPU into a buffer we then own and must free.
   Every successful grabJpeg() must be paired with a releaseJpeg(). */
static bool grabJpeg(camera_fb_t **fbOut, uint8_t **buf, size_t *len, bool *owned) {
  *fbOut = NULL; *buf = NULL; *len = 0; *owned = false;

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) return false;
  *fbOut = fb;

  if (jpegNative) {
    *buf = fb->buf;
    *len = fb->len;
    return true;
  }

  *owned = true;                       // frame2jpg mallocs; we free it later
  return frame2jpg(fb, 80, buf, len);
}

static void releaseJpeg(camera_fb_t *fb, uint8_t *buf, bool owned) {
  if (owned && buf) free(buf);
  if (fb) esp_camera_fb_return(fb);
}

/* ---------- 6. /capture  --  single JPEG ----------------------------------- */
/* Add ?flash=1 to pulse the white LED for this one frame. */
static esp_err_t capture_handler(httpd_req_t *req) {
  bool useFlash = false;
  char query[48];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    char val[8];
    if (httpd_query_key_value(query, "flash", val, sizeof(val)) == ESP_OK)
      useFlash = (val[0] == '1');
  }

  if (useFlash) {
    digitalWrite(FLASH_GPIO_NUM, HIGH);
    delay(120);                                  // let auto-exposure settle
    camera_fb_t *warm = esp_camera_fb_get();     // throw away the dark frame
    if (warm) esp_camera_fb_return(warm);
  }

  camera_fb_t *fb = NULL;
  uint8_t *jpg = NULL;
  size_t jpgLen = 0;
  bool owned = false;
  bool ok = grabJpeg(&fb, &jpg, &jpgLen, &owned);

  if (useFlash) digitalWrite(FLASH_GPIO_NUM, LOW);

  if (!ok) {
    grabFailures++;
    releaseJpeg(fb, jpg, owned);
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t res = httpd_resp_send(req, (const char *)jpg, jpgLen);
  releaseJpeg(fb, jpg, owned);
  framesServed++;
  return res;
}

/* ---------- 7. /status  --  JSON diagnostics ------------------------------- */
static esp_err_t status_handler(httpd_req_t *req) {
  char json[256];
  snprintf(json, sizeof(json),
    "{\"framesize\":\"%s\",\"jpeghw\":%s,\"lowpower\":%s,\"psram\":%s,"
    "\"heap\":%u,\"rssi\":%d,"
    "\"frames\":%u,\"fails\":%u,\"uptime\":%lu,\"ip\":\"%s\"}",
    frameSizeName(activeFrameSize),
    jpegNative ? "true" : "false",
    LOW_POWER_BENCH ? "true" : "false",
    hasPsram ? "true" : "false",
    (unsigned)ESP.getFreeHeap(),
    (int)WiFi.RSSI(),
    (unsigned)framesServed,
    (unsigned)grabFailures,
    (unsigned long)(millis() / 1000),
    WiFi.localIP().toString().c_str());

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

/* ---------- 8. /flash  --  on-board white LED ------------------------------ */
static esp_err_t flash_handler(httpd_req_t *req) {
  char query[32], val[8];
  bool on = false;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "on", val, sizeof(val)) == ESP_OK)
    on = (val[0] == '1');

  digitalWrite(FLASH_GPIO_NUM, on ? HIGH : LOW);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, on ? "on" : "off", HTTPD_RESP_USE_STRLEN);
}

/* ---------- 9. /stream  --  MJPEG on port 81 ------------------------------- */
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* STREAM_CONTENT_TYPE =
  "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res = ESP_OK;
  char part_buf[64];

  res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  if (res != ESP_OK) return res;
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Framerate", "30");

  while (true) {
    uint8_t *jpg = NULL;
    size_t jpgLen = 0;
    bool owned = false;
    fb = NULL;

    if (!grabJpeg(&fb, &jpg, &jpgLen, &owned)) {
      grabFailures++;
      releaseJpeg(fb, jpg, owned);
      res = ESP_FAIL;
      break;
    }

    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, jpgLen);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)jpg, jpgLen);

    releaseJpeg(fb, jpg, owned);
    if (res != ESP_OK) break;     // client closed the connection
    framesServed++;
  }
  return res;
}

/* ---------- 10. start the two HTTP servers --------------------------------- */
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port      = 80;
  config.ctrl_port        = 32768;
  config.max_uri_handlers = 8;
  config.lru_purge_enable = true;   // drop the oldest socket instead of refusing

  httpd_uri_t index_uri = {};
  index_uri.uri = "/";        index_uri.method = HTTP_GET;
  index_uri.handler = index_handler;

  httpd_uri_t capture_uri = {};
  capture_uri.uri = "/capture"; capture_uri.method = HTTP_GET;
  capture_uri.handler = capture_handler;

  httpd_uri_t status_uri = {};
  status_uri.uri = "/status";  status_uri.method = HTTP_GET;
  status_uri.handler = status_handler;

  httpd_uri_t flash_uri = {};
  flash_uri.uri = "/flash";    flash_uri.method = HTTP_GET;
  flash_uri.handler = flash_handler;

  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &flash_uri);
  } else {
    Serial.println("ERROR: port 80 server failed to start");
  }

  config.server_port = 81;
  config.ctrl_port   = 32769;
  httpd_uri_t stream_uri = {};
  stream_uri.uri = "/stream";  stream_uri.method = HTTP_GET;
  stream_uri.handler = stream_handler;

  if (httpd_start(&stream_httpd, &config) == ESP_OK)
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  else
    Serial.println("ERROR: port 81 stream server failed to start");
}

/* ---------- 11. Wi-Fi ------------------------------------------------------- */
bool connectWiFi(bool useStatic) {
  WiFi.disconnect(true);
  delay(120);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);          // power save adds seconds of latency to HTTP

  // Transmit power is the single biggest current spike on this board. 11 dBm
  // is ample at normal indoor range and roughly halves the peak draw,
  // which is what keeps an un-decoupled module from browning out.
  if (LOW_POWER_BENCH) WiFi.setTxPower(WIFI_POWER_11dBm);

  if (useStatic) {
    if (!WiFi.config(CAM_IP, GATEWAY, SUBNET, DNS1))
      Serial.println("  WiFi.config() rejected those addresses");
  } else {
    // 0.0.0.0 across the board puts the stack back on DHCP
    WiFi.config(IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0), IPAddress(0, 0, 0, 0));
  }

  Serial.printf("  connecting to \"%s\" (%s)", WIFI_SSID, useStatic ? "static" : "DHCP");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

/* ---------- 12. SETUP ------------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(400);
  Serial.println("\n\n=== Smart Hospital Assistant Robot -- ESP32-CAM ===");

  pinMode(FLASH_GPIO_NUM, OUTPUT);
  digitalWrite(FLASH_GPIO_NUM, LOW);

  /* --- camera --- */
  hasPsram = psramFound();
  Serial.printf("PSRAM: %s\n", hasPsram ? "found" : "NOT FOUND");

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;   config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;   config.pin_reset = RESET_GPIO_NUM;
  // A 10 MHz pixel clock roughly halves the sensor's current draw. It costs
  // frame rate, which does not matter for reading a stationary QR code.
  config.xclk_freq_hz = LOW_POWER_BENCH ? 10000000 : 20000000;
  config.pixel_format = PIXFORMAT_JPEG;

  if (hasPsram) {
    config.frame_size   = LOW_POWER_BENCH ? FRAMESIZE_CIF : FRAME_SIZE;
    config.jpeg_quality = LOW_POWER_BENCH ? 14 : 10;   // higher = smaller file
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
    config.fb_location  = CAMERA_FB_IN_PSRAM;
  } else {
    // Without PSRAM the frame buffer lives in the ~30 kB of spare DRAM, so the
    // frame must get SMALLER, not larger. VGA will not fit.
    config.frame_size   = FRAMESIZE_QVGA;   // 320x240
    config.jpeg_quality = 12;
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    config.fb_location  = CAMERA_FB_IN_DRAM;
    Serial.println("  -> falling back to QVGA in DRAM. QR range will be short.");
  }

  esp_err_t err = esp_camera_init(&config);

  // 0x106 = ESP_ERR_NOT_SUPPORTED. The driver read the sensor ID fine, but this
  // part has no hardware JPEG encoder (OV7670, OV7725, GC0308...). Capture
  // RGB565 instead and encode each frame in software with frame2jpg().
  // RGB565 is 2 bytes per pixel and uncompressed, so the frame has to shrink.
  if (err == ESP_ERR_NOT_SUPPORTED) {
    Serial.println("\n  This sensor has no JPEG encoder.");
    Serial.println("  Falling back to RGB565 + software JPEG. Slower, but it works.");
    esp_camera_deinit();
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size   = FRAMESIZE_QVGA;   // 320x240 x2 bytes = 150 kB a frame
    config.fb_count     = 1;
    config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
    jpegNative = false;
    err = esp_camera_init(&config);
  }

  if (err != ESP_OK) {
    Serial.printf("Camera init FAILED 0x%x\n", err);
    Serial.println("  0x105 = not detected: reseat the ribbon cable, check 5V.");
    Serial.println("  0x101 = out of memory: lower FRAME_SIZE.");
    Serial.println("  0x106 = sensor cannot do the requested format.");
    Serial.println("  Restarting in 3 s...");
    delay(3000);
    ESP.restart();
  }
  activeFrameSize = config.frame_size;
  Serial.printf("Camera OK at %s, JPEG %s\n", frameSizeName(activeFrameSize),
                jpegNative ? "in hardware" : "in SOFTWARE (slower)");
  if (LOW_POWER_BENCH)
    Serial.println("  LOW_POWER_BENCH is ON (no capacitor fitted).\n"
                   "  Reduced frame, clock and Wi-Fi power. Turn it off in\n"
                   "  section 2 once you have the 470uF cap, for full QR range.");

  /* --- identify the sensor and tune it for crisp QR edges --- */
  // Every setter below is a function pointer that non-OV2640 parts may leave
  // null. Calling one of those would crash the board, hence the guards.
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    Serial.printf("Sensor: %s  (PID 0x%02x)\n",
                  sensorName(s->id.PID), s->id.PID);
    if (s->set_brightness)     s->set_brightness(s, 1);
    if (s->set_contrast)       s->set_contrast(s, 2);   // hard B/W transitions
    if (s->set_saturation)     s->set_saturation(s, 0);
    if (s->set_whitebal)       s->set_whitebal(s, 1);
    if (s->set_gain_ctrl)      s->set_gain_ctrl(s, 1);
    if (s->set_exposure_ctrl)  s->set_exposure_ctrl(s, 1);
    if (s->set_lenc)           s->set_lenc(s, 1);       // lens shading
    if (s->set_vflip)          s->set_vflip(s, CAM_VFLIP);
    if (s->set_hmirror)        s->set_hmirror(s, CAM_HMIRROR);
    if (QR_GRAYSCALE && s->set_special_effect) s->set_special_effect(s, 2);
  } else {
    Serial.println("Sensor handle is null -- cannot read the sensor ID.");
  }

  // The first few frames after init are dark while auto-exposure converges.
  for (int i = 0; i < 4; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) esp_camera_fb_return(fb);
  }

  /* --- Wi-Fi --- */
  bool up = connectWiFi(USE_STATIC_IP);
  if (!up && USE_STATIC_IP) {
    Serial.println("Static IP failed. Retrying on DHCP -- CAM_IP is probably");
    Serial.println("on the wrong subnet for this network. Note the address");
    Serial.println("below and correct section 1, or just stay on DHCP.");
    up = connectWiFi(false);
  }

  if (!up) {
    Serial.println("\nWi-Fi FAILED.");
    Serial.println("  - is the network name spelled exactly right?");
    Serial.println("  - SSID and password are case sensitive");
    Serial.println("  - the network must be 2.4 GHz, not 5 GHz");
    Serial.println("  - a phone hotspot works too for a bench test");
    Serial.println("  Restarting in 5 s...");
    delay(5000);
    ESP.restart();
  }

  /* --- mDNS: http://robotcam.local/ without knowing the IP --- */
  if (MDNS.begin(MDNS_NAME)) {
    MDNS.addService("http", "tcp", 80);
    Serial.printf("mDNS up: http://%s.local/\n", MDNS_NAME);
  } else {
    Serial.println("mDNS failed to start (use the IP instead)");
  }

  String ip = WiFi.localIP().toString();
  Serial.println("\n-----------------------------------------------");
  Serial.printf("  Live view :  http://%s/\n", ip.c_str());
  Serial.printf("              http://%s.local/\n", MDNS_NAME);
  Serial.printf("  Snapshot  :  http://%s/capture\n", ip.c_str());
  Serial.printf("  Stream    :  http://%s:81/stream\n", ip.c_str());
  Serial.printf("  Status    :  http://%s/status\n", ip.c_str());
  Serial.printf("  Signal    :  %d dBm\n", (int)WiFi.RSSI());
  Serial.println("-----------------------------------------------");
  Serial.println("Open the live view URL on any phone on the same network.");

  startCameraServer();

  // Two quick flashes = up and serving.
  for (int i = 0; i < 2; i++) {
    digitalWrite(FLASH_GPIO_NUM, HIGH); delay(60);
    digitalWrite(FLASH_GPIO_NUM, LOW);  delay(180);
  }
}

/* ---------- 13. LOOP -------------------------------------------------------- */
void loop() {
  // The HTTP servers run in their own FreeRTOS tasks. All this loop does is
  // notice if Wi-Fi drops and get back on it.
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Wi-Fi dropped. Reconnecting...");
    if (!connectWiFi(USE_STATIC_IP)) {
      Serial.println("Reconnect failed. Restarting.");
      delay(1000);
      ESP.restart();
    }
    Serial.printf("Back up at %s\n", WiFi.localIP().toString().c_str());
    MDNS.end();
    MDNS.begin(MDNS_NAME);
    MDNS.addService("http", "tcp", 80);
  }
  delay(2000);
}
