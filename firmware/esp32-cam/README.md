# ESP32-CAM firmware

Stock `CameraWebServer` example with three changes.

## Board settings

- Board: `AI Thinker ESP32-CAM`
- Upload speed: `115200`
- Frame size: `FRAMESIZE_VGA` (640×480) — higher is slower and no more accurate for QR at close range

## Changes to the stock example

1. Select the `CAMERA_MODEL_AI_THINKER` define
2. Set a static IP with `WiFi.config()` before `WiFi.begin()`
3. Set `FRAMESIZE_VGA` in `setup()` after `esp_camera_init()`

## Flashing

The board has **no USB port**. You need a CP2102 USB-TTL adapter.

| ESP32-CAM | CP2102 |
|---|---|
| `5V` | `5V` (set the adapter jumper to 5V) |
| `GND` | `GND` |
| `U0T` (GPIO1) | `RXD` |
| `U0R` (GPIO3) | `TXD` |
| `GPIO0` | `GND` — **remove after flashing** |

Press `RST` as the upload begins.

**If you leave GPIO0 grounded, the board sits in bootloader mode forever and appears dead.** This catches almost everyone once.

## Endpoints used by the PC

- `http://<cam-ip>/capture` — single JPEG, what the Python decoder pulls
- `http://<cam-ip>:81/stream` — MJPEG live view for the nurse fallback page

## Hardware notes

- **`GPIO16` must stay unconnected** — PSRAM chip select
- **470µF across `5V`/`GND` within 20mm of the module is mandatory.** Without it the board reboot-loops when Wi-Fi transmits, and looks exactly like a dead board.
- Mount rigidly, facing forward and slightly down. Motion blur is the top cause of QR decode failures.
