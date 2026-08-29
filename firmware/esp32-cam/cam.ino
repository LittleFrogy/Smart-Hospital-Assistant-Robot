/* ============================================================================
   Smart Hospital Assistant Robot  --  ESP32-CAM firmware
   Board: AI-Thinker ESP32-CAM   |   Arduino core for ESP32
   ----------------------------------------------------------------------------
   WHAT THE CAMERA DOES
     - Connects to Wi-Fi with a fixed IP
     - Serves a single still JPEG at        http://<cam-ip>/capture
     - Serves an MJPEG live stream at       http://<cam-ip>:81/stream
     - The PC pulls /capture, decodes the QR with pyzbar, and drives the car.
       The camera itself does NO decoding.

   FLASHING (the board has NO USB port)
     Use a CP2102 USB-TTL adapter:
        CAM 5V   -> adapter 5V   (set adapter jumper to 5V)
        CAM GND  -> adapter GND
        CAM U0T  -> adapter RXD
        CAM U0R  -> adapter TXD
        CAM IO0  -> GND  (ONLY while flashing -- REMOVE afterwards)
     Board:  "AI Thinker ESP32-CAM"      Upload speed: 115200
     Press RST as the upload begins. REMOVE the IO0->GND jumper when done,
     then press RST again to run.

   HARDWARE NOTES
     - Runtime wiring is 5V and GND only. Everything else stays unconnected.
     - GPIO16 must NOT be connected (PSRAM chip select).
     - A 470uF capacitor across 5V/GND within 20mm of the module is mandatory,
       or the board brown-outs and reboot-loops when Wi-Fi transmits.
     - Mount rigidly, facing the QR codes, slightly downward. Motion blur is the
       top cause of decode failures -- the car must be fully stopped to scan.

   This sketch is the standard Espressif CameraWebServer trimmed to two
   endpoints (capture + stream) with a static IP added.
   ============================================================================ */

#include "esp_camera.h"
#include <WiFi.h>
#include "esp_http_server.h"

/* ---------- 1. USER CONFIG -------------------------------------------------- */
const char* WIFI_SSID = "";      // TODO: set your network name
const char* WIFI_PASS = "";      // TODO: set your password

// Static IP. MUST differ from car (.51) and dispenser (.52).
IPAddress CAM_IP  (192, 168, 0, 53);
IPAddress GATEWAY (192, 168, 0, 1);
IPAddress SUBNET  (255, 255, 255, 0);
IPAddress DNS1    (192, 168, 0, 1);

/* ---------- 2. AI-THINKER PIN MAP  (do not change for this board) ---------- */
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

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

/* ---------- 3. /capture  --  single JPEG ----------------------------------- */
static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) {
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  esp_camera_fb_return(fb);
  return res;
}

/* ---------- 4. /stream  --  MJPEG on port 81 ------------------------------- */
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

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) { res = ESP_FAIL; break; }

    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    if (res == ESP_OK) {
      size_t hlen = snprintf(part_buf, 64, STREAM_PART, fb->len);
      res = httpd_resp_send_chunk(req, part_buf, hlen);
    }
    if (res == ESP_OK)
      res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);

    esp_camera_fb_return(fb);
    if (res != ESP_OK) break;
  }
  return res;
}

/* ---------- 5. start the two HTTP servers ---------------------------------- */
void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t capture_uri = {
    .uri = "/capture", .method = HTTP_GET, .handler = capture_handler, .user_ctx = NULL
  };
  if (httpd_start(&camera_httpd, &config) == ESP_OK)
    httpd_register_uri_handler(camera_httpd, &capture_uri);

  config.server_port = 81;
  config.ctrl_port   = 81;
  httpd_uri_t stream_uri = {
    .uri = "/stream", .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL
  };
  if (httpd_start(&stream_httpd, &config) == ESP_OK)
    httpd_register_uri_handler(stream_httpd, &stream_uri);
}

/* ---------- 6. SETUP -------------------------------------------------------- */
void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(false);
  delay(300);

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
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = FRAMESIZE_VGA;   // 640x480 - good for QR at close range
  config.jpeg_quality = 12;              // lower number = better quality, more RAM
  config.fb_count     = 1;
  config.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  config.fb_location  = CAMERA_FB_IN_PSRAM;

  if (psramFound()) {
    config.jpeg_quality = 10;
    config.fb_count     = 2;
    config.grab_mode    = CAMERA_GRAB_LATEST;
  } else {
    // no PSRAM: fall back to a smaller frame so it still runs
    config.frame_size = FRAMESIZE_SVGA;
    config.fb_location = CAMERA_FB_IN_DRAM;
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed 0x%x. Check the ribbon cable.\n", err);
    delay(2000);
    ESP.restart();
  }

  // sensor tweaks that help QR decoding
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_brightness(s, 1);   // slightly brighter
    s->set_contrast(s, 1);     // a little more contrast for crisp edges
    s->set_saturation(s, 0);
  }

  WiFi.mode(WIFI_STA);
  WiFi.config(CAM_IP, GATEWAY, SUBNET, DNS1);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("WiFi connecting");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) {
    delay(300); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("\nCam ready.  Capture: http://");
    Serial.print(WiFi.localIP()); Serial.println("/capture");
    Serial.print("            Stream:  http://");
    Serial.print(WiFi.localIP()); Serial.println(":81/stream");
  } else {
    Serial.println("\nWiFi FAILED - check SSID/PASS/IP.");
  }

  startCameraServer();
}

void loop() {
  // everything is handled by the HTTP server tasks
  delay(1000);
}
